//////////////////////////////////////////////////////////////////////
//
//  ScalarTexturePainterTest.cpp - Scene-language + sampling test for
//    the `texture` form of the `scalar_painter` chunk (the spatially-
//    varying IScalarPainter backed by a 2D image map).
//
//    New syntax (form 10 of `scalar_painter`):
//
//      png_painter   { name dial_oxide_png  file <png>  color_space Rec709RGB_Linear }
//      scalar_painter{ name oxide_thk  texture dial_oxide_png  channel R  scale 220  bias 30 }
//
//    Semantics: at each surface hit the painter samples the named image
//    painter's RASTER directly (no Jakob-Hanika uplift, no colourspace
//    conversion) at the hit UV, picks the chosen channel (R/G/B, default
//    R), and returns `bias + scale * rawTexel` (rawTexel in [0,1]).  This
//    is the supported way to drive an IScalarPainter material slot (e.g.
//    the thin-film `film_thickness`) from an image map.
//
//    This test owns the PLUMBING + SAMPLING contract:
//
//      1. A scene declaring a png_painter + a `scalar_painter { texture
//         ... }` bound to a thinfilm ggx_material's `film_thickness`
//         PARSES and registers the material (proves the texture form is
//         accepted, resolves the image painter, and lands in the
//         IScalarPainterManager where the slot resolver finds it).
//      2. The affine sampling is correct: a TextureScalarPainter built
//         through the public factory returns exactly `bias + scale*texel`
//         for a known pixel, and channel-select picks the right channel
//         (R vs G vs B).  Verified with an in-memory 1x1 raster so the
//         check is independent of any UV->pixel rounding convention.
//      3. The value is NOT JH-uplifted: a raw texel of 0.5 with scale=1,
//         bias=0 comes back as 0.5 (a JH spectral uplift of an inline
//         numeric RGB would mangle a physical magnitude — that's the
//         whole reason IScalarPainter exists).
//      4. Diagnostics fire for the obvious authoring mistakes (unknown
//         texture name; texture that names a non-image painter).
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
#include <cmath>
#include <string>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/ISceneParser.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Interfaces/IScalarPainterManager.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/IRasterImageAccessor.h"
#include "../src/Library/Interfaces/IRasterImageWriter.h"
#include "../src/Library/Interfaces/IWriteBuffer.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/Color/Color.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int s_pass = 0;
	int s_fail = 0;

	void Check( bool ok, const char* what )
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
		std::string path = TempDir() + "rise_scalartex_" + tag + ".RISEscene";
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
	// image + PNG writer, so the parse test has a real image painter
	// source without depending on RISE_MEDIA_PATH.  Stored as
	// Rec709RGB_Linear (the verbatim-store idiom — no sRGB gamma warp).
	std::string WriteTempPNG( const char* tag, double r, double g, double b )
	{
		const std::string path = TempDir() + "rise_scalartex_" + tag + ".png";

		IRasterImage* img = 0;
		RISE_API_CreateRISEColorRasterImage( &img, 4, 4, RISEColor( RISEPel( r, g, b ), 1.0 ) );

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

	// A RayIntersectionGeometric at UV (u,v), surface normal +Z.
	RayIntersectionGeometric MakeRI( double u, double v )
	{
		Ray inRay( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) );
		RasterizerState rs = { 0, 0 };
		RayIntersectionGeometric ri( inRay, rs );
		ri.bHit = true;
		ri.range = 1.0;
		ri.ptIntersection = Point3( 0, 0, 0 );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
		ri.ptCoord = Point2( u, v );
		return ri;
	}

	// Build a TextureScalarPainter (via the public affine factory) over a
	// 1x1 in-memory raster whose single pixel is (r,g,b).  A 1x1 image
	// maps EVERY UV to that one pixel, so the sampled value is
	// independent of the accessor's UV->pixel rounding convention — what
	// we're testing here is the channel-select + affine remap, not the
	// sampler's addressing.
	IScalarPainter* MakeTexScalar( double r, double g, double b,
	                               unsigned int channel, double scale, double bias )
	{
		IRasterImage* img = 0;
		RISE_API_CreateRISEColorRasterImage( &img, 1, 1, RISEColor( RISEPel( r, g, b ), 1.0 ) );

		IRasterImageAccessor* ria = 0;
		RISE_API_CreateNNBRasterImageAccessor( &ria, *img );	// nearest-neighbour: no interpolation

		IScalarPainter* sp = 0;
		RISE_API_CreateTextureScalarPainterAffine( &sp, ria, channel, Scalar( scale ), Scalar( bias ) );

		// The painter and accessor both addref what they wrap; drop our
		// local references so the painter is the sole remaining owner.
		if( ria ) ria->release();
		if( img ) img->release();
		return sp;	// caller releases
	}

	bool Near( double a, double b, double tol )
	{
		return std::fabs( a - b ) <= tol;
	}
}

// ============================================================
//  Test 1: scalar_painter { texture ... } bound to a thinfilm
//          film_thickness parses + registers the material.
// ============================================================
static void TestTextureFormParsesAndBinds()
{
	std::cout << "\n[1] scalar_painter { texture ... } bound to film_thickness parses + registers\n";

	const std::string png = WriteTempPNG( "dial_oxide", 0.5, 0.25, 0.75 );

	std::string s;
	s += "RISE ASCII SCENE 6\n";
	s += "uniformcolor_painter\n{\nname rd\ncolor 0.0 0.0 0.0\n}\n";
	s += "uniformcolor_painter\n{\nname rs\ncolor 1.0 1.0 1.0\n}\n";
	s += std::string("png_painter\n{\nname dial_oxide_png\nfile ") + png + "\ncolor_space Rec709RGB_Linear\n}\n";
	s += "scalar_painter\n{\nname sub_n\nvalue 0.5\n}\n";
	s += "scalar_painter\n{\nname sub_k\nvalue 2.7\n}\n";
	s += "scalar_painter\n{\nname film_n\nvalue 2.5\n}\n";
	s += "scalar_painter\n{\nname film_k\nvalue 0.0\n}\n";
	// The feature under test: a spatially-varying film thickness from the
	// red channel of the image, remapped to 30..250 nm.
	s += "scalar_painter\n{\nname oxide_thk\ntexture dial_oxide_png\nchannel R\nscale 220\nbias 30\n}\n";
	s += "scalar_painter\n{\nname rough\nvalue 0.1\n}\n";
	s += "ggx_material\n{\nname ti_heattint\n";
	s += "rd rd\nrs rs\nalphax rough\nalphay rough\n";
	s += "ior sub_n\nextinction sub_k\n";
	s += "fresnel_mode thinfilm\n";
	s += "film_ior film_n\nfilm_extinction film_k\n";
	s += "film_thickness oxide_thk\n";		// <-- texture-driven scalar slot
	s += "}\n";

	const std::string path = WriteTempScene( "ok", s );
	Job* job = new Job();
	job->addref();

	const bool parsed = ParseSceneFile( path, *job );
	Check( parsed, "scene with png_painter + texture scalar_painter bound to film_thickness parses" );

	// The texture scalar_painter must land in the IScalarPainterManager.
	IScalarPainter* sp = job->GetScalarPainters() ? job->GetScalarPainters()->GetItem( "oxide_thk" ) : 0;
	Check( sp != 0, "texture scalar_painter `oxide_thk` registered in the scalar-painter manager" );

	// And the material it feeds must be registered (slot resolver found it).
	IMaterial* mat = job->GetMaterials() ? job->GetMaterials()->GetItem( "ti_heattint" ) : 0;
	Check( mat != 0, "thinfilm material `ti_heattint` registered (film_thickness slot resolved to the texture scalar_painter)" );

	job->release();
	std::remove( path.c_str() );
	std::remove( png.c_str() );
}

// ============================================================
//  Test 2: affine sampling — out = bias + scale * rawTexel.
// ============================================================
static void TestAffineSampling()
{
	std::cout << "\n[2] TextureScalarPainter returns bias + scale * rawTexel\n";

	// pixel R = 0.5 ; scale 220 ; bias 30  ->  30 + 220*0.5 = 140.
	IScalarPainter* sp = MakeTexScalar( /*r*/0.5, /*g*/0.25, /*b*/0.75,
	                                    /*channel R*/0, /*scale*/220.0, /*bias*/30.0 );
	if( sp ) sp->addref();
	Check( sp != 0, "affine texture scalar_painter constructed" );
	if( sp ) {
		RayIntersectionGeometric ri = MakeRI( 0.37, 0.62 );		// arbitrary UV; 1x1 img -> same pixel
		const ScalarTriple t = sp->GetValuesAt( ri );
		std::cout << "    sampled = " << double(t.v[0]) << " (expected 140)\n";
		Check( Near( double(t.v[0]), 140.0, 1e-4 ),
			"R-channel 0.5 with scale=220 bias=30 -> 140" );
		// A TextureScalarPainter is single-scalar-per-UV: all three slots
		// carry the same remapped value.
		Check( Near( double(t.v[1]), 140.0, 1e-4 ) && Near( double(t.v[2]), 140.0, 1e-4 ),
			"single-scalar replication across the triple" );
		sp->release();
	}

	// Default scale=1, bias=0 returns the raw texel verbatim (NO JH uplift:
	// a physical magnitude is returned as-is, not mangled by the spectral
	// sigmoid LUT).
	IScalarPainter* raw = MakeTexScalar( 0.5, 0.25, 0.75, /*R*/0, /*scale*/1.0, /*bias*/0.0 );
	if( raw ) raw->addref();
	if( raw ) {
		RayIntersectionGeometric ri = MakeRI( 0.5, 0.5 );
		const ScalarTriple t = raw->GetValuesAt( ri );
		Check( Near( double(t.v[0]), 0.5, 1e-4 ),
			"raw texel passes through verbatim with scale=1 bias=0 (no JH uplift)" );
		raw->release();
	}
}

// ============================================================
//  Test 3: channel select picks R / G / B independently.
// ============================================================
static void TestChannelSelect()
{
	std::cout << "\n[3] channel select picks R / G / B\n";

	// Distinct channel values; scale=1 bias=0 so each comes back verbatim.
	const double R = 0.10, G = 0.40, B = 0.90;

	struct Case { unsigned int ch; double expect; const char* what; } cases[] = {
		{ 0, R, "channel R" },
		{ 1, G, "channel G" },
		{ 2, B, "channel B" },
	};

	for( int i = 0; i < 3; ++i ) {
		IScalarPainter* sp = MakeTexScalar( R, G, B, cases[i].ch, 1.0, 0.0 );
		if( sp ) sp->addref();
		if( sp ) {
			RayIntersectionGeometric ri = MakeRI( 0.5, 0.5 );
			const ScalarTriple t = sp->GetValuesAt( ri );
			Check( Near( double(t.v[0]), cases[i].expect, 1e-4 ), cases[i].what );
			sp->release();
		} else {
			Check( false, cases[i].what );
		}
	}
}

// ============================================================
//  Test 4: authoring diagnostics fire (parse fails, no material).
// ============================================================
static void TestDiagnostics()
{
	std::cout << "\n[4] texture-form authoring diagnostics fire\n";

	// (a) texture names a painter that doesn't exist.
	{
		std::string s;
		s += "RISE ASCII SCENE 6\n";
		s += "scalar_painter\n{\nname bad\ntexture nonexistent_png\nchannel R\n}\n";
		const std::string path = WriteTempScene( "bad_missing", s );
		Job* job = new Job(); job->addref();
		const bool parsed = ParseSceneFile( path, *job );
		Check( !parsed, "texture naming an undeclared painter fails to parse" );
		IScalarPainter* sp = job->GetScalarPainters() ? job->GetScalarPainters()->GetItem( "bad" ) : 0;
		Check( sp == 0, "the bad texture scalar_painter was NOT registered" );
		job->release();
		std::remove( path.c_str() );
	}

	// (b) texture names a NON-image painter (a uniformcolor_painter has no
	//     raster accessor -> not spatially samplable).
	{
		std::string s;
		s += "RISE ASCII SCENE 6\n";
		s += "uniformcolor_painter\n{\nname flat\ncolor 0.5 0.5 0.5\n}\n";
		s += "scalar_painter\n{\nname bad2\ntexture flat\nchannel R\n}\n";
		const std::string path = WriteTempScene( "bad_nonimage", s );
		Job* job = new Job(); job->addref();
		const bool parsed = ParseSceneFile( path, *job );
		Check( !parsed, "texture naming a non-image painter (uniformcolor_painter) fails to parse" );
		IScalarPainter* sp = job->GetScalarPainters() ? job->GetScalarPainters()->GetItem( "bad2" ) : 0;
		Check( sp == 0, "the non-image texture scalar_painter was NOT registered" );
		job->release();
		std::remove( path.c_str() );
	}
}

// ============================================================
//  Test 5: the OTHER scalar_painter forms still parse (regression).
//          A texture form must not have broken value / file / base / etc.
// ============================================================
static void TestExistingFormsStillParse()
{
	std::cout << "\n[5] the existing scalar_painter forms still parse\n";

	std::string s;
	s += "RISE ASCII SCENE 6\n";
	s += "scalar_painter\n{\nname f_value\nvalue 1.5\n}\n";
	s += "scalar_painter\n{\nname f_values\nvalues 1.3 1.5 2.0\n}\n";
	s += "scalar_painter\n{\nname f_sellmeier\nsellmeier 1.03961212 0.231792344 1.01046945 0.00600069867 0.0200179144 103.560653\n}\n";
	s += "scalar_painter\n{\nname f_poly\npolynomial 1.0 0.5 0.25\n}\n";
	s += "scalar_painter\n{\nname f_base\nbase f_value\nscale 0.5\n}\n";
	s += "scalar_painter\n{\nname f_mul\nmultiply f_value f_values\n}\n";

	const std::string path = WriteTempScene( "existing_forms", s );
	Job* job = new Job(); job->addref();
	const bool parsed = ParseSceneFile( path, *job );
	Check( parsed, "value / values / sellmeier / polynomial / base / multiply forms all still parse" );

	IScalarPainterManager* m = job->GetScalarPainters();
	Check( m && m->GetItem( "f_value" ) && m->GetItem( "f_values" ) &&
	          m->GetItem( "f_sellmeier" ) && m->GetItem( "f_poly" ) &&
	          m->GetItem( "f_base" ) && m->GetItem( "f_mul" ),
		"all six non-texture forms registered" );

	job->release();
	std::remove( path.c_str() );
}

int main()
{
	std::cout << "=== ScalarTexturePainterTest -- scalar_painter texture form ===\n";
	GlobalLog();	// initialize the global log

	TestTextureFormParsesAndBinds();
	TestAffineSampling();
	TestChannelSelect();
	TestDiagnostics();
	TestExistingFormsStillParse();

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
