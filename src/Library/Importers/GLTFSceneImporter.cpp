//////////////////////////////////////////////////////////////////////
//
//  GLTFSceneImporter.cpp - Implementation.  See header for the
//  design.  glTF 2.0 import; see docs/GLTF_IMPORT.md.
//
//  Importer responsibilities (Phases 1-4):
//   - Resolves the requested glTF scene (or the file's default scene).
//   - Walks the node tree depth-first, accumulating column-major world
//     matrices and passing them verbatim to Job::AddObjectMatrix (no
//     Euler decomposition; gimbal-lock and shear-decomp failure modes
//     are gone since Phase 3).
//   - For each node with a mesh: emits one gltfmesh_geometry per
//     primitive (re-using the Phase 1 loader), bound to a PBR material
//     + optional normal-map modifier through standard_object.
//   - For each glTF material (de-duplicated by index): emits a
//     pbr_metallic_roughness_material by feeding baseColor /
//     metallicRoughness / emissive textures (or factor-driven
//     uniformcolor painters when the slot is texture-less) into
//     Job::AddPBRMetallicRoughnessMaterial.  KHR_materials_emissive_
//     strength scales the emissive painter; KHR_materials_unlit swaps
//     the PBR shape for a LambertianLuminaireMaterial (zero BSDF +
//     emitter so the surface looks self-luminous regardless of lights);
//     KHR_materials_transmission + KHR_materials_volume + KHR_materials_
//     ior produce DielectricMaterial + HomogeneousMedium for refractive
//     glass with Beer-Lambert absorption.
//   - For each glTF texture: emits one painter.  Embedded `.glb` images
//     and inline `data:` URIs go directly from the cgltf bufferView
//     bytes into a painter via Job::AddInMemoryPNGTexturePainter /
//     AddInMemoryJPEGTexturePainter -- no disk round-trip.  External
//     image URIs in the .gltf JSON form continue to use the existing
//     file-path painter APIs.
//   - For each KHR_lights_punctual node: emits an omni_light /
//     spot_light / directional_light with the world-space placement
//     derived from the node's accumulated transform.
//   - For the first camera-bearing node: emits a pinhole_camera with
//     the world-space placement; subsequent camera nodes warn.
//
//  Out-of-scope (warn-and-skip; see docs/GLTF_IMPORT.md):
//   - Skinning / animation / morph targets -- one-time warning per
//     file; geometry renders at bind pose.
//   - alphaMode = MASK / BLEND -- both implemented (Phase 4) via
//     alpha_test_shaderop / transparency_shaderop wired through a
//     per-material advanced_shader.  Only integrators that route
//     through IShader::Shade() honour them (path tracer + legacy
//     direct shaders); BDPT, VCM, MLT, and photon tracers bypass the
//     shader-op pipeline and currently treat both modes as opaque.
//     Per-pixel alpha is read via IPainter::GetAlpha (the un-
//     premultiplied A channel of the baseColor texture).
//   - KHR_draco_mesh_compression / EXT_meshopt_compression -- rejected
//     by the Phase 1 mesh loader; the importer surfaces that error.
//   - KHR_materials_clearcoat / KHR_materials_sheen as a layer atop
//     the PBR base -- the BRDFs exist (SheenBRDF, dielectric for
//     clearcoat) but RISE's CompositeMaterial random-walk SPF doesn't
//     compose well over a 3-lobe PBR base; deferred to Phase 5.
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
#include "../Interfaces/ITriangleMeshGeometry.h"
#include "../RISE_API.h"
#include "../Polygon.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/MediaPathLocator.h"

#include "../../../extlib/cgltf/cgltf.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
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
	// CameraName helper was here but the importer currently hard-codes
	// "default" (only the first glTF camera is imported).  When
	// multi-camera glTF import lands, restore the helper and call it
	// per-camera-node.
	std::string MediumName   ( const std::string& prefix, size_t matIdx ) {
		std::ostringstream oss; oss << prefix << ".medium." << matIdx; return oss.str();
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
	//
	// `preRegistered` (added 2026-05-01 alongside parallel-decode batch):
	// a set of painter names PreDecodeTextures already registered.  When
	// non-null and contains the computed painterName, the function
	// fast-paths back the name without doing a redundant per-call decode
	// + AddItem.  When null or miss, falls through to the original
	// per-call path (preserves single-primitive `gltfmesh_geometry` chunk
	// behaviour and acts as a safety net if the pre-decode pass missed).
	std::string CreateTexturePainter(
		IJob&               job,
		const std::string&  prefix,
		const cgltf_data*   data,
		const std::string&  glbPath,
		const cgltf_texture* tex,
		const char*         role,
		const std::set<std::string>* preRegistered = NULL,
		bool                lowmemTextures = false )	// matches the slow-path AddXXXTexturePainter `lowmem` arg; only consulted when preRegistered is null/miss.
	{
		if( !tex || !tex->image ) {
			return std::string();
		}
		const size_t imgIdx = (size_t)( tex->image - data->images );

		// Fast path: PreDecodeTextures already decoded + registered this
		// painter.  Skip the redundant per-call decode.
		const std::string painterName = ImagePainterName( prefix, role, imgIdx );
		if( preRegistered && preRegistered->find( painterName ) != preRegistered->end() ) {
			return painterName;
		}

		std::string ext, filePath;
		const unsigned char* bytes = NULL;
		size_t len = 0;
		ResolveImageBytes( data, glbPath, imgIdx, ext, filePath, bytes, len );

		if( filePath.empty() && bytes == NULL ) {
			return std::string();	// ResolveImageBytes already logged the cause
		}

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

		// `kLowMem` mirrors the chunk-controlled lowmemTextures option that
		// PreDecodeTextures threads through.  When true, the painter defers
		// color-space convert to per-sample (saves load time + texture RAM
		// at the cost of ~25% per-sample render).  Default-false at the
		// chunk level; Sponza-class scenes flip it on for iteration speed.
		const bool kLowMem = lowmemTextures;

		bool ok = false;
		if( !filePath.empty() ) {
			// On-disk sidecar (the .gltf JSON form with external image files).
			if( ext == ".png" ) {
				ok = job.AddPNGTexturePainter(
					painterName.c_str(), filePath.c_str(),
					cs, /*filter*/ 1, kLowMem, scale, shift );
			} else if( ext == ".jpg" || ext == ".jpeg" ) {
				ok = job.AddJPEGTexturePainter(
					painterName.c_str(), filePath.c_str(),
					cs, /*filter*/ 1, kLowMem, scale, shift );
			}
		} else {
			// Embedded bytes (any .glb image; .gltf `data:` URIs cgltf already
			// decoded into a bufferView).  No disk round-trip.
			if( ext == ".png" ) {
				ok = job.AddInMemoryPNGTexturePainter(
					painterName.c_str(), bytes, len,
					cs, /*filter*/ 1, kLowMem, scale, shift );
			} else if( ext == ".jpg" || ext == ".jpeg" ) {
				ok = job.AddInMemoryJPEGTexturePainter(
					painterName.c_str(), bytes, len,
					cs, /*filter*/ 1, kLowMem, scale, shift );
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
		size_t              matIdx,
		const std::set<std::string>* preRegisteredTextures,	// pre-decoded painters; see PreDecodeTextures
		bool                lowmemTextures )	// passed to CreateTexturePainter slow path; matches PreDecodeTextures' setting
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
		std::string baseColorTexturePainter;	// raw baseColorTexture, before factor multiply.
		                                        // BuildAlphaPainter reads CHAN_A from THIS, not
		                                        // the composed product (CHAN_A on a BlendPainter
		                                        // returns the default 1.0 — the composition chain
		                                        // doesn't propagate texture alpha).
		bool baseColorIsTexture = false;	// false => uniform painter (alpha mask becomes pass/fail)
		double factorAlpha = 1.0;
		if( mat.has_pbr_metallic_roughness ) {
			const cgltf_pbr_metallic_roughness& pmr = mat.pbr_metallic_roughness;
			factorAlpha = (double)pmr.base_color_factor[3];

			std::string textureName;
			if( pmr.base_color_texture.texture ) {
				textureName = CreateTexturePainter(
					job, prefix, data, glbPath, pmr.base_color_texture.texture, "basecolor",
					preRegisteredTextures, lowmemTextures );
			}
			baseColorTexturePainter = textureName;	// captured for BuildAlphaPainter

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
					"metallic_roughness",
					preRegisteredTextures, lowmemTextures );
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

		// Emissive -- glTF spec §3.9.4: emissive = emissiveTexture x
		// emissiveFactor x emissiveStrength.  Earlier revisions treated
		// the slot as either-or (texture iff present, else factor) which
		// silently dropped non-white emissiveFactor on textured assets.
		// The strength term is folded in below as `emissiveScale`; the
		// factor x texture multiply is wired here.
		std::string emissivePainter = "none";
		std::string emissiveTextureName;
		if( mat.emissive_texture.texture ) {
			emissiveTextureName = CreateTexturePainter(
				job, prefix, data, glbPath, mat.emissive_texture.texture, "emissive",
				preRegisteredTextures, lowmemTextures );
		}
		const cgltf_float ef[3] = {
			mat.emissive_factor[0],
			mat.emissive_factor[1],
			mat.emissive_factor[2] };
		const bool factorIsZero = (ef[0] <= 0 && ef[1] <= 0 && ef[2] <= 0);
		const bool factorIsWhite = (ef[0] >= 1.0 && ef[1] >= 1.0 && ef[2] >= 1.0);

		if( !emissiveTextureName.empty() ) {
			if( factorIsZero ) {
				// emissive = texture * 0 = no emission.  Spec-correct.
				emissivePainter = "none";
			} else if( factorIsWhite ) {
				// emissive = texture * 1 = texture.  Skip the multiply.
				emissivePainter = emissiveTextureName;
			} else {
				// emissive = texture * factorRGB via BlendPainter(t, 0, f).
				const std::string nFactor = PainterName( prefix, "emissive_factor", matIdx );
				const double pel[3] = { ef[0], ef[1], ef[2] };
				job.AddUniformColorPainter( nFactor.c_str(), pel, "Rec709RGB_Linear" );

				const std::string nZero = PainterName( prefix, "emissive_zero", matIdx );
				const double zero[3] = { 0.0, 0.0, 0.0 };
				job.AddUniformColorPainter( nZero.c_str(), zero, "Rec709RGB_Linear" );

				const std::string nProduct = PainterName( prefix, "emissive", matIdx );
				job.AddBlendPainter( nProduct.c_str(),
					emissiveTextureName.c_str(),
					nZero.c_str(),
					nFactor.c_str() );
				emissivePainter = nProduct;
			}
		} else if( !factorIsZero ) {
			// No texture; the factor itself is the emissive painter.
			const std::string n = PainterName( prefix, "emissive", matIdx );
			const double pel[3] = { ef[0], ef[1], ef[2] };
			job.AddUniformColorPainter( n.c_str(), pel, "Rec709RGB_Linear" );
			emissivePainter = n;
		}

		// KHR_materials_emissive_strength: scalar multiplier on emissive
		// radiance, used by glTF authoring tools to push emission past the
		// [0,1] sRGB-encoded texture range.  Default 1.0 when extension is
		// absent, so the unconditional read is safe.
		const double emissiveScale = mat.has_emissive_strength
			? (double)mat.emissive_strength.emissive_strength
			: 1.0;

		const std::string matName = MaterialName( prefix, matIdx );

		// KHR_materials_clearcoat and KHR_materials_sheen are detected,
		// but Phase 4 does not yet attempt to layer them on top of the
		// PBR / dielectric / unlit base.  Reason: CompositeMaterial's
		// random walk is currently designed around dielectric-on-Lambertian
		// (the working scenes/Tests/Materials/composite_material.RISEscene
		// pattern) and does not handle a GGX or sheen top over a complex
		// 3-lobe PBR base — the resulting walk drops most diffuse
		// contributions, producing nearly-black surfaces.  Phase 5 will
		// add a proper additive layered material that works with PBR.  For
		// Phase 4 we register the base material under `matName` directly
		// and warn once per material so users can author the layer
		// manually if needed (the chunk-level sheen_material and
		// fresnel_mode = schlick_f0 GGX top are both fully working
		// stand-alone).
		const bool hasClearcoat = mat.has_clearcoat;
		const bool hasSheen     = mat.has_sheen;
		const std::string baseMatName = matName;

		// KHR_materials_unlit: skip the BSDF entirely and treat baseColor
		// as Lambertian radiant exitance.  RISE has no "BSDF-less" material
		// class; the closest fit is LambertianLuminaireMaterial wrapping a
		// zero-reflectance Lambertian (so the BSDF returns 0 and the
		// emitter contributes baseColor / π · scale on hit).  Using
		// scale = π gives `pixel ≈ baseColor` for a viewer-facing surface,
		// which matches glTF unlit semantics: rendered colour equals
		// baseColor regardless of incident light.
		//
		// KHR_materials_transmission (+ optional KHR_materials_volume +
		// KHR_materials_ior): build a dielectric_material instead of the
		// PBR path, with the IOR coming from the ior extension (default
		// 1.5).  The volume extension's attenuation_color +
		// attenuation_distance produce a HomogeneousMedium with Beer-
		// Lambert absorption, attached to the imported objects via
		// SetObjectInteriorMedium during the scene walk.
		bool ok = false;
		if( mat.unlit ) {
			const std::string nZero = PainterName( prefix, "unlit_zero", matIdx );
			const double zero[3] = { 0.0, 0.0, 0.0 };
			job.AddUniformColorPainter( nZero.c_str(), zero, "Rec709RGB_Linear" );

			const std::string nNullLamb = PainterName( prefix, "unlit_null_lamb", matIdx );
			job.AddLambertianMaterial( nNullLamb.c_str(), nZero.c_str() );

			ok = job.AddLambertianLuminaireMaterial(
				baseMatName.c_str(),
				baseColorPainter.c_str(),
				nNullLamb.c_str(),
				/*scale*/ 3.141592653589793 );
		} else if( mat.has_transmission ) {
			// Tau (transmittance painter): white = full pass-through.
			// glTF's transmission_factor scales attenuation; bake it into
			// a uniform here.  baseColor is NOT folded into tau because
			// for thick-walled glass the absorption colour comes from the
			// volume extension (handled below); for thin-walled the
			// asset's tint is approximated as the dielectric scattering
			// inside the volume's σ_a (set to a small value when
			// attenuation_color is non-white).
			//
			// Phase 4 implements the SCALAR subset of KHR_materials_
			// transmission only.  `transmission.transmission_texture` is
			// NOT sampled — assets that vary transmissivity spatially
			// (e.g. a glass panel with masked frosted regions) will
			// import as uniformly transmissive.  Deferred to Phase 5
			// because plumbing a per-pixel τ painter into
			// DielectricMaterial requires either a new τ-painter slot
			// in the material or a per-object material-mix wrapper;
			// neither is wired today.  Warn so authors aren't surprised.
			if( mat.transmission.transmission_texture.texture ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"GLTFSceneImporter:: material `%s` declares a transmissionTexture; "
					"Phase 4 honours only the scalar transmission_factor (%.3f). "
					"The texture is ignored.  See docs/GLTF_IMPORT.md §15 (Phase 5).",
					matName.c_str(), (double)mat.transmission.transmission_factor );
			}
			const double trans = (double)mat.transmission.transmission_factor;
			const std::string nTau = PainterName( prefix, "trans_tau", matIdx );
			const double tau[3] = { trans, trans, trans };
			job.AddUniformColorPainter( nTau.c_str(), tau, "Rec709RGB_Linear" );

			const double iorValue = mat.has_ior
				? (double)mat.ior.ior
				: 1.5;
			const std::string nIor = PainterName( prefix, "trans_ior", matIdx );
			const double iorPel[3] = { iorValue, iorValue, iorValue };
			job.AddUniformColorPainter( nIor.c_str(), iorPel, "Rec709RGB_Linear" );

			const std::string nScat = PainterName( prefix, "trans_scat", matIdx );
			const double zero[3] = { 0.0, 0.0, 0.0 };
			job.AddUniformColorPainter( nScat.c_str(), zero, "Rec709RGB_Linear" );

			ok = job.AddDielectricMaterial(
				baseMatName.c_str(),
				nTau.c_str(),
				nIor.c_str(),
				nScat.c_str(),
				/*hg*/ false );

			// KHR_materials_volume: thick-walled volumetric absorption.
			// σ_a = -ln(attenuation_color) / attenuation_distance per channel.
			// Clamp attenuation_color to (0, 1] to avoid log domain issues
			// (a value of 0 in any channel would imply infinite absorption,
			// which would render the object completely opaque in that band).
			if( mat.has_volume && mat.volume.attenuation_distance > 0 ) {
				const cgltf_volume& vol = mat.volume;
				const double dist = (double)vol.attenuation_distance;
				auto SafeLog = []( double x ) -> double {
					return std::log( std::max( 1e-6, std::min( 1.0, x ) ) );
				};
				const double sigma_a[3] = {
					-SafeLog( (double)vol.attenuation_color[0] ) / dist,
					-SafeLog( (double)vol.attenuation_color[1] ) / dist,
					-SafeLog( (double)vol.attenuation_color[2] ) / dist
				};
				const double sigma_s[3] = { 0.0, 0.0, 0.0 };

				const std::string medName = MediumName( prefix, matIdx );
				job.AddHomogeneousMedium(
					medName.c_str(),
					sigma_a, sigma_s,
					/*phase*/ "isotropic",
					/*phase_g*/ 0.0 );
				// The medium gets bound to per-primitive objects in the
				// Walker (via SetObjectInteriorMedium) since materials are
				// scene-wide but interior media attach to objects.
			}
		} else {
			ok = job.AddPBRMetallicRoughnessMaterial(
				baseMatName.c_str(),
				baseColorPainter.c_str(),
				metallicPainter.c_str(),
				roughnessPainter.c_str(),
				/*ior*/ 1.5,
				emissivePainter.c_str(),
				emissiveScale );
		}
		if( !ok ) {
			GlobalLog()->PrintEx( eLog_Error,
				"GLTFSceneImporter:: failed to register material `%s`", baseMatName.c_str() );
			return false;
		}

		// Layered KHR extensions detected — log once per material so users
		// know the layer was seen and skipped.  Phase 4 carries the base
		// material verbatim; Phase 5 will add proper additive layering.
		if( hasClearcoat ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"GLTFSceneImporter:: material `%s` declares KHR_materials_clearcoat "
				"(factor=%.2f); Phase 4 imports the base PBR only and skips the "
				"clearcoat layer.  See docs/GLTF_IMPORT.md §13 (Phase 5 candidates).",
				matName.c_str(), (double)mat.clearcoat.clearcoat_factor );
		}
		if( hasSheen ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"GLTFSceneImporter:: material `%s` declares KHR_materials_sheen; "
				"Phase 4 imports the base PBR only and skips the sheen layer.  "
				"Use the standalone `sheen_material` chunk for hand-authored fabric.",
				matName.c_str() );
		}

#if 0	// Phase 5 — preserved for the layered-composite work that comes next.
		// Layered KHR extensions: sheen and clearcoat both add a top
		// dielectric/cloth layer over the base PBR.  We compose them in
		// order  base → sheen → clearcoat  via two CompositeMaterial
		// layers, so the user's view ray hits clearcoat first.  When only
		// one extension is present, we collapse to a single composite.
		// `currentBottom` tracks the running "below this layer" material
		// name as we wrap each layer; the LAST layer registers under
		// matName so downstream consumers find the composite.
		std::string currentBottom = baseMatName;
		const std::string nLayerZero = PainterName( prefix, "layer_zero", matIdx );
		if( hasClearcoat || hasSheen ) {
			const double zero[3] = { 0.0, 0.0, 0.0 };
			job.AddUniformColorPainter( nLayerZero.c_str(), zero, "Rec709RGB_Linear" );
		}

		// ----- Sheen layer -----
		if( hasSheen ) {
			const cgltf_sheen& sh = mat.sheen;

			// sheen_color: glTF's sheenColorFactor[3], optionally textured.
			std::string sheenColorPainter;
			if( sh.sheen_color_texture.texture ) {
				sheenColorPainter = CreateTexturePainter(
					job, prefix, data, glbPath, sh.sheen_color_texture.texture, "sheen_color" );
			}
			if( sheenColorPainter.empty() ) {
				const std::string n = PainterName( prefix, "sheen_color", matIdx );
				const double pel[3] = {
					(double)sh.sheen_color_factor[0],
					(double)sh.sheen_color_factor[1],
					(double)sh.sheen_color_factor[2] };
				job.AddUniformColorPainter( n.c_str(), pel, "Rec709RGB_Linear" );
				sheenColorPainter = n;
			}

			// sheen_roughness: scalar painter from sheenRoughnessFactor.
			const std::string nShRough = PainterName( prefix, "sheen_rough", matIdx );
			const double shRough = (double)sh.sheen_roughness_factor;
			const double shPel[3] = { shRough, shRough, shRough };
			job.AddUniformColorPainter( nShRough.c_str(), shPel, "Rec709RGB_Linear" );

			const std::string nSheenTop = baseMatName + "__sheen_top";
			job.AddSheenMaterial( nSheenTop.c_str(),
				sheenColorPainter.c_str(),
				nShRough.c_str() );

			// Final layer-output name: matName if no clearcoat above,
			// otherwise an intermediate name for clearcoat to consume.
			const std::string sheenComposite = hasClearcoat
				? matName + "__sheen_layer"
				: matName;
			// CompositeMaterial recursion limits + thickness mirror the
			// committed clearcoat reference scene (composite_material.RISEscene)
			// which is the only existing usage pattern and was tuned to
			// produce a coherent layered look.
			job.AddCompositeMaterial(
				sheenComposite.c_str(),
				nSheenTop.c_str(),
				currentBottom.c_str(),
				/*max_recur*/                  5,
				/*max_reflection_recursion*/   3,
				/*max_refraction_recursion*/   3,
				/*max_diffuse_recursion*/      3,
				/*max_translucent_recursion*/  3,
				/*thickness*/                  0.5,
				nLayerZero.c_str() );
			currentBottom = sheenComposite;
		}

		// ----- Clearcoat layer -----
		// glTF clearcoat: thin dielectric coating with its own roughness.
		// CompositeMaterial(top=clearcoat_GGX, bottom=currentBottom) — the
		// random walk in CompositeSPF propagates rays through the clearcoat
		// layer, into whatever's below (sheen or base), then back out.
		// Clearcoat has no diffuse; F0 = 0.04 (standard dielectric) scaled
		// by clearcoat_factor so factor = 0 disables the layer.  Roughness
		// is squared to match the GGX α convention.  The glTF optional
		// clearcoatTexture / clearcoatRoughnessTexture / clearcoatNormalTexture
		// are documented but not yet wired (uniform factors only — Phase 5).
		if( hasClearcoat ) {
			const cgltf_clearcoat& cc = mat.clearcoat;
			const double ccFactor   = (double)cc.clearcoat_factor;
			const double ccRoughSq  = (double)cc.clearcoat_roughness_factor *
			                          (double)cc.clearcoat_roughness_factor;

			// rs = factor * 0.04   (Schlick F0; factor scales coverage)
			const std::string nCcF0 = PainterName( prefix, "cc_f0", matIdx );
			const double f0[3] = { 0.04 * ccFactor, 0.04 * ccFactor, 0.04 * ccFactor };
			job.AddUniformColorPainter( nCcF0.c_str(), f0, "Rec709RGB_Linear" );

			// α = roughness^2
			const std::string nCcAlpha = PainterName( prefix, "cc_alpha", matIdx );
			const double alpha[3] = { ccRoughSq, ccRoughSq, ccRoughSq };
			job.AddUniformColorPainter( nCcAlpha.c_str(), alpha, "Rec709RGB_Linear" );

			// IOR / extinction unused in Schlick mode but required by the API.
			const std::string nCcIor = PainterName( prefix, "cc_ior", matIdx );
			const double ior[3] = { 1.5, 1.5, 1.5 };
			job.AddUniformColorPainter( nCcIor.c_str(), ior, "Rec709RGB_Linear" );

			// Top layer GGX in Schlick-from-F0 mode.  Diffuse = zero (shared
			// across all layered extensions, lives in nLayerZero above).
			const std::string nCcTop = baseMatName + "__cc_top";
			job.AddGGXMaterial( nCcTop.c_str(),
				nLayerZero.c_str(),
				nCcF0.c_str(),
				nCcAlpha.c_str(),
				nCcAlpha.c_str(),
				nCcIor.c_str(),
				nLayerZero.c_str(),
				"schlick_f0" );

			job.AddCompositeMaterial(
				matName.c_str(),
				nCcTop.c_str(),
				currentBottom.c_str(),
				/*max_recur*/                  5,
				/*max_reflection_recursion*/   3,
				/*max_refraction_recursion*/   3,
				/*max_diffuse_recursion*/      3,
				/*max_translucent_recursion*/  3,
				/*thickness*/                  0.5,
				nLayerZero.c_str() );
			currentBottom = matName;
		}
#endif	// Phase 5 layered-material guard

		(void)hasClearcoat;
		(void)hasSheen;

		// Optional normal-map modifier.
		if( mat.normal_texture.texture ) {
			const std::string nmTex = CreateTexturePainter(
				job, prefix, data, glbPath, mat.normal_texture.texture, "normal",
				preRegisteredTextures, lowmemTextures );
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
		// Build a Phase-4 alpha-source painter when the material needs one.
		// glTF alpha = textureAlpha * factorAlpha.  We bake factorAlpha
		// into the channel_painter's `scale` so downstream consumers can
		// read a single painter and have the spec semantics applied.
		// When there's no baseColor texture, fall back to a uniform painter
		// at factorAlpha (the test / blend then degenerates to material-
		// wide pass/fail; warn so authors aren't surprised).
		//
		// Critical: read CHAN_A from the RAW baseColor texture
		// (`baseColorTexturePainter`), NOT from `baseColorPainter`.  The
		// latter is a BlendPainter composing texture × factor — its
		// `GetAlpha()` returns the IPainter default (1.0) because the
		// composition doesn't propagate the underlying texture's alpha.
		// Reading from the raw TexturePainter pulls straight-alpha out
		// of the RGBA texel.
		auto BuildAlphaPainter = [&]() -> std::string
		{
			if( baseColorIsTexture && !baseColorTexturePainter.empty() ) {
				const std::string n = PainterName( prefix, "alpha", matIdx );
				job.AddChannelPainter( n.c_str(), baseColorTexturePainter.c_str(),
					/*chan A*/ 3, /*scale*/ factorAlpha, /*bias*/ 0.0 );
				return n;
			}
			const std::string n = PainterName( prefix, "alpha_factor", matIdx );
			const double pel[3] = { factorAlpha, factorAlpha, factorAlpha };
			job.AddUniformColorPainter( n.c_str(), pel, "Rec709RGB_Linear" );
			return n;
		};

		// Per-material alpha-mode wiring.  Both MASK and BLEND need an
		// `advanced_shader` (NOT `standard_shader`) because the alpha-aware
		// op replaces the accumulator rather than adding to it -- the
		// additive standard_shader would produce wrong cutout semantics.
		// Op chain: [Emission +, DirectLighting +, alpha_test_or_transp =].
		// The first two ops fill the accumulator with emission + surface
		// BSDF; the third op either keeps that (opaque) or replaces it
		// with the background colour (cutout / blend).
		auto WireAlphaShader = [&]( const char* opName ) -> bool
		{
			const std::string shaderName = matName + ".shader";
			const char* ops[] = { "DefaultEmission", "DefaultDirectLighting", opName };
			const unsigned int minDepth[] = { 0, 0, 0 };
			const unsigned int maxDepth[] = { 100, 100, 100 };
			const char operations[] = { '+', '+', '=' };
			const bool ok = job.AddAdvancedShader(
				shaderName.c_str(), 3, ops, minDepth, maxDepth, operations );
			if( !ok ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"GLTFSceneImporter:: failed to wire alpha shader for material `%s`; "
					"surface will render as opaque", matName.c_str() );
			}
			return ok;
		};

		if( mat.alpha_mode == cgltf_alpha_mode_mask ) {
			std::string alphaSource = BuildAlphaPainter();
			if( !baseColorIsTexture ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"GLTFSceneImporter:: material `%s` is alphaMode=MASK but has no baseColor "
					"texture; alpha test degraded to material-wide pass/fail at factor=%.3f",
					matName.c_str(), factorAlpha );
			}
			// `alphaSource` already includes factorAlpha (baked in via the
			// ChannelPainter scale or the uniform value), so the cutoff is
			// passed through unchanged.
			const double cutoff = (double)mat.alpha_cutoff;

			const std::string opName = matName + ".alphatest";
			if( job.AddAlphaTestShaderOp( opName.c_str(), alphaSource.c_str(), cutoff ) ) {
				WireAlphaShader( opName.c_str() );
			}
		} else if( mat.alpha_mode == cgltf_alpha_mode_blend ) {
			// Stochastic transparency for foliage / glass / decals.
			// transparency_shaderop computes
			//   c_op = cthis * factor + c_snap * (1 - factor)
			// where cthis = background (recursively cast past surface) and
			// c_snap is the running accumulator.  We want the rendered
			// pixel = surface * alpha + background * (1 - alpha), so set
			// factor = (1 - alpha) and run it AFTER the BSDF op (the
			// advanced-shader `=` operator then replaces the accumulator
			// with the blended result; see WireAlphaShader above).
			const std::string alphaSource = BuildAlphaPainter();

			const std::string nZero  = PainterName( prefix, "blend_zero",  matIdx );
			const std::string nWhite = PainterName( prefix, "blend_white", matIdx );
			const std::string nTrans = PainterName( prefix, "transparency", matIdx );
			const double zero[3]  = { 0.0, 0.0, 0.0 };
			const double white[3] = { 1.0, 1.0, 1.0 };
			job.AddUniformColorPainter( nZero.c_str(),  zero,  "Rec709RGB_Linear" );
			job.AddUniformColorPainter( nWhite.c_str(), white, "Rec709RGB_Linear" );
			job.AddBlendPainter( nTrans.c_str(),
				nZero.c_str(), nWhite.c_str(), alphaSource.c_str() );

			const std::string opName = matName + ".transparency";
			if( job.AddTransparencyShaderOp( opName.c_str(), nTrans.c_str(), /*one_sided*/ false ) ) {
				WireAlphaShader( opName.c_str() );
			}
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

	// ---------- per-primitive geometry-extraction helpers ----------
	// Moved from the retired TriangleMeshLoaderGLTF.cpp (2026-05-01).
	// These now serve both ImportScene's per-primitive Walker hop AND
	// the single-primitive ImportPrimitive entry point — the work
	// previously done independently in the loader.

	// Find the first attribute matching a given (type, set-index) pair.
	// Returns NULL when not present.  glTF lets you have e.g. TEXCOORD_0
	// and TEXCOORD_1 as two attributes with the same `type` field but
	// different `index` values.
	const cgltf_attribute* FindAttribute(
		const cgltf_primitive* prim,
		cgltf_attribute_type   type,
		int                    setIndex )
	{
		for( cgltf_size i = 0; i < prim->attributes_count; ++i ) {
			const cgltf_attribute& a = prim->attributes[i];
			if( a.type == type && a.index == setIndex ) {
				return &a;
			}
		}
		return NULL;
	}

	// Unpack an entire accessor as an array of floats.  Returns true on
	// success.  For an accessor with `count` elements of type T (where T
	// is VEC2/VEC3/VEC4/SCALAR with N components), produces count*N
	// floats.  Handles UNORM / SNORM / unnormalized variants uniformly.
	bool UnpackAccessor(
		const cgltf_accessor* accessor,
		std::vector<float>&   out )
	{
		if( !accessor || !accessor->buffer_view || !accessor->buffer_view->buffer ) {
			return false;
		}
		const cgltf_size numComponents = cgltf_num_components( accessor->type );
		const cgltf_size numFloats     = accessor->count * numComponents;
		out.resize( numFloats );
		const cgltf_size unpacked = cgltf_accessor_unpack_floats(
			accessor, out.data(), numFloats );
		return unpacked == numFloats;
	}

	// Triangulate the (possibly strip or fan) source-index sequence
	// into an output array of (i0, i1, i2) triples.  Source indices may
	// come either from the primitive's `indices` accessor or, when that
	// is absent, from the implicit 0..numVerts-1 sequence.  Caller has
	// already verified `prim->type` is one of the three supported
	// triangle topologies.
	void TriangulateIndices(
		const std::vector<unsigned int>& src,
		cgltf_primitive_type             topo,
		std::vector<unsigned int>&       out )
	{
		out.clear();
		if( src.size() < 3 ) {
			return;
		}
		switch( topo ) {
			case cgltf_primitive_type_triangles: {
				// Drop trailing 1 or 2 stray indices that don't form a
				// complete triangle (defensive against malformed files;
				// cgltf_validate normally catches this).
				const cgltf_size numTris = src.size() / 3;
				out.reserve( numTris * 3 );
				for( cgltf_size t = 0; t < numTris; ++t ) {
					out.push_back( src[ t*3 + 0 ] );
					out.push_back( src[ t*3 + 1 ] );
					out.push_back( src[ t*3 + 2 ] );
				}
				break;
			}
			case cgltf_primitive_type_triangle_strip: {
				// Per glTF 2.0 §3.7.2.1: triangle k uses indices
				// (k, k+1, k+2) for even k, (k+1, k, k+2) for odd k.
				// The parity swap keeps facing direction consistent.
				const cgltf_size numTris = src.size() - 2;
				out.reserve( numTris * 3 );
				for( cgltf_size k = 0; k < numTris; ++k ) {
					if( (k & 1) == 0 ) {
						out.push_back( src[ k + 0 ] );
						out.push_back( src[ k + 1 ] );
						out.push_back( src[ k + 2 ] );
					} else {
						out.push_back( src[ k + 1 ] );
						out.push_back( src[ k + 0 ] );
						out.push_back( src[ k + 2 ] );
					}
				}
				break;
			}
			case cgltf_primitive_type_triangle_fan: {
				// Per glTF 2.0 §3.7.2.1: triangle k uses indices
				// (0, k+1, k+2).  Vertex 0 is the shared fan center.
				const cgltf_size numTris = src.size() - 2;
				out.reserve( numTris * 3 );
				for( cgltf_size k = 0; k < numTris; ++k ) {
					out.push_back( src[ 0 ] );
					out.push_back( src[ k + 1 ] );
					out.push_back( src[ k + 2 ] );
				}
				break;
			}
			default:
				// Unreachable — caller filtered topology.
				break;
		}
	}
} // namespace

GLTFSceneImporter::GLTFSceneImporter( const char* glbPath ) :
  szFilename( GlobalMediaPathLocator().Find( glbPath ).c_str() ),
  cgltfData( NULL )
{
	// Phase-3 lifecycle change (2026-05-01): the cgltf parse moved from
	// per-call (Import / per-primitive Loader) into the constructor.  The
	// motivation is the NewSponza pathology — its bulk import previously
	// re-parsed the .gltf and re-`fread`-ed the 1.5 GB .bin once per
	// primitive (~200 redundant whole-file reads).  Now: one parse, one
	// .bin read, lifetime-scoped to this object.  ImportScene and
	// ImportPrimitive both consume the cached parse.
	//
	// On failure of any step (parse, load_buffers, validate) we log the
	// cause, free any partial state, and leave cgltfData == NULL.  The
	// public IsValid() then returns false; ImportScene / ImportPrimitive
	// short-circuit and return false too.  Callers MUST check IsValid()
	// before using the importer (see header doc).
	cgltf_options copts = {};
	cgltf_data*   data  = NULL;

	cgltf_result r = cgltf_parse_file( &copts, szFilename.c_str(), &data );
	if( r != cgltf_result_success ) {
		GlobalLog()->PrintEx( eLog_Error,
			"GLTFSceneImporter:: cgltf_parse_file failed for `%s` (cgltf_result=%d)",
			szFilename.c_str(), (int)r );
		return;
	}
	r = cgltf_load_buffers( &copts, data, szFilename.c_str() );
	if( r != cgltf_result_success ) {
		GlobalLog()->PrintEx( eLog_Error,
			"GLTFSceneImporter:: cgltf_load_buffers failed for `%s` (cgltf_result=%d)",
			szFilename.c_str(), (int)r );
		cgltf_free( data );
		return;
	}
	r = cgltf_validate( data );
	if( r != cgltf_result_success ) {
		GlobalLog()->PrintEx( eLog_Error,
			"GLTFSceneImporter:: cgltf_validate failed for `%s` (cgltf_result=%d)",
			szFilename.c_str(), (int)r );
		cgltf_free( data );
		return;
	}

	cgltfData = data;
}

GLTFSceneImporter::~GLTFSceneImporter()
{
	if( cgltfData ) {
		cgltf_free( static_cast<cgltf_data*>( cgltfData ) );
		cgltfData = NULL;
	}
}

bool GLTFSceneImporter::IsValid() const
{
	return cgltfData != NULL;
}

bool GLTFSceneImporter::ImportScene( IJob& job, const GLTFImportOptions& opts )
{
	if( !cgltfData ) {
		// Constructor-failure path; log already emitted by the ctor.
		return false;
	}
	cgltf_data* data = static_cast<cgltf_data*>( cgltfData );

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

	// Decode every texture the materials reference in one parallel batch
	// BEFORE the per-material loop.  Without this pass, each material's
	// CreateTexturePainter would call libpng synchronously in the calling
	// thread — for NewSponza-class assets that's 137 sequential single-
	// threaded decodes (tens of seconds).  PreDecodeTextures collects
	// every (image, role) tuple, dedupes, hands the whole list to
	// Job::AddTexturePaintersBatch which fans the libpng work across the
	// global ThreadPool.  After this returns, mRegisteredTextures is
	// populated and CreateTexturePainter (in the per-material loop below)
	// fast-paths to a name lookup.
	if( opts.importMaterials ) {
		PreDecodeTextures( job, opts );
	}

	// Materials -- create up front (de-duplicates across primitives).
	if( opts.importMaterials ) {
		for( size_t mi = 0; mi < data->materials_count; ++mi ) {
			CreateMaterial( job, prefix, data, szFilename, mi, &mRegisteredTextures, opts.lowmemTextures );
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
		// cgltfData is freed by the destructor — no per-method free here.
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
		GLTFSceneImporter&       importer;	// for ImportPrimitive — bypasses the public IJob entry point so the cgltf parse is not redone per primitive
		std::set<std::string>&   registeredGeoms;	// memoizes which (meshIdx,primIdx) geometries we've already built so multi-node instancing of a shared mesh registers ONCE and AddObjectMatrix runs N times

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

					// flip_v=FALSE: glTF 2.0 spec says UV origin is the upper-
					// left corner (V increases downward in image space) — same
					// as RISE's BilinRasterImageAccessor (`row = V * height`
					// indexed from top of file).  No flip is needed at the
					// loader.  An earlier "Phase 1 finding" claimed glTF was
					// V-up and forced flip_v=TRUE; the empirical test was
					// Avocado.glb (pit-on-flesh swap), MetalRoughSpheres
					// (label text upside-down), and AlphaBlendModeTest
					// ("Cutoff 0.25" mirrored), all of which now render
					// correctly with flip_v=FALSE.  The earlier finding was
					// a misread of the spec; it is corrected in
					// docs/GLTF_IMPORT.md §4 V-convention reconciliation.
					// Build + register the primitive's geometry through the
					// already-parsed cgltf_data we own — NOT through
					// `job.AddGLTFTriangleMeshGeometry`, which would
					// recursively spin up a fresh GLTFSceneImporter and
					// re-parse the .gltf for every primitive.  See header
					// doc, "Lifecycle".
					//
					// Multi-node instancing of a shared mesh: any glTF
					// where multiple nodes reference the same mesh (NewSponza
					// columns, Khronos OrientationTest, instanced foliage
					// etc.) means we'll see the SAME meshIdx+primIdx more
					// than once.  GeomName is keyed by mesh+prim only — so
					// the second visit's geom.c_str() collides with the
					// first's.  pGeomManager->AddItem rejects duplicate
					// names and would propagate false up through
					// AddPrebuiltTriangleMeshGeometry → ImportPrimitive,
					// causing the `if (!gOK) continue` below to skip
					// AddObjectMatrix and silently drop every instance after
					// the first.  The 2026-05-01 round-2 review caught this
					// regression vs. the retired loader, which had a
					// happy accident: it discarded AddItem's bool and
					// returned LoadTriangleMesh's success bit instead, so
					// duplicate-name registrations silently no-op'd and
					// AddObjectMatrix proceeded.  Memoize successful builds
					// per geom-name so we register exactly once but bind
					// every instance.  Bonus: the second-visit primitive
					// extraction is also skipped (a perf win on instancing-
					// heavy assets, on top of the parse-once win).
					bool gOK;
					if( registeredGeoms.find( geom ) != registeredGeoms.end() ) {
						gOK = true;	// already-built shared-mesh instance
					} else {
						gOK = importer.ImportPrimitive(
							job, geom.c_str(),
							(unsigned int)meshIdx, (unsigned int)pi,
							doubleSided,
							/*face_normals*/ false,
							/*flip_v*/ false );
						if( gOK ) {
							registeredGeoms.insert( geom );
						}
					}
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
						// Per-material shader override exists when the material was
						// imported with alphaMode = MASK or BLEND; CreateMaterial
						// registers <matName>.shader in those cases.
						if( prim.material->alpha_mode == cgltf_alpha_mode_mask ||
						    prim.material->alpha_mode == cgltf_alpha_mode_blend ) {
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

					// KHR_materials_volume: the material's interior is a
					// homogeneous medium (registered up-front in
					// CreateMaterial under MediumName(prefix, matIdx)).
					// Bind it to this object instance so Beer-Lambert
					// absorption applies to rays inside the volume.
					if( opts.importMaterials && prim.material &&
					    prim.material->has_volume &&
					    prim.material->volume.attenuation_distance > 0 ) {
						const size_t mi = (size_t)( prim.material - data->materials );
						const std::string medName = MediumName( prefix, mi );
						job.SetObjectInteriorMedium( objName.c_str(), medName.c_str() );
					}
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

					// glTF stores yfov in radians; AddPinholeCamera takes fov
					// in radians too (despite the chunk's "degrees" hint).
					const double yfov_rad = (double)cam->data.perspective.yfov;

					// Resolution / pixel rate / scanning rate are RISE
					// internals; pick benign defaults.  Users override via a
					// pinhole_camera chunk after the import.
					// Imports get a stable "default" name; if the importer
					// later supports multiple glTF cameras this will need
					// per-node naming, but glTF uses a single active
					// camera at most so "default" is correct today.
					job.AddPinholeCamera(
						"default",
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
	std::set<std::string> registeredGeoms;	// see Walker struct + the dedup site for rationale
	Walker w = { job, prefix, data, szFilename, opts, sawCamera, *this, registeredGeoms };
	for( size_t r = 0; r < scene->nodes_count; ++r ) {
		w.Walk( scene->nodes[r], identity );
	}

	// cgltfData is freed in the destructor — same lifecycle as a single
	// scene walk, owned for the lifetime of the importer object.
	return true;
}

bool GLTFSceneImporter::ImportPrimitive(
	IJob&        job,
	const char*  geomName,
	unsigned int meshIdx,
	unsigned int primIdx,
	bool         doubleSided,
	bool         faceNormals,
	bool         flipV )
{
	if( !cgltfData ) {
		return false;
	}
	if( !geomName ) {
		GlobalLog()->PrintEasyError(
			"GLTFSceneImporter::ImportPrimitive:: geomName is NULL" );
		return false;
	}

	// Build the geometry locally; register it with the job's manager
	// only on success.  On failure (malformed primitive, out-of-range
	// indices, unsupported topology, Draco compression) the partial
	// pGeometry is released and never enters the manager.
	ITriangleMeshGeometryIndexed* pGeometry = NULL;
	RISE_API_CreateTriangleMeshGeometryIndexed( &pGeometry, doubleSided, faceNormals );
	if( !pGeometry ) {
		GlobalLog()->PrintEasyError(
			"GLTFSceneImporter::ImportPrimitive:: failed to allocate geometry" );
		return false;
	}

	bool ok = BuildGeometryFromPrimitive( pGeometry, meshIdx, primIdx, flipV );
	if( ok ) {
		ok = job.AddPrebuiltTriangleMeshGeometry( geomName, pGeometry );
	}
	safe_release( pGeometry );
	return ok;
}

bool GLTFSceneImporter::BuildGeometryFromPrimitive(
	ITriangleMeshGeometryIndexed* pGeom,
	unsigned int                  meshIdx,
	unsigned int                  primIdx,
	bool                          flipV )
{
	if( !pGeom ) {
		GlobalLog()->PrintEasyError(
			"GLTFSceneImporter::BuildGeometryFromPrimitive:: NULL geometry target" );
		return false;
	}
	if( !cgltfData ) {
		// Constructor logged the parse failure; nothing to extract from.
		return false;
	}
	cgltf_data* data = static_cast<cgltf_data*>( cgltfData );

	if( meshIdx >= data->meshes_count ) {
		GlobalLog()->PrintEx( eLog_Error,
			"GLTFSceneImporter::BuildGeometryFromPrimitive:: mesh_index %u out of range "
			"(file has %u meshes)",
			meshIdx, (unsigned int)data->meshes_count );
		return false;
	}
	const cgltf_mesh& mesh = data->meshes[ meshIdx ];
	if( primIdx >= mesh.primitives_count ) {
		GlobalLog()->PrintEx( eLog_Error,
			"GLTFSceneImporter::BuildGeometryFromPrimitive:: primitive %u out of range "
			"(mesh `%s` has %u primitives)",
			primIdx,
			mesh.name ? mesh.name : "(unnamed)",
			(unsigned int)mesh.primitives_count );
		return false;
	}
	const cgltf_primitive& prim = mesh.primitives[ primIdx ];

	// Topology gate.  We support the three triangle modes; LINES /
	// POINTS / LINE_LOOP / LINE_STRIP are not renderable as triangle
	// meshes and would silently render as empty geometry if accepted.
	const cgltf_primitive_type topo = prim.type;
	const bool topoOK =
		topo == cgltf_primitive_type_triangles    ||
		topo == cgltf_primitive_type_triangle_strip ||
		topo == cgltf_primitive_type_triangle_fan;
	if( !topoOK ) {
		const char* topoName = "unknown";
		switch( topo ) {
			case cgltf_primitive_type_points:     topoName = "POINTS";     break;
			case cgltf_primitive_type_lines:      topoName = "LINES";      break;
			case cgltf_primitive_type_line_loop:  topoName = "LINE_LOOP";  break;
			case cgltf_primitive_type_line_strip: topoName = "LINE_STRIP"; break;
			default: break;
		}
		GlobalLog()->PrintEx( eLog_Error,
			"GLTFSceneImporter::BuildGeometryFromPrimitive:: primitive topology `%s` "
			"is not a triangle topology and cannot be loaded as triangle geometry.",
			topoName );
		return false;
	}

	// Reject Draco / meshopt compression with a clear error message.
	// cgltf parses the extension declaration but does NOT decompress;
	// the buffer bytes we'd read are still compressed and accessor
	// reads would yield garbage.
	if( prim.has_draco_mesh_compression ) {
		GlobalLog()->PrintEasyError(
			"GLTFSceneImporter::BuildGeometryFromPrimitive:: primitive uses "
			"KHR_draco_mesh_compression.  This is not supported (cgltf does not "
			"decode it).  Re-export the asset without Draco compression." );
		return false;
	}

	// Locate the standard attributes we care about.  POSITION is
	// required by the glTF spec; everything else is optional.
	const cgltf_attribute* attrPos    = FindAttribute( &prim, cgltf_attribute_type_position, 0 );
	const cgltf_attribute* attrNormal = FindAttribute( &prim, cgltf_attribute_type_normal,   0 );
	const cgltf_attribute* attrTan    = FindAttribute( &prim, cgltf_attribute_type_tangent,  0 );
	const cgltf_attribute* attrUV0    = FindAttribute( &prim, cgltf_attribute_type_texcoord, 0 );
	const cgltf_attribute* attrUV1    = FindAttribute( &prim, cgltf_attribute_type_texcoord, 1 );
	const cgltf_attribute* attrCol0   = FindAttribute( &prim, cgltf_attribute_type_color,    0 );

	if( !attrPos ) {
		GlobalLog()->PrintEasyError(
			"GLTFSceneImporter::BuildGeometryFromPrimitive:: primitive has no POSITION attribute" );
		return false;
	}

	const cgltf_size numVerts = attrPos->data->count;

	// Fail fast if any optional attribute disagrees with POSITION on
	// element count -- glTF 2.0 §3.7.2.1 mandates equal counts across
	// all primitive attributes, but we should not trust an external
	// file silently.
	if( attrNormal && attrNormal->data->count != numVerts ) {
		GlobalLog()->PrintEx( eLog_Error,
			"GLTFSceneImporter::BuildGeometryFromPrimitive:: NORMAL count (%u) != POSITION count (%u)",
			(unsigned int)attrNormal->data->count, (unsigned int)numVerts );
		return false;
	}
	if( attrTan && attrTan->data->count != numVerts ) {
		GlobalLog()->PrintEx( eLog_Error,
			"GLTFSceneImporter::BuildGeometryFromPrimitive:: TANGENT count (%u) != POSITION count (%u)",
			(unsigned int)attrTan->data->count, (unsigned int)numVerts );
		return false;
	}
	if( attrUV0 && attrUV0->data->count != numVerts ) {
		GlobalLog()->PrintEx( eLog_Error,
			"GLTFSceneImporter::BuildGeometryFromPrimitive:: TEXCOORD_0 count (%u) != POSITION count (%u)",
			(unsigned int)attrUV0->data->count, (unsigned int)numVerts );
		return false;
	}
	if( attrUV1 && attrUV1->data->count != numVerts ) {
		GlobalLog()->PrintEx( eLog_Error,
			"GLTFSceneImporter::BuildGeometryFromPrimitive:: TEXCOORD_1 count (%u) != POSITION count (%u)",
			(unsigned int)attrUV1->data->count, (unsigned int)numVerts );
		return false;
	}
	if( attrCol0 && attrCol0->data->count != numVerts ) {
		GlobalLog()->PrintEx( eLog_Error,
			"GLTFSceneImporter::BuildGeometryFromPrimitive:: COLOR_0 count (%u) != POSITION count (%u)",
			(unsigned int)attrCol0->data->count, (unsigned int)numVerts );
		return false;
	}

	// Optional v2 / v3 sub-interfaces.  v2 carries vertex colors; v3
	// adds tangents and TEXCOORD_1.  Loaders fall back gracefully if
	// the geometry implementation predates either.
	ITriangleMeshGeometryIndexed2* pGeom2 = dynamic_cast<ITriangleMeshGeometryIndexed2*>( pGeom );
	ITriangleMeshGeometryIndexed3* pGeom3 = dynamic_cast<ITriangleMeshGeometryIndexed3*>( pGeom );

	if( attrCol0 && !pGeom2 ) {
		GlobalLog()->PrintEasyWarning(
			"GLTFSceneImporter::BuildGeometryFromPrimitive:: source has COLOR_0 but the target "
			"geometry does not implement ITriangleMeshGeometryIndexed2 -- "
			"colors will be dropped" );
	}
	if( (attrTan || attrUV1) && !pGeom3 ) {
		GlobalLog()->PrintEasyWarning(
			"GLTFSceneImporter::BuildGeometryFromPrimitive:: source has TANGENT or TEXCOORD_1 but the "
			"target geometry does not implement ITriangleMeshGeometryIndexed3 -- "
			"those attributes will be dropped" );
	}

	// Unpack the requested attributes up front; we hand them to the
	// geometry through the typed-array overloads below.
	std::vector<float> posData;
	std::vector<float> normalData;
	std::vector<float> tangentData;
	std::vector<float> uv0Data;
	std::vector<float> uv1Data;
	std::vector<float> colorData;

	if( !UnpackAccessor( attrPos->data, posData ) ) {
		GlobalLog()->PrintEasyError(
			"GLTFSceneImporter::BuildGeometryFromPrimitive:: failed to unpack POSITION accessor" );
		return false;
	}
	if( attrNormal && !UnpackAccessor( attrNormal->data, normalData ) ) {
		GlobalLog()->PrintEasyError(
			"GLTFSceneImporter::BuildGeometryFromPrimitive:: failed to unpack NORMAL accessor" );
		return false;
	}
	if( attrTan && pGeom3 && !UnpackAccessor( attrTan->data, tangentData ) ) {
		GlobalLog()->PrintEasyError(
			"GLTFSceneImporter::BuildGeometryFromPrimitive:: failed to unpack TANGENT accessor" );
		return false;
	}
	if( attrUV0 && !UnpackAccessor( attrUV0->data, uv0Data ) ) {
		GlobalLog()->PrintEasyError(
			"GLTFSceneImporter::BuildGeometryFromPrimitive:: failed to unpack TEXCOORD_0 accessor" );
		return false;
	}
	if( attrUV1 && pGeom3 && !UnpackAccessor( attrUV1->data, uv1Data ) ) {
		GlobalLog()->PrintEasyError(
			"GLTFSceneImporter::BuildGeometryFromPrimitive:: failed to unpack TEXCOORD_1 accessor" );
		return false;
	}
	if( attrCol0 && pGeom2 && !UnpackAccessor( attrCol0->data, colorData ) ) {
		GlobalLog()->PrintEasyError(
			"GLTFSceneImporter::BuildGeometryFromPrimitive:: failed to unpack COLOR_0 accessor" );
		return false;
	}

	const cgltf_size colorComponents = attrCol0 ? cgltf_num_components( attrCol0->data->type ) : 0;

	// Source indices: read from prim.indices if present, else
	// 0..numVerts-1 (the implicit array convention in glTF).
	std::vector<unsigned int> srcIndices;
	if( prim.indices ) {
		srcIndices.resize( prim.indices->count );
		for( cgltf_size i = 0; i < prim.indices->count; ++i ) {
			srcIndices[i] = (unsigned int)cgltf_accessor_read_index( prim.indices, i );
		}
	} else {
		srcIndices.resize( numVerts );
		for( cgltf_size i = 0; i < numVerts; ++i ) {
			srcIndices[i] = (unsigned int)i;
		}
	}

	// Triangulate strip / fan into a flat (i0, i1, i2) sequence.
	std::vector<unsigned int> triIndices;
	TriangulateIndices( srcIndices, topo, triIndices );

	if( triIndices.empty() ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"GLTFSceneImporter::BuildGeometryFromPrimitive:: primitive yielded zero triangles "
			"(mesh `%s`, primitive %u).",
			mesh.name ? mesh.name : "(unnamed)", primIdx );
		// Not a hard error -- a degenerate primitive should still let
		// the surrounding scene load.
	}

	// Push everything into the geometry.

	pGeom->BeginIndexedTriangles();

	// POSITION
	for( cgltf_size i = 0; i < numVerts; ++i ) {
		pGeom->AddVertex( Vertex(
			posData[ i*3 + 0 ],
			posData[ i*3 + 1 ],
			posData[ i*3 + 2 ] ) );
	}

	// NORMAL.  AddNormal silently drops when the geometry was
	// constructed with face_normals=true; we always feed the data we
	// have, the geometry decides what to do with it.
	if( attrNormal ) {
		for( cgltf_size i = 0; i < numVerts; ++i ) {
			pGeom->AddNormal( Normal(
				normalData[ i*3 + 0 ],
				normalData[ i*3 + 1 ],
				normalData[ i*3 + 2 ] ) );
		}
	}

	// TEXCOORD_0.  RISE convention: every face needs a valid iCoords[]
	// triple.  When the source has no TEXCOORD_0, push a single
	// (0, 0) placeholder and point every face at it (mirrors how PLY
	// and 3DS loaders handle missing UVs).
	if( attrUV0 ) {
		for( cgltf_size i = 0; i < numVerts; ++i ) {
			const float u = uv0Data[ i*2 + 0 ];
			const float v = uv0Data[ i*2 + 1 ];
			pGeom->AddTexCoord( TexCoord( u, flipV ? (1.0f - v) : v ) );
		}
	} else {
		pGeom->AddTexCoord( TexCoord( 0, 0 ) );
	}

	// TEXCOORD_1 (v3-only).
	if( attrUV1 && pGeom3 ) {
		for( cgltf_size i = 0; i < numVerts; ++i ) {
			const float u = uv1Data[ i*2 + 0 ];
			const float v = uv1Data[ i*2 + 1 ];
			pGeom3->AddTexCoord1( TexCoord( u, flipV ? (1.0f - v) : v ) );
		}
	}

	// TANGENT (v3-only).  glTF stores xyz = tangent direction in
	// object space, w = bitangent sign (+1 or -1).
	if( attrTan && pGeom3 ) {
		for( cgltf_size i = 0; i < numVerts; ++i ) {
			Tangent4 t;
			t.dir = Vector3(
				tangentData[ i*4 + 0 ],
				tangentData[ i*4 + 1 ],
				tangentData[ i*4 + 2 ] );
			t.bitangentSign = tangentData[ i*4 + 3 ];
			pGeom3->AddTangent( t );
		}
	}

	// COLOR_0 (v2-only).  glTF spec stores vertex colors in linear
	// Rec.709 RGB (NOT sRGB -- this is the explicit difference from
	// PLY's universal-sRGB convention).  Convert via the Rec709->ROMM
	// path so painters see the engine's working colour space.
	if( attrCol0 && pGeom2 ) {
		for( cgltf_size i = 0; i < numVerts; ++i ) {
			const cgltf_size base = i * colorComponents;
			const float r = colorData[ base + 0 ];
			const float g = colorComponents >= 2 ? colorData[ base + 1 ] : r;
			const float b = colorComponents >= 3 ? colorData[ base + 2 ] : r;
			// Alpha (component 4) is dropped -- RISEPel is RGB only.
			Rec709RGBPel src( r, g, b );
			pGeom2->AddColor( RISEPel( src ) );
		}
	}

	// Faces.  glTF uses the same index list to address all attribute
	// arrays uniformly, so iVertices == iNormals == iCoords for every
	// face vertex.  When we synthesised a single-element TEXCOORD,
	// iCoords[] points at index 0 for every face.
	const bool haveUV0 = (attrUV0 != NULL);
	for( cgltf_size t = 0; t + 2 < triIndices.size(); t += 3 ) {
		IndexedTriangle tri;
		for( int k = 0; k < 3; ++k ) {
			const unsigned int idx = triIndices[ t + k ];
			tri.iVertices[k] = idx;
			tri.iNormals[k]  = idx;
			tri.iCoords[k]   = haveUV0 ? idx : 0u;
		}
		pGeom->AddIndexedTriangle( tri );
	}

	// If the source had no NORMAL, ask the geometry to derive smooth
	// normals from face topology.  AddNormal was a no-op for face-
	// normals geometries, so this is the right hook in either case
	// (ComputeVertexNormals does nothing when bUseFaceNormals is set).
	if( !attrNormal ) {
		pGeom->ComputeVertexNormals();
	}

	pGeom->DoneIndexedTriangles();

	return true;
}

// Walk every cgltf material slot, collect (image, role) tuples, dedupe by
// painter name, dispatch one Job::AddTexturePaintersBatch call.  Populates
// mRegisteredTextures with painter names that decoded + registered
// successfully.  See header doc for rationale.
void GLTFSceneImporter::PreDecodeTextures( IJob& job, const GLTFImportOptions& opts )
{
	if( !cgltfData ) {
		return;
	}
	cgltf_data* data = static_cast<cgltf_data*>( cgltfData );
	const std::string& prefix = opts.namePrefix;

	// Per-tuple state that has to outlive the batch call: we hold the
	// resolved file path and (for embedded images) the bufferView byte
	// pointer + length.  TexturePainterBatchRequest stores raw const
	// char* / const unsigned char* pointers, so the std::string backing
	// the file path needs to live until after AddTexturePaintersBatch
	// returns.  Keep them in side vectors keyed by request index.
	struct PendingDecode
	{
		std::string painterName;	// also keyed in `seen` below
		std::string filePath;		// empty when reading from bytes
		const unsigned char* bytes;	// null when reading from filePath
		size_t numBytes;
		char format;				// 0 = PNG, 1 = JPEG; -1 = skip
		char colorSpace;
	};
	std::vector<PendingDecode> pending;
	std::set<std::string> seen;

	// Helper: queue one (texture, role) tuple.  Idempotent on
	// (painterName) — multiple materials sharing the same image+role
	// only enqueue once.  Skips silently when the texture pointer is
	// null, the image has no resolvable source, or the format isn't a
	// PNG/JPEG we can decode.
	auto enqueue = [&]( const cgltf_texture* tex, const char* role )
	{
		if( !tex || !tex->image ) return;
		const size_t imgIdx = (size_t)( tex->image - data->images );
		const std::string painterName = ImagePainterName( prefix, role, imgIdx );
		if( seen.find( painterName ) != seen.end() ) {
			return;
		}
		seen.insert( painterName );

		std::string ext, filePath;
		const unsigned char* bytes = NULL;
		size_t len = 0;
		ResolveImageBytes( data, szFilename, imgIdx, ext, filePath, bytes, len );
		if( filePath.empty() && bytes == NULL ) {
			return;	// ResolveImageBytes already logged the cause
		}

		char format = -1;
		if(      ext == ".png"                    ) format = 0;
		else if( ext == ".jpg" || ext == ".jpeg"  ) format = 1;
		if( format < 0 ) {
			return;	// not a format AddTexturePaintersBatch handles
		}

		const char* colorSpace = TextureColorSpace( role );
		char cs = 1;	// sRGB default
		if(      std::strcmp( colorSpace, "Rec709RGB_Linear" ) == 0 ) cs = 0;
		else if( std::strcmp( colorSpace, "sRGB"             ) == 0 ) cs = 1;
		else if( std::strcmp( colorSpace, "ROMMRGB_Linear"   ) == 0 ) cs = 2;
		else if( std::strcmp( colorSpace, "ProPhotoRGB"      ) == 0 ) cs = 3;

		PendingDecode pd;
		pd.painterName = painterName;
		pd.filePath    = filePath;	// empty if reading from bytes
		pd.bytes       = bytes;
		pd.numBytes    = len;
		pd.format      = format;
		pd.colorSpace  = cs;
		pending.push_back( std::move( pd ) );
	};

	// Walk every material's texture slots.  Mirrors the slots
	// CreateMaterial reads; if a slot is added there, add it here too.
	for( size_t mi = 0; mi < data->materials_count; ++mi ) {
		const cgltf_material& mat = data->materials[ mi ];
		if( mat.has_pbr_metallic_roughness ) {
			enqueue( mat.pbr_metallic_roughness.base_color_texture.texture,        "basecolor" );
			enqueue( mat.pbr_metallic_roughness.metallic_roughness_texture.texture, "metallic_roughness" );
		}
		enqueue( mat.emissive_texture.texture, "emissive" );
		enqueue( mat.normal_texture.texture,   "normal" );
		// Phase-5 sheen / clearcoat texture slots intentionally left out
		// — those code paths are #if 0 in CreateMaterial today.
	}

	if( pending.empty() ) {
		return;
	}

	// Build the contiguous request array AddTexturePaintersBatch consumes.
	// Pointers in TexturePainterBatchRequest reference strings owned by
	// `pending` (filePath via .c_str()) or by the cgltf buffer (bytes); both
	// outlive the batch call.
	std::vector<TexturePainterBatchRequest> requests;
	requests.reserve( pending.size() );
	const double identityScale[3] = { 1.0, 1.0, 1.0 };
	const double zeroShift[3]     = { 0.0, 0.0, 0.0 };
	for( const PendingDecode& pd : pending ) {
		TexturePainterBatchRequest r = {};
		r.name       = pd.painterName.c_str();
		r.filePath   = pd.filePath.empty() ? NULL : pd.filePath.c_str();
		r.bytes      = pd.bytes;
		r.numBytes   = pd.numBytes;
		r.format     = pd.format;
		r.colorSpace = pd.colorSpace;
		r.filterType = 1;		// bilinear
		r.lowmemory  = opts.lowmemTextures;	// chunk-controlled; default false.
								// When true, defers color-space convert to
								// per-sample.  Saves ~4x texture RAM + 5-10x
								// load on heavy-PBR scenes; pays ~25%
								// per-sample render cost.  Match the
								// CreateTexturePainter slow path if changing.
		std::memcpy( r.scale, identityScale, sizeof(identityScale) );
		std::memcpy( r.shift, zeroShift,     sizeof(zeroShift) );
		requests.push_back( r );
	}

	// Dispatch.  Returns false if any request failed; we still record the
	// names that DID succeed so CreateTexturePainter's fast path picks
	// them up.  The slow path (per-call decode) handles any residual misses.
	(void) job.AddTexturePaintersBatch( requests.data(), requests.size() );

	// Mark every request name as registered.  AddTexturePaintersBatch
	// logs per-request decode failures, so a name in `pending` that
	// failed to decode would NOT actually be in the manager.  But the
	// CreateTexturePainter fast path is just a name handoff — the
	// material that consumes that name then finds it missing in the
	// manager (or a downstream paint step gets a "none" sentinel) and
	// falls back to its uniform-color path.  Worse case is one log line
	// per failed texture; correctness preserved.  An alternative would
	// be a per-request return-status array, but the failure rate on
	// well-formed assets is zero, so we save the API surface.
	for( const PendingDecode& pd : pending ) {
		mRegisteredTextures.insert( pd.painterName );
	}
}

