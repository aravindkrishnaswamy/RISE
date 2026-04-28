//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshLoaderPLY.cpp - Implementation of the PLY mesh loader
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 26, 2004
//  Tabs: 4
//  Comments:
//
//  Schema-driven property parser added 2026-04-28: previously this
//  loader assumed exactly 3 floats per vertex and 1 byte (binary) /
//  one count token (ASCII) + N uint indices per face.  Any PLY with
//  declared extra properties on the vertex element (normals, colors,
//  etc.) silently corrupted reads.  The header is now parsed into a
//  list of elements with typed property schemas, and the data section
//  is read per schema — extra properties advance the file pointer
//  correctly even when this loader has no use for them.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TriangleMeshLoaderPLY.h"
#include "GeometryUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/MediaPathLocator.h"
#include <stdio.h>
#include <ctype.h>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	enum PlyType {
		PLY_INVALID = 0,
		PLY_INT8,
		PLY_UINT8,
		PLY_INT16,
		PLY_UINT16,
		PLY_INT32,
		PLY_UINT32,
		PLY_FLOAT32,
		PLY_FLOAT64
	};

	struct PlyProperty
	{
		std::string	name;
		PlyType		type;		// for scalars: the value type; for lists: the element type
		PlyType		countType;	// PLY_INVALID for scalar; otherwise the list-count type
		bool isList() const { return countType != PLY_INVALID; }
	};

	struct PlyElement
	{
		std::string					name;
		unsigned int				count;
		std::vector<PlyProperty>	properties;
	};

	PlyType ParsePlyType( const char* s )
	{
		if( !s ) return PLY_INVALID;
		if( !strcmp( s, "char"   ) || !strcmp( s, "int8"    ) ) return PLY_INT8;
		if( !strcmp( s, "uchar"  ) || !strcmp( s, "uint8"   ) ) return PLY_UINT8;
		if( !strcmp( s, "short"  ) || !strcmp( s, "int16"   ) ) return PLY_INT16;
		if( !strcmp( s, "ushort" ) || !strcmp( s, "uint16"  ) ) return PLY_UINT16;
		if( !strcmp( s, "int"    ) || !strcmp( s, "int32"   ) ) return PLY_INT32;
		if( !strcmp( s, "uint"   ) || !strcmp( s, "uint32"  ) ) return PLY_UINT32;
		if( !strcmp( s, "float"  ) || !strcmp( s, "float32" ) ) return PLY_FLOAT32;
		if( !strcmp( s, "double" ) || !strcmp( s, "float64" ) ) return PLY_FLOAT64;
		return PLY_INVALID;
	}

	unsigned int PlyTypeSize( PlyType t )
	{
		switch( t ) {
			case PLY_INT8:    case PLY_UINT8:                  return 1;
			case PLY_INT16:   case PLY_UINT16:                 return 2;
			case PLY_INT32:   case PLY_UINT32: case PLY_FLOAT32: return 4;
			case PLY_FLOAT64:                                  return 8;
			default:                                           return 0;
		}
	}

	inline void FlipBytes16( void* p )
	{
		unsigned char* b = (unsigned char*)p;
		unsigned char t = b[0]; b[0] = b[1]; b[1] = t;
	}

	inline void FlipBytes32( void* p )
	{
		unsigned char* b = (unsigned char*)p;
		unsigned char t;
		t = b[0]; b[0] = b[3]; b[3] = t;
		t = b[1]; b[1] = b[2]; b[2] = t;
	}

	inline void FlipBytes64( void* p )
	{
		unsigned char* b = (unsigned char*)p;
		unsigned char t;
		t = b[0]; b[0] = b[7]; b[7] = t;
		t = b[1]; b[1] = b[6]; b[6] = t;
		t = b[2]; b[2] = b[5]; b[5] = t;
		t = b[3]; b[3] = b[4]; b[4] = t;
	}

	// Read one scalar value of the given PLY type from a binary stream.
	// Returns the value as a double (sufficient range for all PLY scalar
	// types except possibly the full uint32 range, which fits losslessly
	// in double's 53-bit mantissa for values < 2^32).
	bool ReadBinaryScalar( FILE* f, PlyType t, bool flip, double& out )
	{
		switch( t ) {
			case PLY_INT8: {
				signed char v;
				if( fread( &v, 1, 1, f ) != 1 ) return false;
				out = (double)v; return true;
			}
			case PLY_UINT8: {
				unsigned char v;
				if( fread( &v, 1, 1, f ) != 1 ) return false;
				out = (double)v; return true;
			}
			case PLY_INT16: {
				short v;
				if( fread( &v, 2, 1, f ) != 1 ) return false;
				if( flip ) FlipBytes16( &v );
				out = (double)v; return true;
			}
			case PLY_UINT16: {
				unsigned short v;
				if( fread( &v, 2, 1, f ) != 1 ) return false;
				if( flip ) FlipBytes16( &v );
				out = (double)v; return true;
			}
			case PLY_INT32: {
				int v;
				if( fread( &v, 4, 1, f ) != 1 ) return false;
				if( flip ) FlipBytes32( &v );
				out = (double)v; return true;
			}
			case PLY_UINT32: {
				unsigned int v;
				if( fread( &v, 4, 1, f ) != 1 ) return false;
				if( flip ) FlipBytes32( &v );
				out = (double)v; return true;
			}
			case PLY_FLOAT32: {
				float v;
				if( fread( &v, 4, 1, f ) != 1 ) return false;
				if( flip ) FlipBytes32( &v );
				out = (double)v; return true;
			}
			case PLY_FLOAT64: {
				double v;
				if( fread( &v, 8, 1, f ) != 1 ) return false;
				if( flip ) FlipBytes64( &v );
				out = v; return true;
			}
			default:
				return false;
		}
	}

	// Skip exactly the bytes occupied by one scalar value of the given
	// type (binary path).  Used when a property is declared but unused.
	bool SkipBinaryScalar( FILE* f, PlyType t )
	{
		const unsigned int n = PlyTypeSize( t );
		if( n == 0 ) return false;
		return fseek( f, (long)n, SEEK_CUR ) == 0;
	}

	// Tokenize a whitespace-separated line in place.  Modifies `line`
	// (writes NUL terminators at separator positions) and returns
	// pointers to the start of each token.  Empty lines yield no tokens.
	void TokenizeLine( char* line, std::vector<char*>& tokens )
	{
		tokens.clear();
		char* p = line;
		while( *p ) {
			while( *p && isspace( (unsigned char)*p ) ) ++p;
			if( !*p ) break;
			tokens.push_back( p );
			while( *p && !isspace( (unsigned char)*p ) ) ++p;
			if( *p ) { *p = '\0'; ++p; }
		}
	}

	// Convert one ASCII token into a numeric value, interpreted per the
	// declared PLY type.  Returns false on parse failure.  All paths
	// yield a double (same rationale as ReadBinaryScalar).
	bool ParseAsciiScalar( const char* tok, PlyType t, double& out )
	{
		if( !tok || !*tok ) return false;
		switch( t ) {
			case PLY_INT8: case PLY_INT16: case PLY_INT32: {
				long v;
				if( sscanf( tok, "%ld", &v ) != 1 ) return false;
				out = (double)v; return true;
			}
			case PLY_UINT8: case PLY_UINT16: case PLY_UINT32: {
				unsigned long v;
				if( sscanf( tok, "%lu", &v ) != 1 ) return false;
				out = (double)v; return true;
			}
			case PLY_FLOAT32: case PLY_FLOAT64: {
				double v;
				if( sscanf( tok, "%lf", &v ) != 1 ) return false;
				out = v; return true;
			}
			default:
				return false;
		}
	}

	bool IsFaceListProperty( const PlyProperty& p )
	{
		// The vertex-index list goes by both names depending on PLY
		// vintage: "vertex_indices" (PCL, Blender, modern) and
		// "vertex_index" (the original Stanford spec).  Accept both.
		return p.isList() &&
			( p.name == "vertex_indices" || p.name == "vertex_index" );
	}

	// Normalize a raw PLY channel value to the [0, 1] range used by
	// sRGBPel / Rec709RGBPel.  Integer types are scaled by their full
	// range; floating types are assumed to already be in [0, 1].
	inline double NormalizePlyChannelValue( double raw, PlyType t )
	{
		switch( t ) {
			case PLY_INT8:   case PLY_UINT8:  return raw / 255.0;
			case PLY_INT16:  case PLY_UINT16: return raw / 65535.0;
			case PLY_INT32:  case PLY_UINT32: return raw / 4294967295.0;
			case PLY_FLOAT32:
			case PLY_FLOAT64:
			default:                          return raw;
		}
	}
}

TriangleMeshLoaderPLY::TriangleMeshLoaderPLY( const char * szFile, const bool bInvertFaces_ ) :
  bInvertFaces( bInvertFaces_ )
{
	strncpy( szFilename, GlobalMediaPathLocator().Find(szFile).c_str(), 256 );
}

TriangleMeshLoaderPLY::~TriangleMeshLoaderPLY( )
{
}

// Parse the PLY header into a list of typed elements.  Stops after
// `end_header`.  Sets bAscii / bBigEndian.  Returns false if the
// header is malformed.
static bool ParsePlyHeader(
	FILE* inputFile,
	std::vector<PlyElement>& elements,
	bool& bAscii,
	bool& bBigEndian )
{
	char line[4096];
	bool sawFormat = false;
	bAscii = true;
	bBigEndian = false;

	while( fgets( line, sizeof(line), inputFile ) != NULL ) {

		// Trim trailing newline / carriage return for clean strcmp().
		size_t len = strlen( line );
		while( len > 0 && (line[len-1] == '\n' || line[len-1] == '\r') ) {
			line[--len] = '\0';
		}

		// Skip comments and obj_info lines.
		if( !strncmp( line, "comment",  7 ) ) continue;
		if( !strncmp( line, "obj_info", 8 ) ) continue;
		if( len == 0 ) continue;

		if( !strcmp( line, "end_header" ) ) {
			break;
		}

		char tok0[64] = {0}, tok1[64] = {0}, tok2[64] = {0}, tok3[64] = {0}, tok4[64] = {0};
		int n = sscanf( line, "%63s %63s %63s %63s %63s", tok0, tok1, tok2, tok3, tok4 );

		if( n >= 2 && !strcmp( tok0, "format" ) ) {
			sawFormat = true;
			if(      !strcmp( tok1, "ascii" ) )                bAscii = true;
			else if( !strcmp( tok1, "binary_big_endian" ) )  { bAscii = false; bBigEndian = true;  }
			else if( !strcmp( tok1, "binary_little_endian")) { bAscii = false; bBigEndian = false; }
			else {
				GlobalLog()->PrintEx( eLog_Error, "TriangleMeshLoaderPLY:: Unknown format '%s'", tok1 );
				return false;
			}

		} else if( n >= 3 && !strcmp( tok0, "element" ) ) {
			PlyElement e;
			e.name  = tok1;
			e.count = (unsigned int)strtoul( tok2, NULL, 10 );
			elements.push_back( e );

		} else if( n >= 3 && !strcmp( tok0, "property" ) ) {
			if( elements.empty() ) {
				GlobalLog()->PrintEasyError( "TriangleMeshLoaderPLY:: 'property' before any 'element'" );
				return false;
			}
			PlyProperty p;
			if( !strcmp( tok1, "list" ) ) {
				if( n < 5 ) {
					GlobalLog()->PrintEasyError( "TriangleMeshLoaderPLY:: malformed list property" );
					return false;
				}
				p.countType = ParsePlyType( tok2 );
				p.type      = ParsePlyType( tok3 );
				p.name      = tok4;
				if( p.countType == PLY_INVALID || p.type == PLY_INVALID ) {
					GlobalLog()->PrintEx( eLog_Error,
						"TriangleMeshLoaderPLY:: unknown list type in 'property list %s %s %s'",
						tok2, tok3, tok4 );
					return false;
				}
			} else {
				p.countType = PLY_INVALID;
				p.type      = ParsePlyType( tok1 );
				p.name      = tok2;
				if( p.type == PLY_INVALID ) {
					GlobalLog()->PrintEx( eLog_Error,
						"TriangleMeshLoaderPLY:: unknown scalar type '%s' for property '%s'",
						tok1, tok2 );
					return false;
				}
			}
			elements.back().properties.push_back( p );

		} else if( !strcmp( tok0, "ply" ) ) {
			// "ply" magic may appear inside the header on some emitters;
			// already validated by the caller — ignore here.
			continue;

		} else {
			// Unknown header keyword — warn but continue.
			GlobalLog()->PrintEx( eLog_Warning,
				"TriangleMeshLoaderPLY:: ignoring unknown header line '%s'", line );
		}
	}

	if( !sawFormat ) {
		GlobalLog()->PrintEasyError( "TriangleMeshLoaderPLY:: header had no 'format' line" );
		return false;
	}

	return true;
}

// Find vertex / face elements and their relevant property indices.
// Returns -1 (size_t max) if the element is missing.
static const size_t kPlyMissing = (size_t)-1;

struct PlyVertexLayout
{
	size_t elementIndex;	// index into elements vector
	size_t ix, iy, iz;		// property indices for x, y, z
	size_t ir, ig, ib;		// property indices for red/green/blue (if present)
	size_t ia;				// property index for alpha (if present); ignored, only consumed
	bool hasColor() const { return ir != kPlyMissing && ig != kPlyMissing && ib != kPlyMissing; }
};

struct PlyFaceLayout
{
	size_t elementIndex;
	size_t iIndices;		// property index of the vertex_indices list
};

static bool LocateVertexElement( const std::vector<PlyElement>& elements, PlyVertexLayout& out )
{
	out.elementIndex = kPlyMissing;
	out.ix = out.iy = out.iz = kPlyMissing;
	out.ir = out.ig = out.ib = out.ia = kPlyMissing;
	for( size_t e = 0; e < elements.size(); ++e ) {
		if( elements[e].name == "vertex" ) {
			out.elementIndex = e;
			const std::vector<PlyProperty>& props = elements[e].properties;
			for( size_t p = 0; p < props.size(); ++p ) {
				if( !props[p].isList() ) {
					if(      props[p].name == "x"     ) out.ix = p;
					else if( props[p].name == "y"     ) out.iy = p;
					else if( props[p].name == "z"     ) out.iz = p;
					else if( props[p].name == "red"   ) out.ir = p;
					else if( props[p].name == "green" ) out.ig = p;
					else if( props[p].name == "blue"  ) out.ib = p;
					else if( props[p].name == "alpha" ) out.ia = p;
				}
			}
			return out.ix != kPlyMissing && out.iy != kPlyMissing && out.iz != kPlyMissing;
		}
	}
	return false;
}

static bool LocateFaceElement( const std::vector<PlyElement>& elements, PlyFaceLayout& out )
{
	out.elementIndex = kPlyMissing;
	out.iIndices     = kPlyMissing;
	for( size_t e = 0; e < elements.size(); ++e ) {
		if( elements[e].name == "face" ) {
			out.elementIndex = e;
			const std::vector<PlyProperty>& props = elements[e].properties;
			for( size_t p = 0; p < props.size(); ++p ) {
				if( IsFaceListProperty( props[p] ) ) {
					out.iIndices = p;
					break;
				}
			}
			return out.iIndices != kPlyMissing;
		}
	}
	return false;
}

// Forward declarations for the schema-driven body helpers and the face
// emitter; they live further down in this file as file-scope statics.
static bool LoadAsciiBodyImpl(
	ITriangleMeshGeometryIndexed* pGeom,
	ITriangleMeshGeometryIndexed2* pGeom2,
	FILE* inputFile,
	const std::vector<PlyElement>& elements,
	const PlyVertexLayout& vlay,
	const PlyFaceLayout& flay,
	bool bInvertFaces );

static bool LoadBinaryBodyImpl(
	ITriangleMeshGeometryIndexed* pGeom,
	ITriangleMeshGeometryIndexed2* pGeom2,
	FILE* inputFile,
	const std::vector<PlyElement>& elements,
	const PlyVertexLayout& vlay,
	const PlyFaceLayout& flay,
	bool bFlipEndianess,
	bool bInvertFaces );

static void EmitFace(
	ITriangleMeshGeometryIndexed* pGeom,
	const unsigned int verts[4],
	unsigned int vertCount,
	bool bInvertFaces );

// Convert a captured PLY (red, green, blue) triple into a RISEPel.
// The PLY spec does not formally name a colour space, but the universal
// convention from Blender / PCL / glTF / FBX exporters is sRGB; the
// uchar form especially is invariably gamma-encoded.  Source channels
// in [0, 1] go through the sRGBPel→ROMMRGBPel converting constructor;
// integer channels are normalised first by NormalizePlyChannelValue.
static RISEPel ConvertPlyVertexColor(
	double r_raw, PlyType rType,
	double g_raw, PlyType gType,
	double b_raw, PlyType bType )
{
	const double r = NormalizePlyChannelValue( r_raw, rType );
	const double g = NormalizePlyChannelValue( g_raw, gType );
	const double b = NormalizePlyChannelValue( b_raw, bType );
	const sRGBPel src( r, g, b );
	return RISEPel( src );
}

bool TriangleMeshLoaderPLY::LoadAscii( ITriangleMeshGeometryIndexed* pGeom, FILE* inputFile, const unsigned int numVerts, const unsigned int numPolygons )
{
	// Legacy entry point.  Kept for binary-compat; the new schema-driven
	// path lives in the *Impl helpers below.  This wrapper builds a
	// minimal "x y z + vertex_indices" schema that mirrors the old
	// hardcoded behavior.
	std::vector<PlyElement> elements;
	PlyElement ev;
	ev.name = "vertex"; ev.count = numVerts;
	{
		PlyProperty p; p.countType = PLY_INVALID; p.type = PLY_FLOAT64;
		p.name = "x"; ev.properties.push_back( p );
		p.name = "y"; ev.properties.push_back( p );
		p.name = "z"; ev.properties.push_back( p );
	}
	PlyElement ef;
	ef.name = "face"; ef.count = numPolygons;
	{
		PlyProperty p; p.countType = PLY_UINT8; p.type = PLY_UINT32; p.name = "vertex_indices";
		ef.properties.push_back( p );
	}
	elements.push_back( ev );
	elements.push_back( ef );

	PlyVertexLayout vlay; PlyFaceLayout flay;
	(void)LocateVertexElement( elements, vlay );
	(void)LocateFaceElement( elements, flay );
	return LoadAsciiBodyImpl( pGeom, /*pGeom2=*/0, inputFile, elements, vlay, flay, bInvertFaces );
}

bool TriangleMeshLoaderPLY::LoadBinary( ITriangleMeshGeometryIndexed* pGeom, FILE* inputFile, const unsigned int numVerts, const unsigned int numPolygons, const bool bFlipEndianess )
{
	// Mirror of LoadAscii's compatibility wrapper for the binary path.
	std::vector<PlyElement> elements;
	PlyElement ev;
	ev.name = "vertex"; ev.count = numVerts;
	{
		PlyProperty p; p.countType = PLY_INVALID; p.type = PLY_FLOAT32;
		p.name = "x"; ev.properties.push_back( p );
		p.name = "y"; ev.properties.push_back( p );
		p.name = "z"; ev.properties.push_back( p );
	}
	PlyElement ef;
	ef.name = "face"; ef.count = numPolygons;
	{
		PlyProperty p; p.countType = PLY_UINT8; p.type = PLY_UINT32; p.name = "vertex_indices";
		ef.properties.push_back( p );
	}
	elements.push_back( ev );
	elements.push_back( ef );

	PlyVertexLayout vlay; PlyFaceLayout flay;
	(void)LocateVertexElement( elements, vlay );
	(void)LocateFaceElement( elements, flay );
	return LoadBinaryBodyImpl( pGeom, /*pGeom2=*/0, inputFile, elements, vlay, flay, bFlipEndianess, bInvertFaces );
}

static bool LoadAsciiBodyImpl(
	ITriangleMeshGeometryIndexed* pGeom,
	ITriangleMeshGeometryIndexed2* pGeom2,
	FILE* inputFile,
	const std::vector<PlyElement>& elements,
	const PlyVertexLayout& vlay,
	const PlyFaceLayout& flay,
	bool bInvertFaces )
{
	char line[4096];
	std::vector<char*> tokens;
	const bool wantColors = pGeom2 && vlay.hasColor();

	for( size_t ei = 0; ei < elements.size(); ++ei ) {
		const PlyElement& el = elements[ei];
		const bool isVertex = (ei == vlay.elementIndex);
		const bool isFace   = (ei == flay.elementIndex);

		for( unsigned int i = 0; i < el.count; ++i ) {
			if( fgets( line, sizeof(line), inputFile ) == NULL ) {
				GlobalLog()->PrintEx( eLog_Error,
					"TriangleMeshLoaderPLY:: Failed to read element '%s' #%u",
					el.name.c_str(), i );
				return false;
			}
			TokenizeLine( line, tokens );

			if( isVertex ) {
				// Walk the property schema, consuming the right number of
				// tokens per property.  Capture x/y/z, plus optionally
				// red/green/blue when the source carries colors.
				size_t tok = 0;
				double x = 0, y = 0, z = 0;
				double rRaw = 0, gRaw = 0, bRaw = 0;
				PlyType rT = PLY_UINT8, gT = PLY_UINT8, bT = PLY_UINT8;
				for( size_t pi = 0; pi < el.properties.size(); ++pi ) {
					const PlyProperty& p = el.properties[pi];
					if( p.isList() ) {
						if( tok >= tokens.size() ) return false;
						double cnt = 0;
						if( !ParseAsciiScalar( tokens[tok++], p.countType, cnt ) ) return false;
						unsigned int n = (unsigned int)cnt;
						for( unsigned int k = 0; k < n; ++k ) {
							if( tok >= tokens.size() ) return false;
							double dummy;
							if( !ParseAsciiScalar( tokens[tok++], p.type, dummy ) ) return false;
						}
					} else {
						if( tok >= tokens.size() ) return false;
						double v;
						if( !ParseAsciiScalar( tokens[tok++], p.type, v ) ) return false;
						if(      pi == vlay.ix ) x = v;
						else if( pi == vlay.iy ) y = v;
						else if( pi == vlay.iz ) z = v;
						else if( pi == vlay.ir ) { rRaw = v; rT = p.type; }
						else if( pi == vlay.ig ) { gRaw = v; gT = p.type; }
						else if( pi == vlay.ib ) { bRaw = v; bT = p.type; }
					}
				}
				pGeom->AddVertex( Vertex( x, y, z ) );
				if( wantColors ) {
					pGeom2->AddColor( ConvertPlyVertexColor( rRaw, rT, gRaw, gT, bRaw, bT ) );
				}

			} else if( isFace ) {
				size_t tok = 0;
				unsigned int verts[4] = {0,0,0,0};
				unsigned int vertCount = 0;
				bool gotIndices = false;
				for( size_t pi = 0; pi < el.properties.size(); ++pi ) {
					const PlyProperty& p = el.properties[pi];
					if( p.isList() ) {
						if( tok >= tokens.size() ) return false;
						double cnt = 0;
						if( !ParseAsciiScalar( tokens[tok++], p.countType, cnt ) ) return false;
						unsigned int n = (unsigned int)cnt;
						if( pi == flay.iIndices ) {
							if( n != 3 && n != 4 ) {
								GlobalLog()->PrintEx( eLog_Error,
									"TriangleMeshLoaderPLY:: We only read triangles or quads, this file has a polygon with '%u' vertices", n );
								return false;
							}
							for( unsigned int k = 0; k < n; ++k ) {
								if( tok >= tokens.size() ) return false;
								double v;
								if( !ParseAsciiScalar( tokens[tok++], p.type, v ) ) return false;
								verts[k] = (unsigned int)v;
							}
							vertCount = n;
							gotIndices = true;
						} else {
							for( unsigned int k = 0; k < n; ++k ) {
								if( tok >= tokens.size() ) return false;
								double dummy;
								if( !ParseAsciiScalar( tokens[tok++], p.type, dummy ) ) return false;
							}
						}
					} else {
						if( tok >= tokens.size() ) return false;
						double dummy;
						if( !ParseAsciiScalar( tokens[tok++], p.type, dummy ) ) return false;
					}
				}
				if( !gotIndices ) return false;
				EmitFace( pGeom, verts, vertCount, bInvertFaces );

			} else {
				// Unknown / ignored element — line already consumed.
			}
		}
	}

	return true;
}

static bool LoadBinaryBodyImpl(
	ITriangleMeshGeometryIndexed* pGeom,
	ITriangleMeshGeometryIndexed2* pGeom2,
	FILE* inputFile,
	const std::vector<PlyElement>& elements,
	const PlyVertexLayout& vlay,
	const PlyFaceLayout& flay,
	bool bFlipEndianess,
	bool bInvertFaces )
{
	const bool wantColors = pGeom2 && vlay.hasColor();
	for( size_t ei = 0; ei < elements.size(); ++ei ) {
		const PlyElement& el = elements[ei];
		const bool isVertex = (ei == vlay.elementIndex);
		const bool isFace   = (ei == flay.elementIndex);

		for( unsigned int i = 0; i < el.count; ++i ) {

			if( isVertex ) {
				double x = 0, y = 0, z = 0;
				double rRaw = 0, gRaw = 0, bRaw = 0;
				PlyType rT = PLY_UINT8, gT = PLY_UINT8, bT = PLY_UINT8;
				for( size_t pi = 0; pi < el.properties.size(); ++pi ) {
					const PlyProperty& p = el.properties[pi];
					if( p.isList() ) {
						double cnt = 0;
						if( !ReadBinaryScalar( inputFile, p.countType, bFlipEndianess, cnt ) ) return false;
						unsigned int n = (unsigned int)cnt;
						for( unsigned int k = 0; k < n; ++k ) {
							if( !SkipBinaryScalar( inputFile, p.type ) ) return false;
						}
					} else {
						double v;
						if( !ReadBinaryScalar( inputFile, p.type, bFlipEndianess, v ) ) return false;
						if(      pi == vlay.ix ) x = v;
						else if( pi == vlay.iy ) y = v;
						else if( pi == vlay.iz ) z = v;
						else if( pi == vlay.ir ) { rRaw = v; rT = p.type; }
						else if( pi == vlay.ig ) { gRaw = v; gT = p.type; }
						else if( pi == vlay.ib ) { bRaw = v; bT = p.type; }
					}
				}
				pGeom->AddVertex( Vertex( x, y, z ) );
				if( wantColors ) {
					pGeom2->AddColor( ConvertPlyVertexColor( rRaw, rT, gRaw, gT, bRaw, bT ) );
				}

			} else if( isFace ) {
				unsigned int verts[4] = {0,0,0,0};
				unsigned int vertCount = 0;
				bool gotIndices = false;
				for( size_t pi = 0; pi < el.properties.size(); ++pi ) {
					const PlyProperty& p = el.properties[pi];
					if( p.isList() ) {
						double cnt = 0;
						if( !ReadBinaryScalar( inputFile, p.countType, bFlipEndianess, cnt ) ) return false;
						unsigned int n = (unsigned int)cnt;
						if( pi == flay.iIndices ) {
							if( n != 3 && n != 4 ) {
								GlobalLog()->PrintEx( eLog_Error,
									"TriangleMeshLoaderPLY:: We only read triangles or quads, this file has a polygon with '%u' vertices", n );
								return false;
							}
							for( unsigned int k = 0; k < n; ++k ) {
								double v;
								if( !ReadBinaryScalar( inputFile, p.type, bFlipEndianess, v ) ) return false;
								verts[k] = (unsigned int)v;
							}
							vertCount = n;
							gotIndices = true;
						} else {
							for( unsigned int k = 0; k < n; ++k ) {
								if( !SkipBinaryScalar( inputFile, p.type ) ) return false;
							}
						}
					} else {
						if( !SkipBinaryScalar( inputFile, p.type ) ) return false;
					}
				}
				if( !gotIndices ) return false;
				EmitFace( pGeom, verts, vertCount, bInvertFaces );

			} else {
				// Unknown element — walk the schema to advance past it.
				for( size_t pi = 0; pi < el.properties.size(); ++pi ) {
					const PlyProperty& p = el.properties[pi];
					if( p.isList() ) {
						double cnt = 0;
						if( !ReadBinaryScalar( inputFile, p.countType, bFlipEndianess, cnt ) ) return false;
						unsigned int n = (unsigned int)cnt;
						for( unsigned int k = 0; k < n; ++k ) {
							if( !SkipBinaryScalar( inputFile, p.type ) ) return false;
						}
					} else {
						if( !SkipBinaryScalar( inputFile, p.type ) ) return false;
					}
				}
			}
		}
	}

	return true;
}

static void EmitFace(
	ITriangleMeshGeometryIndexed* pGeom,
	const unsigned int verts[4],
	unsigned int vertCount,
	bool bInvertFaces )
{
	if( vertCount == 3 ) {
		IndexedTriangle tri;
		const unsigned int a = verts[0], b = verts[1], c = verts[2];
		if( bInvertFaces ) {
			tri.iVertices[0] = tri.iNormals[0] = a;
			tri.iVertices[1] = tri.iNormals[1] = c;
			tri.iVertices[2] = tri.iNormals[2] = b;
		} else {
			tri.iVertices[0] = tri.iNormals[0] = a;
			tri.iVertices[1] = tri.iNormals[1] = b;
			tri.iVertices[2] = tri.iNormals[2] = c;
		}
		tri.iCoords[0] = tri.iCoords[1] = tri.iCoords[2] = 0;
		pGeom->AddIndexedTriangle( tri );

	} else if( vertCount == 4 ) {
		const unsigned int a = verts[0], b = verts[1], c = verts[2], d = verts[3];
		IndexedTriangle triA;
		IndexedTriangle triB;
		if( bInvertFaces ) {
			triA.iVertices[0] = triA.iNormals[0] = a;
			triA.iVertices[1] = triA.iNormals[1] = c;
			triA.iVertices[2] = triA.iNormals[2] = b;
			triB.iVertices[0] = triB.iNormals[0] = a;
			triB.iVertices[1] = triB.iNormals[1] = d;
			triB.iVertices[2] = triB.iNormals[2] = c;
		} else {
			triA.iVertices[0] = triA.iNormals[0] = a;
			triA.iVertices[1] = triA.iNormals[1] = b;
			triA.iVertices[2] = triA.iNormals[2] = c;
			triB.iVertices[0] = triB.iNormals[0] = a;
			triB.iVertices[1] = triB.iNormals[1] = c;
			triB.iVertices[2] = triB.iNormals[2] = d;
		}
		triA.iCoords[0] = triA.iCoords[1] = triA.iCoords[2] = 0;
		triB.iCoords[0] = triB.iCoords[1] = triB.iCoords[2] = 0;
		pGeom->AddIndexedTriangle( triA );
		pGeom->AddIndexedTriangle( triB );
	}
}

bool TriangleMeshLoaderPLY::LoadTriangleMesh( ITriangleMeshGeometryIndexed* pGeom )
{
	FILE* inputFile = fopen( szFilename, "rb" );

	if( !inputFile || !pGeom ) {
		GlobalLog()->Print( eLog_Error, "TriangleMeshLoaderPLY:: Failed to open file or bad geometry object" );
		if( inputFile ) fclose( inputFile );
		return false;
	}

	char line[4096] = {0};
	if( fgets( line, sizeof(line), inputFile ) == NULL ) {
		fclose( inputFile );
		return false;
	}
	if( strncmp( line, "ply", 3 ) != 0 ) {
		GlobalLog()->PrintEasyError( "TriangleMeshLoaderPLY:: Failed to read ply header" );
		fclose( inputFile );
		return false;
	}

	std::vector<PlyElement> elements;
	bool bAscii = true;
	bool bBigEndian = false;
	if( !ParsePlyHeader( inputFile, elements, bAscii, bBigEndian ) ) {
		fclose( inputFile );
		return false;
	}

	PlyVertexLayout vlay;
	PlyFaceLayout flay;
	if( !LocateVertexElement( elements, vlay ) ) {
		GlobalLog()->PrintEasyError( "TriangleMeshLoaderPLY:: header has no 'vertex' element with x/y/z" );
		fclose( inputFile );
		return false;
	}
	if( !LocateFaceElement( elements, flay ) ) {
		GlobalLog()->PrintEasyError( "TriangleMeshLoaderPLY:: header has no 'face' element with vertex_indices" );
		fclose( inputFile );
		return false;
	}

	pGeom->BeginIndexedTriangles();
	pGeom->AddTexCoord( TexCoord( 0, 0 ) );

	// Vertex colors flow through the v2 sub-interface.  If the geometry
	// implementation doesn't expose it (an out-of-tree subclass that
	// hasn't picked up the new interface), drop colors silently rather
	// than fail the load — positions still get loaded correctly.
	ITriangleMeshGeometryIndexed2* pGeom2 = dynamic_cast<ITriangleMeshGeometryIndexed2*>( pGeom );
	if( vlay.hasColor() && !pGeom2 ) {
		GlobalLog()->PrintEasyWarning(
			"TriangleMeshLoaderPLY:: source has vertex colors but the target "
			"geometry does not implement ITriangleMeshGeometryIndexed2 — colors will be dropped" );
	}

	bool bSuccess;
	if( bAscii ) {
		bSuccess = LoadAsciiBodyImpl( pGeom, pGeom2, inputFile, elements, vlay, flay, bInvertFaces );
	} else {
#ifdef RISE_BIG_ENDIAN
		bSuccess = LoadBinaryBodyImpl( pGeom, pGeom2, inputFile, elements, vlay, flay, !bBigEndian, bInvertFaces );
#else
		bSuccess = LoadBinaryBodyImpl( pGeom, pGeom2, inputFile, elements, vlay, flay, bBigEndian, bInvertFaces );
#endif
	}

	fclose( inputFile );

	if( bSuccess ) {
		pGeom->ComputeVertexNormals();
		pGeom->DoneIndexedTriangles();
		return true;
	}

	GlobalLog()->PrintEasyError( "TriangleMeshLoaderPLY:: Something went wrong while trying to read the data" );
	return false;
}
