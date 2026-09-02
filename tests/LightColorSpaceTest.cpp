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

#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/ILight.h"
#include "../src/Library/Interfaces/ILightManager.h"
#include "../src/Library/SceneEditor/LightIntrospection.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
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

//----------------------------------------------------------------------
// (d) THE EDITOR ROUND-TRIP.  A colour edit on a `colorspace sRGB` chunk
//     must not double-decode; it must be ATOMIC; and undo must put the
//     chunk back BYTE-IDENTICALLY, spelling included.
//
// WHY THIS IS A SEPARATE HAZARD from everything above.  A CST-routed light
// edit (SceneEditor::ApplyForwardMutation's SetLightProperty arm) writes the
// PROPERTY PANEL's value into the chunk and re-derives.  The panel's value is
// `ILight::emissionColor()` -- already-converted, LINEAR.  The chunk's
// `colorspace sRGB` line is not touched by a plain colour write, so the
// re-derive gamma-DECODES the linear digits a SECOND time: nudging a swatch
// reading (1, 0.0331, 0.0194) up to (1, 0.05, 0.02) lands the light on
// (1, 0.0039, 0.0016) -- an order of magnitude the wrong way, compounding on
// every further nudge.  Every check in Tests 1-3 passes with that bug
// present, because none of them goes through the editor.
//
// The fix writes `colorspace Rec709RGB_Linear` AND the digits as ONE atomic
// Document edit (SceneEditor::RouteCstLightColorComposite_ ->
// Job::ApplyCstParamEdits), which is why the assertions below are about the
// light's colour, the chunk TEXT, and what happens when a part is refused.
//
// THE UNDO HALF IS NOT COSMETIC.  Undo restores both original texts verbatim,
// so the chunk comes back spelling `colorspace sRGB`.  It has to: an AGENT
// history entry captures RAW CHUNK TEXT (SceneEditController::
// CaptureAgentPriorParamValue_), while a panel entry captures the DECODED
// live value.  If undo left the chunk linear, undoing an OLDER agent colour
// edit afterwards would replay sRGB digits under a linear chunk and land the
// light ~6x too bright -- Test 8 is exactly that sequence.
//
// MUTATION-VERIFIED (round 2, both runs actually performed):
//   * Reverting SceneEditor::ApplyRevertMutation's SetLightProperty arm to the
//     pre-round-2 behaviour (restore the decoded colour, leave the chunk
//     spelled `Rec709RGB_Linear`) turns THREE of this test's checks red -- the
//     byte-identity one, the verbatim-digits one, and the introspection row --
//     plus three more in Test 8.
//   * Replacing the atomic Job::ApplyCstParamEdits call with the pre-round-2
//     pair of sequential ApplyCstParamEdit calls turns SIX of Test 5's checks
//     red (both malformed values and the duplicate-`color` scene each leave a
//     half-converted chunk and a light sitting on the authored digits read as
//     linear, (1, 0.2, 0.15) instead of (1, 0.0331, 0.0196)).
//----------------------------------------------------------------------

// A `colorspace sRGB` light -- i.e. exactly what
// `tools/migrate_scenes_light_colorspace.py` left behind on 581 colour lines
// -- plus the minimum around it for a clean CST derive + re-derive.
static const char* const kEditorScene =
	"omni_light\n{\nname L\npower 4\ncolor 1.0 0.2 0.15\ncolorspace sRGB\nposition 0 5 0\n}\n"
	"uniformcolor_painter\n{\nname p1\ncolor 1 0 0\n}\n"
	"lambertian_material\n{\nname m\nreflectance p1\n}\n"
	"sphere_geometry\n{\nname g\nradius 1\n}\n"
	"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n";

// The same light with a DOUBLE `color` line.  The scene still derives (the
// parse is last-wins), but the editor must REFUSE to write it: an occ=0 write
// would rewrite the dead first line and leave the live value alone.
static const char* const kDupColorScene =
	"omni_light\n{\nname L\npower 4\ncolor 0.9 0.8 0.7\ncolor 1.0 0.2 0.15\ncolorspace sRGB\nposition 0 5 0\n}\n"
	"uniformcolor_painter\n{\nname p1\ncolor 1 0 0\n}\n"
	"lambertian_material\n{\nname m\nreflectance p1\n}\n"
	"sphere_geometry\n{\nname g\nradius 1\n}\n"
	"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n";

static RISEPel LiveLightColor( IJobPriv& job )
{
	ILightManager* lm = job.GetLights();
	const ILight* l = lm ? lm->GetItem( "L" ) : nullptr;
	return l ? l->emissionColor() : RISEPel( -1, -1, -1 );
}

static std::string LiveDocText( IJobPriv& job )
{
	const RISE::Cst::Document* d = job.GetCstDocument();
	return d ? RISE::Cst::SerializeCst( *d ) : std::string();
}

// The `colorspace` row LightIntrospection reports for light "L" (empty if
// there is no such row).
static std::string IntrospectedColorSpace( IJobPriv& job, const RISE::Cst::Document* doc )
{
	ILightManager* lm = job.GetLights();
	const ILight* l = lm ? lm->GetItem( "L" ) : nullptr;
	if( !l ) return std::string();
	const std::vector<CameraProperty> rows =
		LightIntrospection::Inspect( String( "L" ), *l, doc );
	for( size_t i = 0; i < rows.size(); ++i )
		if( rows[i].name == String( "colorspace" ) ) return std::string( rows[i].value.c_str() );
	return std::string();
}

static void TestEditorColorEditOnSRGBChunk()
{
	std::cout << "Test 4: a CST-routed colour edit on a `colorspace sRGB` light" << std::endl;

	const double kAuthored[3] = { 1.0, 0.2, 0.15 };
	// The light's colour as the SCENE FILE means it: the authored digits
	// decoded through the sRGB transfer function.  Computed here, not read
	// back off the renderer.
	const RISEPel expectOriginal = RISEPel( sRGBPel( kAuthored ) );
	// What the user asks for when they nudge the panel's swatch.  The panel
	// speaks LINEAR, so this is the light's colour verbatim.
	const RISEPel expectEdited( 1.0, 0.05, 0.02 );

	IJobPriv* pJob = LoadScene( kEditorScene, "edit" );
	Check( pJob != nullptr, "editor scene loads via the CST path" );
	if( !pJob ) return;
	Check( pJob->HasRetainedCstDocument(), "the editor scene retains a CST Document (else the edit is not CST-routed)" );

	const std::string docOriginal = LiveDocText( *pJob );
	Check( docOriginal.find( "color 1.0 0.2 0.15" ) != std::string::npos
	    && docOriginal.find( "colorspace sRGB" ) != std::string::npos,
	       "PRECONDITION: the loaded Document really carries the authored `color` + `colorspace sRGB` lines" );

	Check( PelEq( LiveLightColor( *pJob ), expectOriginal, 1e-9 ),
	       "PRECONDITION: the `colorspace sRGB` light loads DECODED -- got "
	       + PelStr( LiveLightColor( *pJob ) ) + ", want " + PelStr( expectOriginal ) );
	Check( IntrospectedColorSpace( *pJob, pJob->GetCstDocument() ) == "sRGB",
	       "the panel's `colorspace` row reports the chunk's ACTUAL value (`sRGB`), not a hard-coded default" );
	Check( IntrospectedColorSpace( *pJob, nullptr ) == "Rec709RGB_Linear",
	       "with no Document (an API-constructed light) the row falls back to the language default" );

	{
		SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
		c.SetSelection( SceneEditController::Category::Light, String( "L" ) );
		Check( c.SetPropertyForCategory( SceneEditController::Category::Light,
		                                 String( "color" ), String( "1 0.05 0.02" ) ),
		       "the colour edit applies" );

		const double kEditedDigits[3] = { 1.0, 0.05, 0.02 };
		const RISEPel doubleDecoded = RISEPel( sRGBPel( kEditedDigits ) );
		const RISEPel edited = LiveLightColor( *pJob );
		Check( PelEq( edited, expectEdited, 0.0 ),
		       "MONEY ASSERTION -- the edited light is EXACTLY what the panel asked for: got "
		       + PelStr( edited ) + ", want " + PelStr( expectEdited )
		       + " (a second sRGB decode would give " + PelStr( doubleDecoded ) + ")" );

		const std::string docEdited = LiveDocText( *pJob );
		Check( docEdited.find( "Rec709RGB_Linear" ) != std::string::npos,
		       "the chunk now carries `colorspace Rec709RGB_Linear`" );
		Check( docEdited.find( "sRGB" ) == std::string::npos,
		       "...and no longer carries `colorspace sRGB` (the conversion is a replacement, not an addition)" );
		Check( IntrospectedColorSpace( *pJob, pJob->GetCstDocument() ) == "Rec709RGB_Linear",
		       "the panel's `colorspace` row follows the chunk to linear" );

		Check( c.Editor().Undo(), "undo of the colour edit returns true" );
		const RISEPel reverted = LiveLightColor( *pJob );
		Check( PelEq( reverted, expectOriginal, 1e-6 ),
		       "undo restores the ORIGINAL DECODED colour -- got " + PelStr( reverted )
		       + ", want " + PelStr( expectOriginal )
		       + " (a revert written back under the old `colorspace sRGB` would decode it AGAIN)" );

		// (a) The BYTE-IDENTITY half.  Not "the value came back" -- the whole
		// chunk came back, `colorspace sRGB` spelling included.  Test 7 is why.
		const std::string docUndone = LiveDocText( *pJob );
		Check( docUndone == docOriginal,
		       "undo leaves the Document BYTE-IDENTICAL to the pre-edit text (the `colorspace sRGB` "
		       "spelling is restored, not just the colour value)" );
		Check( docUndone.find( "color 1.0 0.2 0.15" ) != std::string::npos,
		       "...spelling the ORIGINAL colour digits verbatim (`1.0`, not the `%g`-formatted live value)" );
		Check( IntrospectedColorSpace( *pJob, pJob->GetCstDocument() ) == "sRGB",
		       "...and the panel's `colorspace` row follows the chunk back to sRGB" );

		// Redo re-applies the forward PAIR (the composite is recomputed from
		// the Document, which the undo above put back into the sRGB state).
		Check( c.Editor().Redo(), "redo of the colour edit returns true" );
		const RISEPel redone = LiveLightColor( *pJob );
		Check( PelEq( redone, expectEdited, 0.0 ),
		       "redo lands the edited colour again EXACTLY -- got " + PelStr( redone ) );
		const std::string docRedone = LiveDocText( *pJob );
		Check( docRedone.find( "Rec709RGB_Linear" ) != std::string::npos
		    && docRedone.find( "sRGB" ) == std::string::npos,
		       "redo re-converts the chunk to `colorspace Rec709RGB_Linear`" );
	}

	safe_release( pJob );
}

//----------------------------------------------------------------------
// Test 5: ATOMICITY.  The composite is TWO param writes; if either half
// cannot land, NOTHING may change.
//
// The pre-round-2 implementation issued them as two separate
// Job::ApplyCstParamEdit calls, each re-deriving with no rollback between
// them.  A refused colour write therefore left the chunk converted to
// `Rec709RGB_Linear` with the AUTHORED sRGB digits still in place: the light
// jumped to those digits read as LINEAR (~6x too bright on a mid-tone),
// nothing was pushed to history, and a re-render was kicked on a state no
// undo could reach.  Both malformed values below reach the Document write and
// are caught by the re-derive, which is exactly the window that bug lived in.
//----------------------------------------------------------------------
static void CheckColorEditRefusedChangesNothing( const char* sceneText, const char* tag,
                                                 const char* badValue, const RISEPel& expectColor,
                                                 const std::string& why )
{
	IJobPriv* pJob = LoadScene( sceneText, tag );
	Check( pJob != nullptr, std::string( "atomicity scene loads (" ) + why + ")" );
	if( !pJob ) return;

	const std::string docBefore = LiveDocText( *pJob );
	const RISEPel colorBefore = LiveLightColor( *pJob );
	Check( PelEq( colorBefore, expectColor, 1e-9 ),
	       std::string( "PRECONDITION (" ) + why + "): the light loads at " + PelStr( expectColor )
	       + ", got " + PelStr( colorBefore ) );

	{
		SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
		c.SetSelection( SceneEditController::Category::Light, String( "L" ) );
		const unsigned int depthBefore = c.Editor().History().UndoDepth();

		const bool applied = c.SetPropertyForCategory(
			SceneEditController::Category::Light, String( "color" ), String( badValue ) );
		Check( !applied, std::string( "the edit is REFUSED (" ) + why + ")" );
		Check( LiveDocText( *pJob ) == docBefore,
		       std::string( "...the Document is BYTE-IDENTICAL afterwards (" ) + why
		       + ") -- no half-applied `colorspace` conversion" );
		Check( PelEq( LiveLightColor( *pJob ), colorBefore, 0.0 ),
		       std::string( "...the light did not move (" ) + why + "): got "
		       + PelStr( LiveLightColor( *pJob ) ) + ", want " + PelStr( colorBefore ) );
		Check( c.Editor().History().UndoDepth() == depthBefore,
		       std::string( "...and no history entry was pushed (" ) + why + ")" );
	}

	safe_release( pJob );
}

static void TestEditorColorEditAtomicity()
{
	std::cout << "Test 5: a refused colour edit on an sRGB chunk changes NOTHING" << std::endl;

	const double kAuthored[3] = { 1.0, 0.2, 0.15 };
	const RISEPel expectOriginal = RISEPel( sRGBPel( kAuthored ) );

	// (b) MALFORMED VALUES.  Both write into the Document copy fine and are
	// caught by the RE-DERIVE, so they exercise exactly the window the
	// two-call implementation left open: the `colorspace` half already
	// committed (and re-derived) when the colour half is refused.
	//
	// NOT USED HERE, and worth naming: a SHORT triple (`color 1 0.05`) is NOT
	// refused -- the chunk parser zero-fills the missing component, so that
	// edit legitimately applies and lands the light on (1, 0.05, 0).  It is a
	// valid edit, not a malformed one, so it cannot pin atomicity.
	CheckColorEditRefusedChangesNothing( kEditorScene, "atomabc", "abc", expectOriginal,
	                                     "not a number" );
	CheckColorEditRefusedChangesNothing( kEditorScene, "atomnan", "1 nan 0.02", expectOriginal,
	                                     "a non-finite component" );

	// (c) A DOUBLED `color` LINE.  Refused by the duplicate-occurrence guard
	// BEFORE anything is written -- the guard now runs over EVERY param of the
	// batch up front, which is what makes the refusal total rather than partial.
	// The live colour is the LAST line's (the parse is last-wins).
	CheckColorEditRefusedChangesNothing( kDupColorScene, "atomdup", "1 0.05 0.02", expectOriginal,
	                                     "the chunk spells `color` twice" );
}

//----------------------------------------------------------------------
// Test 7: the introspection row for a light the Document cannot resolve.
//
// Round-2 review: reporting `Rec709RGB_Linear` for an unresolvable or
// ambiguous name is a claim with no basis -- the light exists, the Document
// simply cannot say which chunk built it.  The no-Document case is different
// and keeps the default: an API-constructed light really WAS built linear.
//----------------------------------------------------------------------
// A `colorspace sRGB` light with NO `color` line -- the shape whose colour the
// panel's edit INSERTS rather than replaces.  The double-decode hazard is the
// same (linear digits landing under a non-linear reading), so the composite
// must fire here too; the light's pre-edit colour is the descriptor default
// `0 0 0`, which is what undo restores (as an explicit line -- see
// SceneEditor::LightColorCompositeState_'s doc).
static const char* const kNoColorLineScene =
	"omni_light\n{\nname L\npower 4\ncolorspace sRGB\nposition 0 5 0\n}\n"
	"uniformcolor_painter\n{\nname p1\ncolor 1 0 0\n}\n"
	"lambertian_material\n{\nname m\nreflectance p1\n}\n"
	"sphere_geometry\n{\nname g\nradius 1\n}\n"
	"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n";

static void TestEditorColorEditInsertsColorLine()
{
	std::cout << "Test 6: a colour edit on a `colorspace sRGB` chunk that has NO `color` line" << std::endl;

	IJobPriv* pJob = LoadScene( kNoColorLineScene, "nocolor" );
	Check( pJob != nullptr, "the no-`color` scene loads" );
	if( !pJob ) return;

	const RISEPel expectBlack( 0.0, 0.0, 0.0 );
	Check( PelEq( LiveLightColor( *pJob ), expectBlack, 0.0 ),
	       "PRECONDITION: with no `color` line the light is the descriptor default (0,0,0) -- got "
	       + PelStr( LiveLightColor( *pJob ) ) );

	{
		SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
		c.SetSelection( SceneEditController::Category::Light, String( "L" ) );
		Check( c.SetPropertyForCategory( SceneEditController::Category::Light,
		                                 String( "color" ), String( "1 0.05 0.02" ) ),
		       "the colour edit applies (inserting a `color` line)" );
		const RISEPel expectEdited( 1.0, 0.05, 0.02 );
		Check( PelEq( LiveLightColor( *pJob ), expectEdited, 0.0 ),
		       "the INSERTED colour is EXACTLY what the panel asked for -- got "
		       + PelStr( LiveLightColor( *pJob ) ) + " (the composite converted the chunk; without it "
		       "the inserted digits would decode under the surviving `colorspace sRGB`)" );
		Check( LiveDocText( *pJob ).find( "Rec709RGB_Linear" ) != std::string::npos,
		       "...and the chunk converted to the linear convention" );

		Check( c.Editor().Undo(), "undo returns true" );
		Check( PelEq( LiveLightColor( *pJob ), expectBlack, 0.0 ),
		       "undo restores the light to its pre-edit (default, black) colour -- got "
		       + PelStr( LiveLightColor( *pJob ) ) );
		const std::string docUndone = LiveDocText( *pJob );
		Check( docUndone.find( "colorspace sRGB" ) != std::string::npos,
		       "...and the `colorspace sRGB` spelling is back" );
		Check( docUndone.find( "color 0 0 0" ) != std::string::npos,
		       "...with the default colour now spelled explicitly (the documented non-byte-identical case: "
		       "restoring an ABSENT param would need a REMOVE, and `0 0 0` is a fixed point of every "
		       "colour space, so the light is restored exactly)" );
	}
	safe_release( pJob );
}

static void TestIntrospectionUnknownColorSpace()
{
	std::cout << "Test 7: the `colorspace` row is honest about what it cannot resolve" << std::endl;

	IJobPriv* pJob = LoadScene( kEditorScene, "unk" );
	Check( pJob != nullptr, "introspection scene loads" );
	if( !pJob ) return;

	ILightManager* lm = pJob->GetLights();
	const ILight* l = lm ? lm->GetItem( "L" ) : nullptr;
	Check( l != nullptr, "the light resolves in the manager" );
	if( l ) {
		// A name that is NOT in the Document, asked WITH a Document.
		const std::vector<CameraProperty> rows =
			LightIntrospection::Inspect( String( "no_such_light" ), *l, pJob->GetCstDocument() );
		std::string cs;
		for( size_t i = 0; i < rows.size(); ++i )
			if( rows[i].name == String( "colorspace" ) ) cs = std::string( rows[i].value.c_str() );
		Check( cs == "(unknown)",
		       "an unresolvable light name reports `(unknown)`, not an asserted `Rec709RGB_Linear` -- got `"
		       + cs + "`" );
	}
	safe_release( pJob );
}

//----------------------------------------------------------------------
// Test 8: SHARED UNDO -- an AGENT colour edit, then a PANEL colour edit,
//         then two undos.
//
// THE BUG THIS PINS.  The two history entry kinds capture different things:
// an agent entry captures the chunk's RAW TEXT (SceneEditController::
// CaptureAgentPriorParamValue_ -> Cst::ParamValueAtOccurrence), a panel entry
// captures the light's DECODED live value.  If the panel edit's undo restored
// only the colour and left the chunk spelled `Rec709RGB_Linear`, the SECOND
// undo would replay the agent entry's sRGB digits under a linear chunk and
// the light would land at the authored numbers instead of their decode --
// (1, 0.2, 0.15) instead of (1, 0.0331, 0.0194), ~6x too bright in G and B.
//
// MUTATION-VERIFIED (round 2, run performed): reverting
// SceneEditor::ApplyRevertMutation's SetLightProperty arm to the pre-round-2
// behaviour (restore the colour only, leave the chunk linear) turns THREE of
// this test's assertions red -- undo #1's byte-identity, the money assertion
// (which lands on exactly (1, 0.2, 0.15), the authored digits read as linear),
// and the final Document comparison -- while Test 4's byte-identity assertions
// go red too.
//----------------------------------------------------------------------
static void TestSharedUndoAgentThenPanel()
{
	std::cout << "Test 8: agent colour edit + panel colour edit, undone in LIFO order" << std::endl;

	const double kAuthored[3] = { 1.0, 0.2, 0.15 };
	const RISEPel expectOriginal = RISEPel( sRGBPel( kAuthored ) );
	// What the AGENT writes: scene-file digits, read through the chunk's own
	// (still sRGB) convention.
	const double kAgentDigits[3] = { 0.5, 0.4, 0.3 };
	const RISEPel expectAgent = RISEPel( sRGBPel( kAgentDigits ) );

	IJobPriv* pJob = LoadScene( kEditorScene, "shared" );
	Check( pJob != nullptr, "shared-undo scene loads" );
	if( !pJob ) return;

	const std::string docOriginal = LiveDocText( *pJob );

	{
		SceneEditController c( *pJob, /*interactiveRasterizer*/0 );

		// --- 1. The AGENT edits `color` through the CST path.  The chunk is
		//        still sRGB, so the digits it writes are sRGB digits, and the
		//        history entry it pushes captures the ORIGINAL sRGB digits.
		const SceneEditController::AgentCommitResult r = c.ApplyAgentParamEdit(
			String( "L" ), String( "omni_light" ), String( "color" ),
			String( "0.5 0.4 0.3" ), /*baseVersionOrNull*/ nullptr );
		Check( r.applied, std::string( "the agent colour commit applies (status `" )
		                  + r.status.c_str() + "`)" );
		Check( PelEq( LiveLightColor( *pJob ), expectAgent, 1e-9 ),
		       "the agent's digits decode through the chunk's sRGB convention -- got "
		       + PelStr( LiveLightColor( *pJob ) ) + ", want " + PelStr( expectAgent ) );
		const std::string docAfterAgent = LiveDocText( *pJob );
		Check( docAfterAgent.find( "colorspace sRGB" ) != std::string::npos,
		       "the agent edit did NOT convert the chunk (only a `color` edit through the "
		       "editor's own composite does that)" );

		// --- 2. The USER nudges the swatch.  This is the composite: the chunk
		//        converts to linear and takes the panel's linear digits.
		c.SetSelection( SceneEditController::Category::Light, String( "L" ) );
		Check( c.SetPropertyForCategory( SceneEditController::Category::Light,
		                                 String( "color" ), String( "1 0.05 0.02" ) ),
		       "the panel colour edit applies on top of the agent edit" );
		Check( LiveDocText( *pJob ).find( "Rec709RGB_Linear" ) != std::string::npos,
		       "...converting the chunk to the linear convention" );

		// --- 3. Undo the PANEL edit.  The chunk must come back EXACTLY as the
		//        agent left it -- sRGB spelling and the agent's digits.
		Check( c.Editor().Undo(), "undo #1 (the panel edit) returns true" );
		Check( LiveDocText( *pJob ) == docAfterAgent,
		       "undo #1 restores the post-agent Document BYTE-IDENTICALLY (sRGB spelling included)" );
		Check( PelEq( LiveLightColor( *pJob ), expectAgent, 1e-6 ),
		       "undo #1 puts the light back on the agent's decoded colour -- got "
		       + PelStr( LiveLightColor( *pJob ) ) );

		// --- 4. Undo the AGENT edit.  Its captured prior value is RAW sRGB
		//        TEXT; it can only be replayed correctly because step 3 put the
		//        chunk back into the sRGB convention.
		Check( c.Editor().Undo(), "undo #2 (the agent edit) returns true" );
		const RISEPel finalColor = LiveLightColor( *pJob );
		Check( PelEq( finalColor, expectOriginal, 1e-6 ),
		       "MONEY ASSERTION -- after both undos the light is back on its ORIGINAL DECODED colour: got "
		       + PelStr( finalColor ) + ", want " + PelStr( expectOriginal )
		       + " (leaving the chunk linear across undo #1 would land it on the authored digits "
		         "read as linear, ~6x too bright)" );
		Check( LiveDocText( *pJob ) == docOriginal,
		       "...and the Document is byte-identical to the originally-loaded text" );
	}

	safe_release( pJob );
}

int main()
{
	std::cout << "LightColorSpaceTest" << std::endl;
	std::cout << "===================" << std::endl;

	TestParsedColorSpaces();
	TestParseAndKeyframeAgree();
	TestUnknownColorSpaceRefused();
	TestEditorColorEditOnSRGBChunk();
	TestEditorColorEditAtomicity();
	TestEditorColorEditInsertsColorLine();
	TestIntrospectionUnknownColorSpace();
	TestSharedUndoAgentThenPanel();

	std::cout << "=== LightColorSpaceTest: " << passCount << " passed, "
	          << failCount << " failed ===" << std::endl;
	return failCount == 0 ? 0 : 1;
}
