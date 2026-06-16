//////////////////////////////////////////////////////////////////////
//
//  PngPainterFilterTypeParseTest.cpp - Regression for the png_painter
//    (and sibling image-painter) `filter_type` descriptor/Finalize
//    reconciliation.
//
//    Before this fix the image-painter chunk descriptors advertised
//    filter_type enum = {nearest, bilinear, catmull-rom, box,
//    cubic-bspline, gaussian}, but Finalize() only accepted the
//    PascalCase set {NNB, Bilinear, CatmullRom, UniformBSpline} and
//    hard-failed the whole chunk on anything else.  So a scene that
//    copied an advertised value (e.g. `filter_type bilinear`) failed to
//    load with "Failed to load chunk 'png_painter'", while two of the
//    advertised values (box, gaussian) had no backing accessor at all.
//
//    The descriptor-driven parser design intends the descriptor to BE
//    the accepted set, so the fix makes Finalize accept the descriptor's
//    user-facing lowercase names (mapping to the four RasterImage
//    accessors the runtime actually implements) while keeping the legacy
//    PascalCase names as backward-compatible aliases.  All five image
//    painters (png / jpg / hdr / exr / tiff) route through the shared
//    ParseFilterTypeParam / AddFilterTypeParam helpers.
//
//    This test owns the parser contract:
//
//      1. Every image-painter descriptor advertises EXACTLY the four
//         supported modes (and the removed box/gaussian are gone).
//      2. Every value the png_painter descriptor advertises actually
//         parses (data-driven off the grammar, so a future-added mode is
//         covered automatically) -- this is the core regression.
//      3. The legacy PascalCase aliases still parse (back-compat).
//      4. Unknown / removed values (box, gaussian, bogus) are rejected.
//      5. Omitting filter_type still parses (default bilinear).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/ISceneParser.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/IRasterImageWriter.h"
#include "../src/Library/Interfaces/IWriteBuffer.h"
#include "../src/Library/Parsers/ChunkDescriptor.h"
#include "../src/Library/SceneEditorSuggestions/SceneGrammar.h"
#include "../src/Library/Utilities/Color/Color.h"

using namespace RISE;
using namespace RISE::Implementation;
using RISE::SceneEditorSuggestions::SceneGrammar;

namespace
{
	int s_pass = 0;
	int s_fail = 0;

	void Check( bool ok, const std::string& what )
	{
		if( ok ) {
			++s_pass;
			std::cout << "  ok  : " << what << "\n";
		} else {
			++s_fail;
			std::cout << "  FAIL: " << what << "\n";
		}
	}

	std::string TempDir()
	{
		const char* t = getenv( "TMPDIR" );
		std::string dir = ( t && t[0] ) ? t : "/tmp/";
		if( dir.back() != '/' ) dir += '/';
		return dir;
	}

	std::string WriteTempScene( const char* tag, const std::string& body )
	{
		std::string path = TempDir() + "rise_filtertype_" + tag + ".RISEscene";
		std::ofstream f( path.c_str(), std::ios::binary | std::ios::trunc );
		f << body;
		f.close();
		return path;
	}

	bool ParseSceneFile( const std::string& path, Job& job )
	{
		ISceneParser* parser = 0;
		if( !RISE_API_CreateAsciiSceneParser( &parser, path.c_str() ) || !parser ) {
			return false;
		}
		parser->addref();
		const bool ok = parser->ParseAndLoadScene( job );
		parser->release();
		return ok;
	}

	// Write a tiny solid-colour PNG to TMPDIR using RISE's own raster
	// image + PNG writer, so the parse test has a real image-painter
	// source without depending on RISE_MEDIA_PATH.  Stored as
	// Rec709RGB_Linear (the verbatim-store idiom -- no sRGB gamma warp).
	std::string WriteTempPNG()
	{
		const std::string path = TempDir() + "rise_filtertype_src.png";

		IRasterImage* img = 0;
		RISE_API_CreateRISEColorRasterImage( &img, 4, 4, RISEColor( RISEPel( 0.5, 0.25, 0.75 ), 1.0 ) );

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

	// A png_painter scene that sets filter_type to `value` (or omits the
	// line entirely when `value` is empty).  Inline + absolute PNG path so
	// the parse is hermetic.
	std::string PngPainterScene( const std::string& pngPath, const std::string& value )
	{
		std::string s;
		s += "RISE ASCII SCENE 6\n";
		s += "png_painter\n{\nname tex\nfile " + pngPath + "\ncolor_space Rec709RGB_Linear\n";
		if( !value.empty() ) {
			s += "filter_type " + value + "\n";
		}
		s += "}\n";
		return s;
	}

	// Parse a one-painter scene built from `value` and report whether the
	// painter `tex` ended up registered.  `outParsed` receives the raw
	// ParseAndLoadScene result.
	bool ParsePngWithFilter( const std::string& pngPath, const std::string& value, bool& outParsed )
	{
		const std::string tag = value.empty() ? std::string("default") : value;
		// '/' from "catmull-rom" etc. is fine in a filename, but keep the
		// tag filesystem-safe regardless.
		std::string safeTag = tag;
		std::replace( safeTag.begin(), safeTag.end(), '/', '_' );
		const std::string path = WriteTempScene( safeTag.c_str(), PngPainterScene( pngPath, value ) );

		Job* job = new Job();
		job->addref();
		outParsed = ParseSceneFile( path, *job );
		IPainter* p = job->GetPainters() ? job->GetPainters()->GetItem( "tex" ) : 0;
		const bool registered = ( p != 0 );
		job->release();
		std::remove( path.c_str() );
		return registered;
	}
}

// ============================================================
//  Test 1: every image-painter descriptor advertises EXACTLY the four
//          supported filter modes (and box/gaussian are gone).
// ============================================================
static void TestDescriptorsAdvertiseSupportedModes()
{
	std::cout << "\n[1] image-painter descriptors advertise exactly the 4 supported filter modes\n";

	const std::vector<std::string> expected = { "nearest", "bilinear", "catmull-rom", "cubic-bspline" };
	const char* chunks[] = { "png_painter", "jpg_painter", "hdr_painter", "exr_painter", "tiff_painter" };

	const SceneGrammar& g = SceneGrammar::Instance();
	for( const char* keyword : chunks ) {
		const ChunkDescriptor* d = g.FindChunk( keyword );
		Check( d != 0, std::string( "grammar knows chunk `" ) + keyword + "`" );
		if( !d ) continue;

		const ParameterDescriptor* ft = 0;
		for( const ParameterDescriptor& p : d->parameters ) {
			if( p.name == "filter_type" ) { ft = &p; break; }
		}
		Check( ft != 0, std::string( keyword ) + " declares a filter_type parameter" );
		if( !ft ) continue;

		Check( ft->enumValues == expected,
			std::string( keyword ) + " filter_type advertises exactly {nearest, bilinear, catmull-rom, cubic-bspline}" );

		const bool hasFiction =
			std::find( ft->enumValues.begin(), ft->enumValues.end(), std::string("box") ) != ft->enumValues.end() ||
			std::find( ft->enumValues.begin(), ft->enumValues.end(), std::string("gaussian") ) != ft->enumValues.end();
		Check( !hasFiction, std::string( keyword ) + " no longer advertises the backing-less box/gaussian modes" );
	}
}

// ============================================================
//  Test 2: every value the png_painter descriptor advertises actually
//          parses.  Data-driven off the grammar so the descriptor IS the
//          accepted set -- a future-added mode is covered automatically,
//          and any new descriptor/Finalize drift fails here.
// ============================================================
static void TestAdvertisedValuesParse( const std::string& pngPath )
{
	std::cout << "\n[2] every advertised png_painter filter_type value parses\n";

	const ChunkDescriptor* d = SceneGrammar::Instance().FindChunk( "png_painter" );
	const ParameterDescriptor* ft = 0;
	if( d ) {
		for( const ParameterDescriptor& p : d->parameters ) {
			if( p.name == "filter_type" ) { ft = &p; break; }
		}
	}
	Check( ft != 0 && !ft->enumValues.empty(), "png_painter advertises a non-empty filter_type enum" );
	if( !ft ) return;

	for( const std::string& value : ft->enumValues ) {
		bool parsed = false;
		const bool registered = ParsePngWithFilter( pngPath, value, parsed );
		Check( parsed && registered,
			"png_painter filter_type `" + value + "` loads successfully (chunk parses + painter registered)" );
	}
}

// ============================================================
//  Test 3: the legacy PascalCase names still parse (back-compat).  These
//          were the ONLY accepted strings before the fix, so every
//          pre-existing scene that set filter_type uses them.
// ============================================================
static void TestLegacyAliasesStillParse( const std::string& pngPath )
{
	std::cout << "\n[3] legacy PascalCase filter_type aliases still parse (back-compat)\n";

	const char* aliases[] = { "NNB", "Bilinear", "CatmullRom", "UniformBSpline" };
	for( const char* value : aliases ) {
		bool parsed = false;
		const bool registered = ParsePngWithFilter( pngPath, value, parsed );
		Check( parsed && registered,
			std::string( "legacy filter_type `" ) + value + "` still loads successfully" );
	}
}

// ============================================================
//  Test 4: unknown / removed filter_type values are rejected (the chunk
//          fails to load, so ParseAndLoadScene returns false and the
//          painter is NOT registered).  Covers the now-removed box/
//          gaussian and a generic typo.
// ============================================================
static void TestUnknownValuesRejected( const std::string& pngPath )
{
	std::cout << "\n[4] unknown / removed filter_type values are rejected\n";

	const char* bad[] = { "box", "gaussian", "bogus" };
	for( const char* value : bad ) {
		bool parsed = false;
		const bool registered = ParsePngWithFilter( pngPath, value, parsed );
		Check( !parsed && !registered,
			std::string( "filter_type `" ) + value + "` is rejected (chunk fails to load, painter not registered)" );
	}
}

// ============================================================
//  Test 5: omitting filter_type parses (default bilinear).  Guards the
//          absent-parameter branch of the shared helper.
// ============================================================
static void TestDefaultParses( const std::string& pngPath )
{
	std::cout << "\n[5] omitting filter_type parses (default bilinear)\n";

	bool parsed = false;
	const bool registered = ParsePngWithFilter( pngPath, std::string(), parsed );
	Check( parsed && registered, "png_painter with no filter_type line loads successfully (default)" );
}

int main()
{
	std::cout << "=== PngPainterFilterTypeParseTest -- filter_type descriptor/Finalize reconciliation ===\n";
	GlobalLog();	// initialize the global log

	const std::string pngPath = WriteTempPNG();

	TestDescriptorsAdvertiseSupportedModes();
	TestAdvertisedValuesParse( pngPath );
	TestLegacyAliasesStillParse( pngPath );
	TestUnknownValuesRejected( pngPath );
	TestDefaultParses( pngPath );

	std::remove( pngPath.c_str() );

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
