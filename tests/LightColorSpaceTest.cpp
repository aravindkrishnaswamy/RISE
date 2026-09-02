//////////////////////////////////////////////////////////////////////
//
//  LightColorSpaceTest.cpp - The four zero-area lights read `color` as
//    LINEAR Rec.709 by default, and honour an explicit `colorspace`.
//
//  WHAT THIS PINS (2026-09-02).  `omni_light`, `spot_light`,
//  `directional_light` and `ambient_light` used to gamma-DECODE their
//  authored `color` as sRGB -- `Job::Add*Light` wrapped the triple in
//  `sRGBPel(...)` -- so `color 1.0 0.2 0.2` lit the scene with
//  (1.0, 0.033, 0.033).  Nothing in the scene language said so, and the
//  SAME triple on a `uniformcolor_painter` (or an emissive material's
//  `exitance`) meant what it said.  Worse, the ANIMATION / EDITOR path
//  never decoded: `PointLight::KeyframeFromParameters("color")` builds a
//  `RISEPel(d)` straight from the digits, so a live colour edit rendered
//  differently from the same value reloaded off disk.
//
//  Three assertions per light kind, in rising order of teeth:
//
//    (a) DEFAULT IS LINEAR.  A parsed `color 1 0.2 0.2` with no
//        `colorspace` line lands on emissionColor() == (1, 0.2, 0.2)
//        EXACTLY.  This is the assertion the old sRGBPel decode fails --
//        it produced 0.0331048 in the G and B channels.
//
//    (b) `colorspace sRGB` STILL DECODES.  The reference is computed
//        INDEPENDENTLY here as `RISEPel( sRGBPel( ... ) )`, not read back
//        off a second light, so a bug in the shared conversion switch
//        cannot make both sides agree on a wrong answer.  This is what
//        `tools/migrate_scenes_light_colorspace.py` relies on to preserve
//        every pre-2026-09-02 scene's look.
//
//    (c) PARSE AND KEYFRAME NOW AGREE.  A light parsed with
//        `color 0.25 0.5 0.75` and a light whose colour is set through
//        `KeyframeFromParameters("color") + SetIntermediateValue` (the
//        editor / animation path) end on the same RISEPel.  Before the
//        fix these two paths disagreed by the sRGB transfer function --
//        the divergence that made a live edit and a reload differ.
//
//  MUTATION-VERIFIED: restoring the `sRGBPel(color)` wrap in ANY ONE of
//  the four Job::Add*Light methods turns that kind's (a) and (c) checks
//  red (its (b) check stays green -- (b) alone would not catch the bug).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/ILight.h"
#include "../src/Library/Interfaces/ILightManager.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/RString.h"

using namespace RISE;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const std::string& name )
{
	if( cond ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << name << std::endl;
	}
}

// EXACT for the linear rows (the parsed doubles must survive untouched);
// a tiny tolerance for the sRGB rows, where the reference goes through
// the same pow() but is computed independently.
static bool PelEq( const RISEPel& a, const RISEPel& b, double tol )
{
	return std::fabs( (double)a.r - (double)b.r ) <= tol
	    && std::fabs( (double)a.g - (double)b.g ) <= tol
	    && std::fabs( (double)a.b - (double)b.b ) <= tol;
}

static std::string PelStr( const RISEPel& p )
{
	char b[128];
	std::snprintf( b, sizeof(b), "(%.9g, %.9g, %.9g)",
		(double)p.r, (double)p.g, (double)p.b );
	return std::string( b );
}

//----------------------------------------------------------------------
// Load a scene body (version banner prepended) and hand back the job.
//----------------------------------------------------------------------
static IJobPriv* LoadScene( const std::string& body, const char* tag )
{
	char path[512];
	const char* tmp = std::getenv( "TMPDIR" );
	std::snprintf( path, sizeof(path), "%s/light_colorspace_%s_%d.RISEscene",
		( tmp && *tmp ) ? tmp : "/tmp", tag, (int)::getpid() );
	{
		std::ofstream ofs( path );
		if( !ofs.is_open() ) return nullptr;
		ofs << "RISE ASCII SCENE 7\n\n" << body;
	}

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		std::remove( path );
		return nullptr;
	}
	const bool ok = pJob->LoadAsciiSceneViaCst( path );
	std::remove( path );
	if( !ok ) {
		safe_release( pJob );
		return nullptr;
	}
	return pJob;
}

// Parse ONE light chunk and return its emissionColor().  `found` reports
// whether the chunk parsed and the light registered, so a parse failure
// is a named FAIL rather than a silent (0,0,0) that accidentally matches.
static RISEPel ColorOfParsedLight( const std::string& chunk, const char* tag, bool& found )
{
	found = false;
	RISEPel out( 0, 0, 0 );
	IJobPriv* pJob = LoadScene( chunk, tag );
	if( !pJob ) return out;

	ILightManager* lights = pJob->GetLights();
	const ILight* l = lights ? lights->GetItem( "L" ) : nullptr;
	if( l ) {
		out = l->emissionColor();
		found = true;
	}
	safe_release( pJob );
	return out;
}

//----------------------------------------------------------------------
// The four chunk kinds, each with the extra parameters it needs to be a
// well-formed chunk.  `%s` is where the colour block is spliced in.
//----------------------------------------------------------------------
struct LightKind
{
	const char* keyword;
	const char* extras;      // additional parameter lines (already tab-indented)
};

static const LightKind kKinds[] = {
	{ "omni_light",        "\tposition 0 5 0\n" },
	{ "spot_light",        "\tposition 0 5 0\n\ttarget 0 0 0\n\tinner 20\n\touter 40\n" },
	{ "directional_light", "\tdirection 0 1 0\n" },
	{ "ambient_light",     "" },
};

static std::string BuildChunk( const LightKind& k, const char* colorLines )
{
	return std::string( k.keyword ) + "\n{\n"
		+ "\tname L\n"
		+ "\tpower 1.0\n"
		+ k.extras
		+ colorLines
		+ "}\n";
}

//----------------------------------------------------------------------
// (a) + (b): default-linear, and explicit `colorspace sRGB`.
//----------------------------------------------------------------------
static void TestParsedColorSpaces()
{
	std::cout << "Test 1: `color` is LINEAR by default; `colorspace sRGB` decodes" << std::endl;

	// A deliberately chromatic triple with no component in {0,1}: every
	// channel moves under the sRGB transfer function, so a stray decode
	// anywhere is visible.  0.2 -> 0.0331048 is a 6x error, not a rounding
	// difference.
	const double kRGB[3] = { 1.0, 0.2, 0.2 };

	const RISEPel expectLinear( kRGB[0], kRGB[1], kRGB[2] );
	// Computed HERE, independently of Job's conversion switch.
	const RISEPel expectSRGB = RISEPel( sRGBPel( kRGB ) );

	// Guard the premise: the two references must actually differ, else
	// every assertion below is vacuous.
	Check( !PelEq( expectLinear, expectSRGB, 1e-4 ),
	       "PREMISE: linear and sRGB readings of (1, 0.2, 0.2) differ -- "
	       "linear " + PelStr( expectLinear ) + " vs sRGB " + PelStr( expectSRGB ) );

	for( const LightKind& k : kKinds ) {
		const std::string kw( k.keyword );

		// (a) No `colorspace` line -> LINEAR, exactly.
		{
			bool found = false;
			const std::string chunk = BuildChunk( k, "\tcolor 1.0 0.2 0.2\n" );
			const RISEPel got = ColorOfParsedLight( chunk, "lin", found );
			Check( found, kw + ": default-colourspace chunk parses and registers" );
			Check( found && PelEq( got, expectLinear, 0.0 ),
			       kw + ": `color 1.0 0.2 0.2` with no `colorspace` is LINEAR -- got "
			       + PelStr( got ) + ", want " + PelStr( expectLinear )
			       + " (a gamma-decode would give " + PelStr( expectSRGB ) + ")" );
		}

		// (b) `colorspace sRGB` -> the decoded triple.
		{
			bool found = false;
			const std::string chunk = BuildChunk( k, "\tcolor 1.0 0.2 0.2\n\tcolorspace sRGB\n" );
			const RISEPel got = ColorOfParsedLight( chunk, "srgb", found );
			Check( found, kw + ": `colorspace sRGB` chunk parses and registers" );
			Check( found && PelEq( got, expectSRGB, 1e-9 ),
			       kw + ": `colorspace sRGB` decodes -- got " + PelStr( got )
			       + ", want " + PelStr( expectSRGB ) );
		}

		// (b') `colorspace Rec709RGB_Linear` is the explicit spelling of
		//      the default, and must agree with omitting the line.
		{
			bool found = false;
			const std::string chunk = BuildChunk( k, "\tcolor 1.0 0.2 0.2\n\tcolorspace Rec709RGB_Linear\n" );
			const RISEPel got = ColorOfParsedLight( chunk, "rec709", found );
			Check( found && PelEq( got, expectLinear, 0.0 ),
			       kw + ": explicit `colorspace Rec709RGB_Linear` == omitting the line" );
		}
	}
}

//----------------------------------------------------------------------
// (c) The parsed path and the keyframe/editor path now agree.
//----------------------------------------------------------------------
static void TestParseAndKeyframeAgree()
{
	std::cout << "Test 2: a parsed `color` and a keyframe `color` edit agree" << std::endl;

	// Every component chromatic and off the transfer function's fixed
	// points, so the pre-fix disagreement would be large.
	const char* const kValue = "0.25 0.5 0.75";
	const RISEPel expect( 0.25, 0.5, 0.75 );

	for( const LightKind& k : kKinds ) {
		const std::string kw( k.keyword );

		// Parse a light with that colour...
		const std::string chunk = BuildChunk( k, "\tcolor 0.25 0.5 0.75\n" );
		IJobPriv* pJob = LoadScene( chunk, "kf" );
		Check( pJob != nullptr, kw + ": keyframe-comparison scene loads" );
		if( !pJob ) continue;

		ILightManager* lights = pJob->GetLights();
		ILightPriv* live = lights ? lights->GetItem( "L" ) : nullptr;
		Check( live != nullptr, kw + ": light registered for the keyframe comparison" );
		if( !live ) { safe_release( pJob ); continue; }

		const RISEPel parsed = live->emissionColor();
		Check( PelEq( parsed, expect, 0.0 ),
		       kw + ": parsed `color 0.25 0.5 0.75` is linear -- got " + PelStr( parsed ) );

		// ...then drive the SAME digits through the animation / editor
		// path.  Start from a different colour so a no-op setter cannot
		// pass by accident.
		{
			IKeyframeParameter* zero = live->KeyframeFromParameters(
				String( "color" ), String( "0 0 0" ) );
			Check( zero != nullptr, kw + ": KeyframeFromParameters(color, \"0 0 0\") accepted" );
			if( zero ) {
				live->SetIntermediateValue( *zero );
				zero->release();
			}
			Check( PelEq( live->emissionColor(), RISEPel( 0, 0, 0 ), 0.0 ),
			       kw + ": PRECONDITION -- the keyframe path really moved the colour to black" );
		}

		IKeyframeParameter* kf = live->KeyframeFromParameters(
			String( "color" ), String( kValue ) );
		Check( kf != nullptr, kw + ": KeyframeFromParameters(color) accepted the triple" );
		if( kf ) {
			live->SetIntermediateValue( *kf );
			kf->release();
		}
		const RISEPel keyed = live->emissionColor();

		Check( PelEq( keyed, parsed, 0.0 ),
		       kw + ": MONEY ASSERTION -- keyframe colour " + PelStr( keyed )
		       + " == parsed colour " + PelStr( parsed )
		       + "; a live edit and the same value reloaded off disk now agree" );

		safe_release( pJob );
	}
}

//----------------------------------------------------------------------
// An unknown colour-space name must be REFUSED, not silently defaulted.
//----------------------------------------------------------------------
static void TestUnknownColorSpaceRefused()
{
	std::cout << "Test 3: an unknown `colorspace` value is refused" << std::endl;

	for( const LightKind& k : kKinds ) {
		const std::string kw( k.keyword );
		const std::string chunk = BuildChunk( k, "\tcolor 1.0 0.2 0.2\n\tcolorspace Frobozz\n" );
		bool found = false;
		const RISEPel got = ColorOfParsedLight( chunk, "bad", found );
		(void)got;
		Check( !found,
		       kw + ": `colorspace Frobozz` does NOT register a light (a bogus space "
		       "must not silently fall back to a default reading)" );
	}
}

int main()
{
	std::cout << "LightColorSpaceTest" << std::endl;
	std::cout << "===================" << std::endl;

	TestParsedColorSpaces();
	TestParseAndKeyframeAgree();
	TestUnknownColorSpaceRefused();

	std::cout << "=== LightColorSpaceTest: " << passCount << " passed, "
	          << failCount << " failed ===" << std::endl;
	return failCount == 0 ? 0 : 1;
}
