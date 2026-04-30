//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshLoaderGLTF.cpp - Implementation of the glTF 2.0
//  mesh loader.  Parses .gltf / .glb via cgltf and feeds a single
//  primitive of a single mesh into the v3 indexed triangle mesh
//  interface.  See docs/GLTF_IMPORT.md for the design plan.
//
//  Phase 1 scope: POSITION, NORMAL, TANGENT, TEXCOORD_0,
//  TEXCOORD_1, COLOR_0, indices.  Triangle topology only -- TRIANGLES,
//  TRIANGLE_STRIP, and TRIANGLE_FAN supported (the latter two get
//  triangulated at load time); LINES / POINTS / LOOP rejected.  Every
//  other attribute (JOINTS_n / WEIGHTS_n / COLOR_n / TEXCOORD_n>=2 /
//  underscore-prefixed extensions) is dropped silently with a debug
//  log entry; KHR_draco_mesh_compression / EXT_meshopt_compression
//  are rejected with a clear error since cgltf does not decode them.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 30, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TriangleMeshLoaderGLTF.h"
#include "../Interfaces/ITriangleMeshGeometry.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/MediaPathLocator.h"
#include "../Polygon.h"
#include "../Utilities/Color/Color.h"

#include <vector>
#include <string>

#include "../../../extlib/cgltf/cgltf.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
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

TriangleMeshLoaderGLTF::TriangleMeshLoaderGLTF(
	const char*	  szFile,
	unsigned int  meshIdx,
	unsigned int  primIdx,
	bool          flipV ) :
  szFilename( GlobalMediaPathLocator().Find( String( szFile ) ).c_str() ),
  meshIndex( meshIdx ),
  primitiveIndex( primIdx ),
  bFlipV( flipV )
{
}

TriangleMeshLoaderGLTF::~TriangleMeshLoaderGLTF()
{
}

bool TriangleMeshLoaderGLTF::LoadTriangleMesh( ITriangleMeshGeometryIndexed* pGeom )
{
	if( !pGeom ) {
		GlobalLog()->PrintEasyError( "TriangleMeshLoaderGLTF:: NULL geometry target" );
		return false;
	}

	cgltf_options opts = {};
	cgltf_data*   data = NULL;

	cgltf_result r = cgltf_parse_file( &opts, szFilename.c_str(), &data );
	if( r != cgltf_result_success ) {
		GlobalLog()->PrintEx( eLog_Error,
			"TriangleMeshLoaderGLTF:: cgltf_parse_file failed for `%s` (cgltf_result=%d)",
			szFilename.c_str(), (int)r );
		return false;
	}

	r = cgltf_load_buffers( &opts, data, szFilename.c_str() );
	if( r != cgltf_result_success ) {
		GlobalLog()->PrintEx( eLog_Error,
			"TriangleMeshLoaderGLTF:: cgltf_load_buffers failed for `%s` (cgltf_result=%d). "
			"Sidecar .bin file missing or unreadable?",
			szFilename.c_str(), (int)r );
		cgltf_free( data );
		return false;
	}

	r = cgltf_validate( data );
	if( r != cgltf_result_success ) {
		GlobalLog()->PrintEx( eLog_Error,
			"TriangleMeshLoaderGLTF:: cgltf_validate failed for `%s` (cgltf_result=%d). "
			"glTF file is malformed.",
			szFilename.c_str(), (int)r );
		cgltf_free( data );
		return false;
	}

	if( meshIndex >= data->meshes_count ) {
		GlobalLog()->PrintEx( eLog_Error,
			"TriangleMeshLoaderGLTF:: mesh_index %u out of range (file has %u meshes)",
			meshIndex, (unsigned int)data->meshes_count );
		cgltf_free( data );
		return false;
	}

	const cgltf_mesh& mesh = data->meshes[ meshIndex ];
	if( primitiveIndex >= mesh.primitives_count ) {
		GlobalLog()->PrintEx( eLog_Error,
			"TriangleMeshLoaderGLTF:: primitive %u out of range "
			"(mesh `%s` has %u primitives)",
			primitiveIndex,
			mesh.name ? mesh.name : "(unnamed)",
			(unsigned int)mesh.primitives_count );
		cgltf_free( data );
		return false;
	}

	const cgltf_primitive& prim = mesh.primitives[ primitiveIndex ];

	// Topology gate.  Phase 1 supports the three triangle modes; LINES /
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
			"TriangleMeshLoaderGLTF:: primitive topology `%s` is not a "
			"triangle topology and cannot be loaded as triangle geometry.",
			topoName );
		cgltf_free( data );
		return false;
	}

	// Reject Draco / meshopt compression with a clear error message.
	// cgltf parses the extension declaration but does NOT decompress;
	// the buffer bytes we'd read are still compressed and accessor
	// reads would yield garbage.
	if( prim.has_draco_mesh_compression ) {
		GlobalLog()->PrintEx( eLog_Error,
			"TriangleMeshLoaderGLTF:: primitive uses KHR_draco_mesh_compression. "
			"This is not supported in Phase 1 (cgltf does not decode it). "
			"Re-export the asset without Draco compression." );
		cgltf_free( data );
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
			"TriangleMeshLoaderGLTF:: primitive has no POSITION attribute" );
		cgltf_free( data );
		return false;
	}

	const cgltf_size numVerts = attrPos->data->count;

	// Fail fast if any optional attribute disagrees with POSITION on
	// element count -- glTF 2.0 §3.7.2.1 mandates equal counts across
	// all primitive attributes, but we should not trust an external
	// file silently.
	if( attrNormal && attrNormal->data->count != numVerts ) {
		GlobalLog()->PrintEx( eLog_Error,
			"TriangleMeshLoaderGLTF:: NORMAL count (%u) != POSITION count (%u)",
			(unsigned int)attrNormal->data->count, (unsigned int)numVerts );
		cgltf_free( data );
		return false;
	}
	if( attrTan && attrTan->data->count != numVerts ) {
		GlobalLog()->PrintEx( eLog_Error,
			"TriangleMeshLoaderGLTF:: TANGENT count (%u) != POSITION count (%u)",
			(unsigned int)attrTan->data->count, (unsigned int)numVerts );
		cgltf_free( data );
		return false;
	}
	if( attrUV0 && attrUV0->data->count != numVerts ) {
		GlobalLog()->PrintEx( eLog_Error,
			"TriangleMeshLoaderGLTF:: TEXCOORD_0 count (%u) != POSITION count (%u)",
			(unsigned int)attrUV0->data->count, (unsigned int)numVerts );
		cgltf_free( data );
		return false;
	}
	if( attrUV1 && attrUV1->data->count != numVerts ) {
		GlobalLog()->PrintEx( eLog_Error,
			"TriangleMeshLoaderGLTF:: TEXCOORD_1 count (%u) != POSITION count (%u)",
			(unsigned int)attrUV1->data->count, (unsigned int)numVerts );
		cgltf_free( data );
		return false;
	}
	if( attrCol0 && attrCol0->data->count != numVerts ) {
		GlobalLog()->PrintEx( eLog_Error,
			"TriangleMeshLoaderGLTF:: COLOR_0 count (%u) != POSITION count (%u)",
			(unsigned int)attrCol0->data->count, (unsigned int)numVerts );
		cgltf_free( data );
		return false;
	}

	// Optional v2 / v3 sub-interfaces.  v2 carries vertex colors; v3
	// adds tangents and TEXCOORD_1.  Loaders fall back gracefully if
	// the geometry implementation predates either.
	ITriangleMeshGeometryIndexed2* pGeom2 = dynamic_cast<ITriangleMeshGeometryIndexed2*>( pGeom );
	ITriangleMeshGeometryIndexed3* pGeom3 = dynamic_cast<ITriangleMeshGeometryIndexed3*>( pGeom );

	if( attrCol0 && !pGeom2 ) {
		GlobalLog()->PrintEasyWarning(
			"TriangleMeshLoaderGLTF:: source has COLOR_0 but the target "
			"geometry does not implement ITriangleMeshGeometryIndexed2 -- "
			"colors will be dropped" );
	}
	if( (attrTan || attrUV1) && !pGeom3 ) {
		GlobalLog()->PrintEasyWarning(
			"TriangleMeshLoaderGLTF:: source has TANGENT or TEXCOORD_1 but the "
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
		GlobalLog()->PrintEasyError( "TriangleMeshLoaderGLTF:: failed to unpack POSITION accessor" );
		cgltf_free( data );
		return false;
	}
	if( attrNormal && !UnpackAccessor( attrNormal->data, normalData ) ) {
		GlobalLog()->PrintEasyError( "TriangleMeshLoaderGLTF:: failed to unpack NORMAL accessor" );
		cgltf_free( data );
		return false;
	}
	if( attrTan && pGeom3 && !UnpackAccessor( attrTan->data, tangentData ) ) {
		GlobalLog()->PrintEasyError( "TriangleMeshLoaderGLTF:: failed to unpack TANGENT accessor" );
		cgltf_free( data );
		return false;
	}
	if( attrUV0 && !UnpackAccessor( attrUV0->data, uv0Data ) ) {
		GlobalLog()->PrintEasyError( "TriangleMeshLoaderGLTF:: failed to unpack TEXCOORD_0 accessor" );
		cgltf_free( data );
		return false;
	}
	if( attrUV1 && pGeom3 && !UnpackAccessor( attrUV1->data, uv1Data ) ) {
		GlobalLog()->PrintEasyError( "TriangleMeshLoaderGLTF:: failed to unpack TEXCOORD_1 accessor" );
		cgltf_free( data );
		return false;
	}
	if( attrCol0 && pGeom2 && !UnpackAccessor( attrCol0->data, colorData ) ) {
		GlobalLog()->PrintEasyError( "TriangleMeshLoaderGLTF:: failed to unpack COLOR_0 accessor" );
		cgltf_free( data );
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
			"TriangleMeshLoaderGLTF:: primitive yielded zero triangles "
			"(mesh `%s`, primitive %u).",
			mesh.name ? mesh.name : "(unnamed)", primitiveIndex );
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
			pGeom->AddTexCoord( TexCoord( u, bFlipV ? (1.0f - v) : v ) );
		}
	} else {
		pGeom->AddTexCoord( TexCoord( 0, 0 ) );
	}

	// TEXCOORD_1 (v3-only).
	if( attrUV1 && pGeom3 ) {
		for( cgltf_size i = 0; i < numVerts; ++i ) {
			const float u = uv1Data[ i*2 + 0 ];
			const float v = uv1Data[ i*2 + 1 ];
			pGeom3->AddTexCoord1( TexCoord( u, bFlipV ? (1.0f - v) : v ) );
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

	cgltf_free( data );
	return true;
}
