//////////////////////////////////////////////////////////////////////
//
//  GlintModifierSceneParseTest.cpp - Validates the `glint_modifier`
//    scene-language chunk and its plumbing into the engine:
//
//    1. PLUMB-THROUGH FIDELITY: a parsed glint_modifier is
//       bit-identical in behaviour to a directly-constructed
//       GlintModifier with the same parameters (FindFacet compared
//       over many points) — catches any parameter-order or unit swap
//       between the chunk, IJob::AddGlintModifier, the RISE_API
//       factory, and the ctor.
//    2. DEFAULTS: a chunk with only a name constructs the documented
//       defaults (density 5, coverage 0.15, fill 0.6, spread 2,
//       scale 1 1 1, shift 0 0 0, seed 0), verified the same way.
//    3. LOUD DIAGNOSTICS: (a) a non-finite token (`coverage nan`)
//       fails the parse at the descriptor layer
//       (AllTokensAreFiniteNumbers — the load-bearing gate; the
//       ctor's laundered backstop only goes silently inert);
//       (b) inert domains (density 0, spread -1) fail the parse;
//       (c) a zero scale component fails the parse.
//    4. CLAMP WARNINGS: coverage/fill > 1 parse successfully and
//       behave exactly as 1.0 (documented clamp semantics).
//    5. OBJECT ATTACH: a standard_object referencing the modifier by
//       name parses (the Modifier reference category resolves).
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

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/ISceneParser.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Modifiers/GlintModifier.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"

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

	std::string WriteTempScene( const char* tag, const std::string& body )
	{
		std::string dir;
		const char* t = getenv( "TMPDIR" );
		if( t && t[0] ) {
			dir = t;
			if( dir.back() != '/' ) dir += '/';
		} else {
			dir = "/tmp/";
		}
		std::string path = dir + "rise_glint_parse_" + tag + ".RISEscene";
		std::ofstream f( path.c_str(), std::ios::binary | std::ios::trunc );
		f << body;
		f.close();
		return path;
	}

	bool ParseSceneFile( const std::string& path, Job& job )
	{
		ISceneParser* parser = 0;
		if( !RISE_API_CreateAsciiSceneParser( &parser, path.c_str() ) || !parser ) {
			std::remove( path.c_str() );
			return false;
		}
		parser->addref();
		const bool ok = parser->ParseAndLoadScene( job );
		parser->release();
		std::remove( path.c_str() );		// keep TMPDIR clean
		return ok;
	}

	//! Fetch a parsed modifier by name and downcast to GlintModifier.
	const GlintModifier* FetchGlint( Job& job, const char* name )
	{
		IJobPriv* priv = dynamic_cast<IJobPriv*>( &job );
		if( !priv || !priv->GetModifiers() ) return 0;
		IRayIntersectionModifier* mod = priv->GetModifiers()->GetItem( name );
		return dynamic_cast<const GlintModifier*>( mod );
	}

	//! Bit-compare two modifiers' facet fields over many sample points.
	bool FieldsIdentical( const GlintModifier& a, const GlintModifier& b, int samples )
	{
		RandomNumberGenerator rng( 7 );
		for( int i = 0; i < samples; i++ )
		{
			const Point3 p(
				rng.CanonicalRandom() * 40.0,
				rng.CanonicalRandom() * 40.0,
				rng.CanonicalRandom() * 40.0 );
			const GlintFacet fa = a.FindFacet( p );
			const GlintFacet fb = b.FindFacet( p );
			if( fa.found != fb.found ) return false;
			if( fa.found && ( fa.centreQ.x != fb.centreQ.x || fa.centreQ.y != fb.centreQ.y ||
			                  fa.centreQ.z != fb.centreQ.z || fa.theta != fb.theta || fa.phi != fb.phi ) ) {
				return false;
			}
		}
		return true;
	}

	//! True if the field is non-degenerate (finds SOME facets).
	bool FieldAlive( const GlintModifier& m )
	{
		RandomNumberGenerator rng( 9 );
		for( int i = 0; i < 5000; i++ )
		{
			const Point3 p(
				rng.CanonicalRandom() * 40.0,
				rng.CanonicalRandom() * 40.0,
				rng.CanonicalRandom() * 40.0 );
			if( m.FindFacet( p ).found ) return true;
		}
		return false;
	}

	std::string SceneHeader()
	{
		return "RISE ASCII SCENE 6\n";
	}
}

// ============================================================
//  1. Plumb-through fidelity
// ============================================================

static void TestPlumbThrough()
{
	std::cout << "--- 1: parsed chunk == direct ctor (bit-identical field) ---" << std::endl;

	std::string s = SceneHeader();
	s += "glint_modifier\n{\n";
	s += "name gm\n";
	s += "density 2.0\n";
	s += "coverage 0.8\n";
	s += "fill 1.0\n";
	s += "spread 4.0\n";
	s += "scale 3 1 1\n";
	s += "shift 0.37 0.11 -0.23\n";
	s += "seed 7\n";
	s += "}\n";

	Job* job = new Job();
	job->addref();
	const bool parsed = ParseSceneFile( WriteTempScene( "plumb", s ), *job );
	Check( parsed, "scene with fully-specified glint_modifier parses" );

	const GlintModifier* got = FetchGlint( *job, "gm" );
	Check( got != 0, "parsed modifier registered under its name and is a GlintModifier" );

	if( got ) {
		GlintModifier* ref = new GlintModifier( 2.0, 0.8, 1.0, 4.0,
			Vector3( 3, 1, 1 ), Vector3( 0.37, 0.11, -0.23 ), 7 );
		ref->addref();
		Check( FieldAlive( *got ), "parsed modifier field is alive" );
		Check( FieldsIdentical( *got, *ref, 20000 ),
			"parsed field bit-identical to direct ctor (no parameter-order/unit swap)" );
		ref->release();
	}
	job->release();
}

// ============================================================
//  2. Defaults
// ============================================================

static void TestDefaults()
{
	std::cout << "--- 2: name-only chunk gets the documented defaults ---" << std::endl;

	std::string s = SceneHeader();
	s += "glint_modifier\n{\nname gdef\n}\n";

	Job* job = new Job();
	job->addref();
	Check( ParseSceneFile( WriteTempScene( "defaults", s ), *job ), "name-only glint_modifier parses" );

	const GlintModifier* got = FetchGlint( *job, "gdef" );
	Check( got != 0, "default modifier registered" );
	if( got ) {
		GlintModifier* ref = new GlintModifier( 5.0, 0.15, 0.6, 2.0,
			Vector3( 1, 1, 1 ), Vector3( 0, 0, 0 ), 0 );
		ref->addref();
		Check( FieldsIdentical( *got, *ref, 20000 ), "defaults are 5 / 0.15 / 0.6 / 2deg / 1,1,1 / 0,0,0 / seed 0" );
		ref->release();
	}
	job->release();
}

// ============================================================
//  3. Loud diagnostics
// ============================================================

static void TestDiagnostics()
{
	std::cout << "--- 3: bad inputs fail the parse LOUDLY ---" << std::endl;

	struct Case { const char* tag; const char* line; };
	const Case bad[] = {
		{ "nan",       "coverage nan\n" },			// non-finite token -> descriptor layer rejects
		{ "inf",       "density inf\n" },			// non-finite token
		{ "dens0",     "density 0\n" },				// inert domain -> chunk diagnostic
		{ "negspread", "spread -1\n" },				// inert domain
		{ "scale0",    "scale 1 0 1\n" },			// zero stretch component
		{ "ovf_dens",  "density 1e999\n" },			// overflow: strtod ERANGE -> string-layer reject
		{ "ovf_cov",   "coverage 1e999\n" },		// overflow
	};

	for( unsigned int i = 0; i < sizeof(bad)/sizeof(bad[0]); i++ )
	{
		std::string s = SceneHeader();
		s += "glint_modifier\n{\nname gbad\n";
		s += bad[i].line;
		s += "}\n";

		Job* job = new Job();
		job->addref();
		const bool parsed = ParseSceneFile( WriteTempScene( bad[i].tag, s ), *job );
		std::string what = std::string( "`" ) + bad[i].line;
		what.erase( what.find_last_of( '\n' ) );
		what += "` fails the parse";
		Check( !parsed, what.c_str() );
		job->release();
	}
}

// ============================================================
//  4. Clamp semantics for over-range fractions
// ============================================================

static void TestClamps()
{
	std::cout << "--- 4: coverage/fill > 1 parse (warn) and behave as 1.0 ---" << std::endl;

	std::string s = SceneHeader();
	s += "glint_modifier\n{\nname gclamp\ndensity 2.0\ncoverage 1.5\nfill 2.0\nspread 4.0\nseed 7\n}\n";

	Job* job = new Job();
	job->addref();
	Check( ParseSceneFile( WriteTempScene( "clamp", s ), *job ), "over-range coverage/fill still parses (clamp semantics)" );

	const GlintModifier* got = FetchGlint( *job, "gclamp" );
	Check( got != 0, "clamped modifier registered" );
	if( got ) {
		GlintModifier* ref = new GlintModifier( 2.0, 1.0, 1.0, 4.0,
			Vector3( 1, 1, 1 ), Vector3( 0, 0, 0 ), 7 );
		ref->addref();
		Check( FieldsIdentical( *got, *ref, 20000 ), "coverage 1.5 / fill 2.0 behave exactly as 1.0 / 1.0" );
		ref->release();
	}
	job->release();
}

// ============================================================
//  5. Object attach (Modifier reference resolves)
// ============================================================

static void TestObjectAttach()
{
	std::cout << "--- 5: standard_object attaches the modifier by name ---" << std::endl;

	std::string s = SceneHeader();
	s += "glint_modifier\n{\nname gm\n}\n";
	s += "uniformcolor_painter\n{\nname gray\ncolor 0.5 0.5 0.5\n}\n";
	s += "lambertian_material\n{\nname mat\nreflectance gray\n}\n";
	s += "sphere_geometry\n{\nname sph\nradius 1.0\n}\n";
	s += "standard_object\n{\nname obj\ngeometry sph\nmaterial mat\nmodifier gm\n}\n";

	Job* job = new Job();
	job->addref();
	Check( ParseSceneFile( WriteTempScene( "attach", s ), *job ), "object + glint_modifier scene parses" );
	Check( FetchGlint( *job, "gm" ) != 0, "modifier resolvable after object attach" );
	job->release();
}

// ============================================================
//  Main
// ============================================================

int main()
{
	std::cout << "=== GlintModifier Scene-Parse Test ===" << std::endl;

	GlobalLog();

	TestPlumbThrough();
	TestDefaults();
	TestDiagnostics();
	TestClamps();
	TestObjectAttach();

	std::cout << "\n" << s_pass << " passed, " << s_fail << " failed" << std::endl;
	return ( s_fail == 0 ) ? 0 : 1;
}
