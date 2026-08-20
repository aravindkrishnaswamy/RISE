//////////////////////////////////////////////////////////////////////
//
//  PainterIntrospectionRoundTripTest.cpp - doc 88 S4 + S4b: prove a painter's
//    own parameters round-trip -- enumerated as typed rows, edited
//    through the ONE CST edit pathway, persisted to the retained
//    Document, marked dirty in a first-class channel, and reverted by
//    an INVERSE PATCH through shared undo/redo.
//
//  This is the headless proof of the whole G7 gap (MATERIAL_EDITOR.md
//  §2.1: "a material's own scalar-literal slot round-trips today but a
//  painter chunk's value does not").  The SwiftUI / Qt panels are
//  manual-verify-only; everything they can do is driven here through
//  the same C++ surface they call (SceneEditController::
//  SetPropertyForCategory(Category::Painter) and the per-category
//  property snapshot), against a LIVE controller with a real render
//  thread -- the AgentLiveCommitTest pattern -- so the cancel-and-park,
//  re-derive, rebind, dirty and undo behaviour is exercised for real
//  rather than simulated.
//
//  Coverage map (one test function each):
//    1  Inspect rows: uniformcolor / perlin3d / expression / ramp,
//       including the pipe row, the repeatable-occurrence rows, and the
//       ExpressionParamSpec min/max/step/label merge.
//    2  Scalar param edit -> Document updated, live scene re-derived,
//       dirty set in the (Painter, name) channel.
//    3  Expression BODY edit -> full re-derive, program recompiled
//       (the painter evaluates to different colours afterwards).
//    4  INVALID expression body -> clean refusal, Document byte-
//       identical, dirty untouched.
//    5  Undo restores the prior value in the Document AND the live
//       scene; Redo re-applies.  Dirty correct in both directions.
//    6  Edit refused while an editor transaction is open.
//    7  Multi-token value (`color r g b`) captures + restores the FULL
//       token join, not the first token.
//    8  Save -> Undo -> Redo: HasUnsavedChanges correct at every step
//       (the historical "Redo after Save left it false" data-loss).
//    9  Occurrence row names are refused by the edit route.
//    10 The rows arrive at the per-category property SNAPSHOT the two
//       GUI bridges read (both are category-generic and already carry
//       Painter = 10, so this is what "the bridges need no change"
//       rests on -- measured, not read off the code).
//    11 Undo of an expression BODY edit restores the exact original
//       Document bytes AND re-derives a program that evaluates
//       identically to the pre-edit one (review round 1, P3-b #1).
//    12 scalar_painter{expression} panel parity: pipe row reports
//       `scalar`, the body row is editable, an edit applies/dirties/
//       undoes exactly like the colour-pipe expression painter (review
//       round 1, P3-b #2).
//    13 A BARE repeatable-role name (`stop`, `param` -- no "[i]"
//       suffix) is refused by the same route that refuses the
//       bracketed occurrence rows, with a non-repeatable control param
//       on the same chunk still applying (review round 1, P2-a).
//
//  doc 88 S4b (occurrence-addressed editing + slider row metadata) adds:
//    9  (rewritten) the BOUNDARY of the bracketed vocabulary -- out of
//       range, malformed index, bracket on a non-repeatable param.
//    14 `stop[1]` rewrites EXACTLY that line, byte-exactly, and the live
//       painter changes.
//    15 `param[1]` value edit: evaluation changes, `param[0]` and its
//       ParamSpec metadata untouched.
//    16 `def[0]` edit recompiles; an INVALID def body is refused with the
//       Document byte-identical.
//    17 Undo/Redo at an occurrence restores the right LINE, whole.
//    18 The DRIFT GUARD: an occurrence layout that moved under a pending
//       Undo produces an honest refusal, not a clobbered neighbour.
//    19 Row range metadata (hasRange/min/max/step) through both the
//       introspection rows and the per-category snapshot.
//    20 (round-1 P1) The drift guard's WHITESPACE NORMALISATION: a
//       column-aligned original / a slider-style commit that resends
//       untouched bytes verbatim must not read as drift, while a real
//       token-level value change still must.
//
//  Self-contained: no RISE_MEDIA_PATH, an inline native-v7 scene, and a
//  mock DoOneRenderPass so the render thread cycles without a rasterizer.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <string>
#include <fstream>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/Interfaces/IScalarPainterManager.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/SceneEditor/PainterIntrospection.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Intersection/RayIntersection.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0, failCount = 0;
static void Check( bool c, const char* n )
{
	if( c ) { ++passCount; }
	else    { ++failCount; std::cout << "  FAIL: " << n << std::endl; }
}

//////////////////////////////////////////////////////////////////////
// A controller with a mock render pass, so the cancel-and-park path an
// edit takes is exercised against a thread that is really in flight.
//////////////////////////////////////////////////////////////////////
class TestController : public SceneEditController
{
public:
	explicit TestController( IJobPriv& job, unsigned int simulatedRenderMs = 5 )
	: SceneEditController( job, /*interactiveRasterizer*/0 )
	, mSimulatedRenderMs( simulatedRenderMs )
	{}

protected:
	void DoOneRenderPass() override
	{
		// Simulate render work in cancel-checked slices so an edit that
		// cancel-and-parks is observable and a race would surface as a crash.
		const unsigned int slices = 5;
		for( unsigned int i = 0; i < slices; ++i ) {
			if( IsCancelRequested() ) return;
			std::this_thread::sleep_for(
				std::chrono::milliseconds( mSimulatedRenderMs / slices + 1 ) );
		}
	}

private:
	unsigned int mSimulatedRenderMs;
};

//////////////////////////////////////////////////////////////////////
// Fixture scene.  Deliberately covers all four painter shapes the
// slice claims: a multi-token colour literal, a numeric-parameter noise
// painter, an expression painter with BOTH a metadata-carrying `param`
// and a bare one, and a ramp with repeated `stop` lines.
//////////////////////////////////////////////////////////////////////
static const char* kScene =
	"RISE ASCII SCENE 7\n"
	"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
	"uniformcolor_painter\n{\nname basecol\ncolor 0.25 0.5 0.75\n}\n"
	"perlin3d_painter\n{\nname noise\ncolora white\ncolorb basecol\noctaves 4\npersistence 0.5\nscale 2 2 2\n}\n"
	"expression_painter\n{\nname ex\n"
		"param ring_scale 4.0 min 0.5 max 20 step 0.25 label \"Ring density\"\n"
		"param wob 0.25\n"
		"def rings clamp(0.5+0.5*sin(P.x*ring_scale), 0, 1)\n"
		"expr mix(vec3(0,0,0), vec3(1,1,1), rings)\n"
		"seed 3.0\ntime 0.0\n}\n"
	"ramp_painter\n{\nname rmp\ninput noise\nchannel R\ninterpolation linear\n"
		"stop 0.0 0.1 0.2 0.3\nstop 1.0 0.9 0.8 0.7\n}\n"
	// doc 88 S4b: a THREE-stop ramp -- the occurrence-addressed edit tests need
	// a middle occurrence (so a write to [1] has a neighbour on each side to
	// prove untouched) and the drift tests need a layout that can lose a line
	// and still have an occurrence 1.
	"ramp_painter\n{\nname rmp3\ninput noise\nchannel R\ninterpolation linear\n"
		// Values deliberately DISTINCT from `rmp`'s: the occurrence tests probe the
		// serialized Document by substring, and shared literals would make "stop 0
		// is untouched" pass on the other ramp's line.
		"stop 0.0 0.11 0.21 0.31\nstop 0.5 0.44 0.54 0.64\nstop 1.0 0.91 0.81 0.71\n}\n"
	"scalar_painter\n{\nname sp_rough\nexpression 0.3\n}\n"
	"lambertian_luminaire_material\n{\nname lum\nexitance basecol\nscale 5.0\nmaterial none\n}\n"
	"sphere_geometry\n{\nname s\nradius 1\n}\n"
	"standard_object\n{\nname obj\ngeometry s\nmaterial lum\n}\n";

static Job* LoadScene( const char* text, const char* tmpPath )
{
	{ std::ofstream o( tmpPath, std::ios::binary ); o << text; }
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( tmpPath ) ) {
		pJob->release();
		std::remove( tmpPath );
		return nullptr;
	}
	return pJob;
}

//! The whole retained Document serialized -- the honest "did the scene
//! TEXT change" probe (this is byte-for-byte what a save would write).
static std::string DocText( Job& j )
{
	const RISE::Cst::Document* d = j.GetCstDocument();
	return d ? RISE::Cst::SerializeCst( *d ) : std::string();
}

//! One row out of the painter property snapshot, by name.  Returns false
//! when the row is absent -- which is itself an assertion many tests make.
static bool RowFor( const std::vector<CameraProperty>& rows, const char* name,
                    CameraProperty& out )
{
	for( std::size_t i = 0; i < rows.size(); ++i ) {
		if( rows[i].name == String( name ) ) { out = rows[i]; return true; }
	}
	return false;
}

static std::vector<CameraProperty> InspectPainter( Job& j, const char* name )
{
	return PainterIntrospection::Inspect( j.GetCstDocument(), j, String( name ) );
}

//! A colour painter's value at a fixed synthetic shading point.  Used to
//! prove an edit reached the LIVE managers (not just the Document) and --
//! for the expression body -- that the VM program was really recompiled.
static bool PainterColorAt( Job& j, const char* name, double x, RISEPel& out )
{
	IPainterManager* pm = j.GetPainters();
	IPainter* p = pm ? pm->GetItem( name ) : 0;
	if( !p ) return false;
	RayIntersectionGeometric ri( Ray(), nullRasterizerState );
	ri.bHit = true;
	ri.ptIntersection = Point3( x, 0.31, -0.77 );
	ri.ptObjIntersec  = ri.ptIntersection;
	ri.ptCoord        = Point2( 0.25, 0.6 );
	ri.vNormal        = Vector3( 0, 1, 0 );
	out = p->GetColor( ri );
	return true;
}

static bool DescriptionContains( const CameraProperty& row, const char* needle )
{
	return std::string( row.description.c_str() ).find( needle ) != std::string::npos;
}

//! Split a serialized Document into lines (keeping empties) so a test can
//! assert that an occurrence edit touched EXACTLY one line and left every
//! other byte alone.  A whole-Document string compare proves "something
//! changed"; this proves "this line and only this line".
static std::vector<std::string> SplitLines( const std::string& s )
{
	std::vector<std::string> out;
	std::string cur;
	for( std::size_t i = 0; i < s.size(); ++i ) {
		if( s[i] == '\n' ) { out.push_back( cur ); cur.clear(); }
		else cur += s[i];
	}
	out.push_back( cur );
	return out;
}

//! Indices of the lines that differ between two serialized Documents.
//! Returns {-1} when the line COUNTS differ -- a caller asserting "one line
//! changed" must fail on an insert/remove too, and a positional diff would
//! otherwise report every line after the splice.
static std::vector<int> ChangedLineIndices( const std::string& before, const std::string& after )
{
	const std::vector<std::string> a = SplitLines( before );
	const std::vector<std::string> b = SplitLines( after );
	std::vector<int> out;
	if( a.size() != b.size() ) { out.push_back( -1 ); return out; }
	for( std::size_t i = 0; i < a.size(); ++i )
		if( a[i] != b[i] ) out.push_back( (int)i );
	return out;
}

//! The one line at `idx` of a serialized Document (empty for out of range).
static std::string LineAt( const std::string& doc, int idx )
{
	const std::vector<std::string> ls = SplitLines( doc );
	return ( idx >= 0 && (std::size_t)idx < ls.size() ) ? ls[idx] : std::string();
}

static bool PelsDiffer( const RISEPel& a, const RISEPel& b, double eps = 1e-6 )
{
	return std::abs( (double)a.r - (double)b.r ) > eps
	    || std::abs( (double)a.g - (double)b.g ) > eps
	    || std::abs( (double)a.b - (double)b.b ) > eps;
}

//////////////////////////////////////////////////////////////////////
// Test 1: the rows themselves.
//////////////////////////////////////////////////////////////////////
static void TestInspectRows()
{
	std::cout << "Test 1: PainterIntrospection rows across four painter kinds..." << std::endl;
	const char* tmp = "painterintro_rows.RISEscene";
	Job* j = LoadScene( kScene, tmp );
	Check( j != nullptr, "fixture scene loads via the CST path" );
	if( !j ) return;

	CameraProperty row;

	// ---- uniformcolor_painter: the multi-token colour literal ----
	{
		const std::vector<CameraProperty> rows = InspectPainter( *j, "basecol" );
		Check( !rows.empty(), "uniformcolor_painter yields rows" );
		Check( RowFor( rows, "type", row ) && row.value == String( "uniformcolor_painter" )
		    && !row.editable,
		       "leading `type` row names the chunk keyword, read-only" );
		Check( RowFor( rows, "pipe", row ) && row.value == String( "colour" ) && !row.editable,
		       "`pipe` row reports the COLOUR pipe for a uniformcolor_painter" );
		Check( RowFor( rows, "color", row ) && row.editable
		    && std::string( row.value.c_str() ).find( "0.25 0.5 0.75" ) != std::string::npos,
		       "`color` row is EDITABLE and carries the full multi-token value" );
		Check( RowFor( rows, "color", row ) && row.kind == ValueKind::DoubleVec3,
		       "`color` row carries the descriptor's DoubleVec3 kind" );
		Check( RowFor( rows, "colorspace", row ) && row.editable,
		       "an OMITTED param still surfaces, editable, at its descriptor default" );
	}

	// ---- perlin3d_painter: numeric params + reference slots ----
	{
		const std::vector<CameraProperty> rows = InspectPainter( *j, "noise" );
		Check( RowFor( rows, "octaves", row ) && row.editable && row.kind == ValueKind::UInt
		    && std::string( row.value.c_str() ).find( "4" ) != std::string::npos,
		       "perlin3d `octaves` row: editable, UInt, value 4" );
		Check( RowFor( rows, "persistence", row ) && row.editable && row.kind == ValueKind::Double
		    && std::string( row.value.c_str() ).find( "0.5" ) != std::string::npos,
		       "perlin3d `persistence` row: editable, Double, value 0.5" );
		Check( RowFor( rows, "scale", row ) && row.editable
		    && std::string( row.value.c_str() ).find( "2 2 2" ) != std::string::npos,
		       "perlin3d `scale` row carries the full 3-token value" );
		Check( RowFor( rows, "colora", row ) && row.kind == ValueKind::Reference
		    && !row.presets.empty(),
		       "a Reference row offers the LIVE painter candidates as presets" );
	}

	// ---- expression_painter: body, seed, time + ParamSpec merge ----
	{
		const std::vector<CameraProperty> rows = InspectPainter( *j, "ex" );
		Check( RowFor( rows, "expr", row ) && row.editable
		    && std::string( row.value.c_str() ).find( "mix(" ) != std::string::npos,
		       "expression `expr` BODY row is editable and carries the authored text" );
		Check( RowFor( rows, "seed", row ) && row.editable
		    && std::string( row.value.c_str() ).find( "3" ) != std::string::npos,
		       "expression `seed` row is editable" );
		Check( RowFor( rows, "time", row ) && row.editable,
		       "expression `time` row is editable" );

		// The repeatable rows -- invisible before this module.
		Check( RowFor( rows, "param[0]", row ), "repeatable `param` occurrence 0 surfaces as a row" );
		// doc 88 S4b flipped this: occurrence rows are EDITABLE now.
		Check( RowFor( rows, "param[0]", row ) && row.editable,
		       "an occurrence row is EDITABLE (S4b: occ-addressed editing)" );
		Check( RowFor( rows, "param[0]", row )
		    && std::string( row.value.c_str() ).find( "ring_scale 4.0" ) != std::string::npos,
		       "occurrence 0's value is the FULL `param` line, token join intact" );
		// The ParamSpec metadata merge -- the whole point of carrying it.
		Check( RowFor( rows, "param[0]", row ) && DescriptionContains( row, "Ring density" ),
		       "ParamSpec `label` is merged onto the param row" );
		// P3-a review fix: ExpressionParamSpec::ReadQuoted STRIPS the authored
		// quotes from `label "Ring density"` (out.label holds the inner text
		// only -- see ExpressionParamSpec.h), so ParamSpecSuffix's own
		// "  Label: \"" + spec.label + "\"." wrap supplies the ONLY pair.
		// Pin the count directly rather than trusting a substring find: a
		// future change that stops stripping on the scanner side, or that
		// adds a second wrap on the description side, shows up here as a
		// doubled or missing `""` instead of silently passing the substring
		// check above.
		{
			RowFor( rows, "param[0]", row );
			const std::string desc( row.description.c_str() );
			const std::string needle = "\"Ring density\"";
			const std::size_t first = desc.find( needle );
			Check( first != std::string::npos
			    && desc.find( needle, first + needle.size() ) == std::string::npos,
			       "the label is wrapped in EXACTLY one pair of quotes (verdict: the scanner correctly strips "
			       "the authored quotes; PainterIntrospection's own wrap is not a double-quote bug)" );
			Check( desc.find( "\"\"Ring" ) == std::string::npos
			    && desc.find( "density\"\"" ) == std::string::npos,
			       "no doubled quote marks flank the label text" );
		}
		Check( RowFor( rows, "param[0]", row ) && DescriptionContains( row, "0.5 .. 20" ),
		       "ParamSpec min/max are merged onto the param row" );
		Check( RowFor( rows, "param[0]", row ) && DescriptionContains( row, "Step: 0.25" ),
		       "ParamSpec `step` is merged onto the param row" );
		// ...and the SECOND param, which declares none, must not inherit the first's.
		Check( RowFor( rows, "param[1]", row )
		    && std::string( row.value.c_str() ).find( "wob" ) != std::string::npos,
		       "repeatable `param` occurrence 1 surfaces with its own value" );
		Check( RowFor( rows, "param[1]", row ) && !DescriptionContains( row, "Ring density" )
		    && !DescriptionContains( row, "Range:" ),
		       "a metadata-free param does NOT inherit its sibling's ParamSpec (name-matched, not index-drifted)" );
		Check( RowFor( rows, "def[0]", row ) && row.editable
		    && std::string( row.value.c_str() ).find( "rings" ) != std::string::npos,
		       "repeatable `def` occurrence surfaces editable" );
		Check( !RowFor( rows, "param[2]", row ),
		       "no phantom occurrence row past the authored count" );
	}

	// ---- ramp_painter: enums + repeated stops ----
	{
		const std::vector<CameraProperty> rows = InspectPainter( *j, "rmp" );
		Check( RowFor( rows, "interpolation", row ) && row.editable
		    && row.kind == ValueKind::Enum && row.presets.size() == 3,
		       "ramp `interpolation` is an editable Enum row with its 3 values as presets" );
		Check( RowFor( rows, "channel", row ) && row.editable && row.presets.size() == 4,
		       "ramp `channel` is an editable Enum row with 4 presets" );
		Check( RowFor( rows, "stop[0]", row ) && row.editable
		    && std::string( row.value.c_str() ).find( "0.0 0.1 0.2 0.3" ) != std::string::npos,
		       "ramp `stop[0]` surfaces editable with its full 4-token value" );
		Check( RowFor( rows, "stop[1]", row )
		    && std::string( row.value.c_str() ).find( "1.0 0.9 0.8 0.7" ) != std::string::npos,
		       "ramp `stop[1]` is the SECOND stop, not a repeat of the first" );
		Check( RowFor( rows, "stop[0]", row ) && DescriptionContains( row, "Occurrence 0" )
		    && DescriptionContains( row, "REPEATABLE" ),
		       "the occurrence row names WHICH occurrence it is and that the param is repeatable" );
		Check( RowFor( rows, "stop[0]", row ) && !DescriptionContains( row, "READ-ONLY" ),
		       "the S4 read-only disclaimer is GONE from the occurrence row (it would now be a lie)" );
	}

	// ---- the scalar pipe + the unknown name ----
	{
		Check( PainterIntrospection::PipesFor( *j, String( "basecol" ) )
		           == PainterIntrospection::PipeColour,
		       "PipesFor reports colour-only for uniformcolor_painter" );
		Check( PainterIntrospection::PipesFor( *j, String( "nope" ) )
		           == PainterIntrospection::PipeNone,
		       "PipesFor reports None for an unregistered name" );
		Check( InspectPainter( *j, "nope" ).empty(),
		       "Inspect returns no rows for a name that is not a painter chunk" );
		Check( InspectPainter( *j, "lum" ).empty(),
		       "Inspect refuses a name that resolves to a NON-painter chunk kind" );
	}

	// ---- the occurrence-row-name predicate ----
	{
		Check( PainterIntrospection::IsOccurrenceRowName( String( "stop[0]" ) ),
		       "IsOccurrenceRowName recognises a synthetic occurrence row" );
		Check( !PainterIntrospection::IsOccurrenceRowName( String( "octaves" ) ),
		       "IsOccurrenceRowName leaves a real param role alone" );
	}

	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 2: a scalar param edit persists, re-derives, and marks the
// FIRST-CLASS Painter dirty channel.
//////////////////////////////////////////////////////////////////////
static void TestScalarParamEditRoundTrip()
{
	std::cout << "Test 2: scalar param edit -> Document + live scene + dirty..." << std::endl;
	const char* tmp = "painterintro_edit.RISEscene";
	Job* j = LoadScene( kScene, tmp );
	Check( j != nullptr, "scene loads" );
	if( !j ) return;
	{
		TestController c( *j );
		c.SetSelection( SceneEditController::Category::Painter, String( "noise" ) );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

		Check( !c.HasUnsavedChanges(), "clean before the edit" );
		const std::string before = DocText( *j );
		Check( before.find( "octaves 4" ) != std::string::npos, "Document says `octaves 4` before" );

		const bool ok = c.SetPropertyForCategory(
			SceneEditController::Category::Painter, String( "octaves" ), String( "7" ) );
		Check( ok, "the panel edit route applies a painter param edit" );

		const std::string after = DocText( *j );
		Check( after.find( "octaves 7" ) != std::string::npos,
		       "the retained Document now says `octaves 7` (persists through save)" );
		Check( after.find( "octaves 4" ) == std::string::npos,
		       "the old value is GONE from the Document (a set, not an append)" );

		// The row re-reads the Document, so the panel agrees with the file.
		CameraProperty row;
		Check( RowFor( InspectPainter( *j, "noise" ), "octaves", row )
		    && std::string( row.value.c_str() ).find( "7" ) != std::string::npos,
		       "the property row re-reads the edited value" );

		// Dirty landed in the first-class per-entity Painter channel -- NOT
		// the coarse uncategorized CST-head boolean it used to fall into.
		Check( c.HasUnsavedChanges(), "the edit marks the editor dirty" );
		{
			const std::vector<DirtyEntity> ents = c.Editor().Dirty().EntitySnapshot();
			Check( ents.size() == 1
			    && ents[0].first == EntityCategory::Painter
			    && ents[0].second == "noise",
			       "the dirty mark is (Painter, \"noise\") -- category + name pinned" );
		}
		Check( c.Editor().Dirty().Count() == 0,
		       "a painter edit does NOT touch the object-transform channel" );
	}
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 3: editing the EXPRESSION BODY recompiles the VM program.
//////////////////////////////////////////////////////////////////////
static void TestExpressionBodyEdit()
{
	std::cout << "Test 3: expression body edit -> re-derive + recompiled program..." << std::endl;
	const char* tmp = "painterintro_expr.RISEscene";
	Job* j = LoadScene( kScene, tmp );
	Check( j != nullptr, "scene loads" );
	if( !j ) return;
	{
		TestController c( *j );
		c.SetSelection( SceneEditController::Category::Painter, String( "ex" ) );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

		RISEPel before, after;
		Check( PainterColorAt( *j, "ex", 0.4, before ),
		       "the expression painter evaluates before the edit" );

		// Replace the body with a constant that CANNOT coincide with the
		// original at the probe point (the original is a clamped sine mix).
		const bool ok = c.SetPropertyForCategory(
			SceneEditController::Category::Painter,
			String( "expr" ), String( "vec3(0.125, 0.375, 0.625)" ) );
		Check( ok, "the expression body edit applies" );

		Check( DocText( *j ).find( "vec3(0.125, 0.375, 0.625)" ) != std::string::npos,
		       "the new body is in the retained Document" );
		Check( PainterColorAt( *j, "ex", 0.4, after ),
		       "the expression painter still evaluates after the re-derive" );
		Check( std::abs( (double)after.r - 0.125 ) < 1e-6
		    && std::abs( (double)after.g - 0.375 ) < 1e-6
		    && std::abs( (double)after.b - 0.625 ) < 1e-6,
		       "the LIVE painter evaluates the NEW program (the VM was recompiled, not just the text swapped)" );
		Check( std::abs( (double)after.r - (double)before.r ) > 1e-6
		    || std::abs( (double)after.g - (double)before.g ) > 1e-6,
		       "the evaluated colour actually changed" );
	}
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 4: an INVALID expression body is refused cleanly.
//////////////////////////////////////////////////////////////////////
static void TestInvalidExpressionRefused()
{
	std::cout << "Test 4: invalid expression body -> clean refusal..." << std::endl;
	const char* tmp = "painterintro_badexpr.RISEscene";
	Job* j = LoadScene( kScene, tmp );
	Check( j != nullptr, "scene loads" );
	if( !j ) return;
	{
		TestController c( *j );
		c.SetSelection( SceneEditController::Category::Painter, String( "ex" ) );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

		const std::string before = DocText( *j );
		RISEPel colBefore;
		Check( PainterColorAt( *j, "ex", 0.4, colBefore ), "painter evaluates before" );
		Check( !c.HasUnsavedChanges(), "clean before the refused edit" );

		const bool ok = c.SetPropertyForCategory(
			SceneEditController::Category::Painter,
			String( "expr" ), String( "mix(vec3(0,0,0), , notafunction(" ) );
		Check( !ok, "a syntactically invalid body is REFUSED" );
		Check( DocText( *j ) == before,
		       "the Document is BYTE-IDENTICAL after a refused edit (the dry-run gate held)" );
		Check( !c.HasUnsavedChanges(),
		       "a refused edit does NOT mark dirty (nothing changed to save)" );
		RISEPel colAfter;
		Check( PainterColorAt( *j, "ex", 0.4, colAfter )
		    && std::abs( (double)colAfter.r - (double)colBefore.r ) < 1e-12,
		       "the LIVE painter is untouched by the refused edit" );
	}
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 5: Undo / Redo via the inverse patch.
//////////////////////////////////////////////////////////////////////
static void TestUndoRedo()
{
	std::cout << "Test 5: undo/redo of a painter param edit..." << std::endl;
	const char* tmp = "painterintro_undo.RISEscene";
	Job* j = LoadScene( kScene, tmp );
	Check( j != nullptr, "scene loads" );
	if( !j ) return;
	{
		TestController c( *j );
		c.SetSelection( SceneEditController::Category::Painter, String( "noise" ) );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

		Check( c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "persistence" ), String( "0.875" ) ), "edit applies" );
		Check( DocText( *j ).find( "persistence 0.875" ) != std::string::npos,
		       "Document carries the new persistence" );

		c.Undo();
		Check( DocText( *j ).find( "persistence 0.5" ) != std::string::npos,
		       "Undo restored the PRIOR value in the Document" );
		Check( DocText( *j ).find( "persistence 0.875" ) == std::string::npos,
		       "the edited value is gone after Undo" );
		{
			CameraProperty row;
			Check( RowFor( InspectPainter( *j, "noise" ), "persistence", row )
			    && std::string( row.value.c_str() ).find( "0.5" ) != std::string::npos,
			       "the property row reflects the undone value (live scene re-derived)" );
		}
		Check( c.HasUnsavedChanges(),
		       "still dirty after Undo (the Document moved away from the loaded bytes and back -- Save NoOps on byte-equality)" );

		c.Redo();
		Check( DocText( *j ).find( "persistence 0.875" ) != std::string::npos,
		       "Redo re-applied the edit to the Document" );
		{
			const std::vector<DirtyEntity> ents = c.Editor().Dirty().EntitySnapshot();
			bool found = false;
			for( std::size_t i = 0; i < ents.size(); ++i ) {
				if( ents[i].first == EntityCategory::Painter && ents[i].second == "noise" ) found = true;
			}
			Check( found, "Redo re-marks the (Painter, \"noise\") channel" );
		}
	}
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 6: refused inside an open editor transaction.
//
// This is ALSO what makes the per-entity Painter dirty channel safe
// without the CST-head boolean's OR-merge shield: a painter edit can
// never land inside a transaction, so a rollback's plain-copy restore
// of mEntityDirty can never wipe a mark whose Document mutation
// survived.
//////////////////////////////////////////////////////////////////////
static void TestTransactionRefusal()
{
	std::cout << "Test 6: painter edit refused mid-transaction..." << std::endl;
	const char* tmp = "painterintro_txn.RISEscene";
	Job* j = LoadScene( kScene, tmp );
	Check( j != nullptr, "scene loads" );
	if( !j ) return;
	{
		TestController c( *j );
		c.SetSelection( SceneEditController::Category::Painter, String( "noise" ) );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

		const std::string before = DocText( *j );
		c.BeginTransaction();
		const bool ok = c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "octaves" ), String( "9" ) );
		Check( !ok, "a painter edit is REFUSED while an editor transaction is open" );
		Check( DocText( *j ) == before, "the Document is untouched by the refused edit" );
		Check( !c.HasUnsavedChanges(), "no dirty mark from the refused mid-transaction edit" );
		c.RollbackTransaction();

		// ...and it works again once the transaction closes.
		Check( c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "octaves" ), String( "9" ) ),
		       "the same edit applies once the transaction has closed" );
	}
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 7: a MULTI-TOKEN value captures + restores the FULL join.
//
// The historical P1 this guards: the prior-value capture kept only the
// FIRST pvalue token, so an Undo of `color 1 1 1` restored `1`.  Here
// the check is component-wise on all three channels, both in the
// Document text and in the LIVE painter, so a truncation cannot hide.
//////////////////////////////////////////////////////////////////////
static void TestMultiTokenCapture()
{
	std::cout << "Test 7: multi-token colour capture/restore..." << std::endl;
	const char* tmp = "painterintro_multitok.RISEscene";
	Job* j = LoadScene( kScene, tmp );
	Check( j != nullptr, "scene loads" );
	if( !j ) return;
	{
		TestController c( *j );
		c.SetSelection( SceneEditController::Category::Painter, String( "basecol" ) );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

		RISEPel orig;
		Check( PainterColorAt( *j, "basecol", 0.0, orig ), "basecol evaluates before" );
		Check( std::abs( (double)orig.r - 0.25 ) < 1e-9
		    && std::abs( (double)orig.g - 0.50 ) < 1e-9
		    && std::abs( (double)orig.b - 0.75 ) < 1e-9,
		       "basecol starts at the authored 0.25 0.5 0.75" );

		Check( c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "color" ), String( "0.125 0.625 0.875" ) ), "multi-token edit applies" );
		Check( DocText( *j ).find( "color 0.125 0.625 0.875" ) != std::string::npos,
		       "the Document carries all three new components" );

		c.Undo();
		// The Document must carry all THREE original components back.
		Check( DocText( *j ).find( "color 0.25 0.5 0.75" ) != std::string::npos,
		       "Undo restored the FULL three-token colour to the Document (not a truncated first token)" );

		// ...and so must the LIVE painter, component by component.  A
		// first-token-only capture restores `0.25`, which derives as
		// r=0.25 g=0 b=0 -- caught here and only here.
		RISEPel back;
		Check( PainterColorAt( *j, "basecol", 0.0, back ), "basecol evaluates after Undo" );
		Check( std::abs( (double)back.r - 0.25 ) < 1e-9, "Undo restored R = 0.25" );
		Check( std::abs( (double)back.g - 0.50 ) < 1e-9, "Undo restored G = 0.5 (NOT 0 -- the truncation guard)" );
		Check( std::abs( (double)back.b - 0.75 ) < 1e-9, "Undo restored B = 0.75 (NOT 0 -- the truncation guard)" );
	}
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 8: Save -> Undo -> Redo dirty bookkeeping.
//
// The historical data-loss this guards: after a Save clears the tracker,
// an Undo or Redo mutates the Document AWAY from the saved bytes with
// nothing to flip HasUnsavedChanges() back on -- so a
// close-without-prompt silently discards it.  For painters that mark
// used to be reachable only through the coarse boolean; it now has to
// come back through the per-entity Painter channel in BOTH directions.
//////////////////////////////////////////////////////////////////////
static void TestSaveThenUndoRedoDirty()
{
	std::cout << "Test 8: save -> undo -> redo dirty bookkeeping..." << std::endl;
	const char* tmp = "painterintro_savedirty.RISEscene";
	Job* j = LoadScene( kScene, tmp );
	Check( j != nullptr, "scene loads" );
	if( !j ) return;
	{
		TestController c( *j );
		c.SetSelection( SceneEditController::Category::Painter, String( "noise" ) );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

		Check( c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "octaves" ), String( "6" ) ), "edit applies" );
		Check( c.HasUnsavedChanges(), "dirty after the edit" );

		const char* saveAs = "painterintro_saveas.RISEscene";
		std::remove( saveAs );
		const SaveResult sr = c.RequestSave( std::string( saveAs ) );
		Check( Succeeded( sr.status ), "Save-As succeeds" );
		Check( !c.HasUnsavedChanges(), "a successful save CLEARS the painter dirty mark" );

		// UNDO after the save: the Document moves away from the saved bytes.
		c.Undo();
		Check( DocText( *j ).find( "octaves 4" ) != std::string::npos, "Undo reverted the Document" );
		Check( c.HasUnsavedChanges(),
		       "UNDO after a save re-marks dirty (else a close-without-prompt loses the revert)" );

		// Save again so the tracker is clean, then REDO.
		std::remove( saveAs );
		const SaveResult sr2 = c.RequestSave( std::string( saveAs ) );
		Check( Succeeded( sr2.status ), "second Save-As succeeds" );
		Check( !c.HasUnsavedChanges(), "clean again after the second save" );

		c.Redo();
		Check( DocText( *j ).find( "octaves 6" ) != std::string::npos, "Redo re-applied the Document edit" );
		Check( c.HasUnsavedChanges(),
		       "REDO after a save re-marks dirty (the historical P1: this used to stay false)" );
		{
			const std::vector<DirtyEntity> ents = c.Editor().Dirty().EntitySnapshot();
			bool found = false;
			for( std::size_t i = 0; i < ents.size(); ++i ) {
				if( ents[i].first == EntityCategory::Painter && ents[i].second == "noise" ) found = true;
			}
			Check( found,
			       "the Redo-after-Save mark landed in the (Painter, \"noise\") channel, not the coarse boolean" );
		}
		std::remove( saveAs );
	}
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 9 (doc 88 S4b): the edit route accepts a WELL-FORMED, IN-RANGE
// occurrence row name and refuses everything else that wears brackets.
//
// S4 refused the whole bracketed vocabulary; S4b gives it a meaning, so
// what has to be pinned now is the BOUNDARY -- an out-of-range index, a
// malformed index, and a bracketed name on a NON-repeatable param must
// all still leave the Document byte-identical rather than inserting a
// line the descriptor never declared.
//////////////////////////////////////////////////////////////////////
static void TestOccurrenceRowRefused()
{
	std::cout << "Test 9: malformed / out-of-range occurrence names refused..." << std::endl;
	const char* tmp = "painterintro_occ.RISEscene";
	Job* j = LoadScene( kScene, tmp );
	Check( j != nullptr, "scene loads" );
	if( !j ) return;
	{
		TestController c( *j );
		c.SetSelection( SceneEditController::Category::Painter, String( "rmp" ) );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

		const std::string before = DocText( *j );

		// OUT OF RANGE: `rmp` has exactly two stops.
		Check( !c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "stop[99]" ), String( "1.0 0.0 0.0 0.0" ) ),
		       "an out-of-range occurrence index is refused" );
		Check( !c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "stop[2]" ), String( "1.0 0.0 0.0 0.0" ) ),
		       "the first index PAST the authored count is refused (off-by-one boundary)" );

		// MALFORMED: bracket shape, no usable index.
		Check( !c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "stop[]" ), String( "1.0 0.0 0.0 0.0" ) ), "an empty index is refused" );
		Check( !c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "stop[x]" ), String( "1.0 0.0 0.0 0.0" ) ), "a non-numeric index is refused" );
		Check( !c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "stop[-1]" ), String( "1.0 0.0 0.0 0.0" ) ), "a negative index is refused" );
		Check( !c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "stop[0]junk" ), String( "1.0 0.0 0.0 0.0" ) ),
		       "trailing text after the bracket is refused (no loose parse to 0)" );
		Check( !c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "stop[0" ), String( "1.0 0.0 0.0 0.0" ) ),
		       "an unterminated bracket is refused" );

		// BRACKET ON A NON-REPEATABLE PARAM: `channel` occurs once, so it has
		// no occurrences to address; the index must not be honoured as 0.
		Check( !c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "channel[0]" ), String( "G" ) ),
		       "a bracketed name on a NON-repeatable param is refused" );

		// BRACKET ON A NAME THAT IS NOT A PARAM AT ALL.
		Check( !c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "nosuch[0]" ), String( "1" ) ),
		       "a bracketed name that is not a parameter of this chunk kind is refused" );

		Check( DocText( *j ) == before,
		       "every refused bracketed edit left the Document byte-identical (no phantom line inserted)" );
		Check( !c.HasUnsavedChanges(), "no dirty mark from any refused occurrence edit" );

		// ...and the CONTROL: a well-formed, in-range occurrence DOES apply on
		// the same chunk, so the refusals above are keyed on the boundary and
		// not on a blanket lockout.
		Check( c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "stop[1]" ), String( "1.0 0.0 0.25 0.5" ) ),
		       "control: a well-formed IN-RANGE occurrence edit applies" );
		Check( DocText( *j ).find( "stop 1.0 0.0 0.25 0.5" ) != std::string::npos,
		       "control: the Document carries the occurrence edit" );
	}
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 10: the BRIDGE path.
//
// Every test above drives PainterIntrospection / SetPropertyForCategory
// directly.  Both GUI shells instead go through the per-category
// property SNAPSHOT (RefreshProperties -> PropertyCountFor /
// PropertyNameFor / PropertyValueFor / PropertyEditableFor, which the
// RISE_API_SceneEditController_Property*For C shims forward opaquely to
// RISEViewportBridge.mm's `propertySnapshotFor:` and ViewportBridge.cpp's
// `propertySnapshotFor` -- both category-generic, both already carrying
// Painter = 10).  This test proves the painter rows actually ARRIVE
// there, so "the bridges need no change" is a measured claim rather
// than a reading of the code.
//////////////////////////////////////////////////////////////////////
static void TestBridgeSnapshotPath()
{
	std::cout << "Test 10: rows reach the per-category snapshot the shells read..." << std::endl;
	const char* tmp = "painterintro_bridge.RISEscene";
	Job* j = LoadScene( kScene, tmp );
	Check( j != nullptr, "scene loads" );
	if( !j ) return;
	{
		TestController c( *j );
		c.SetSelection( SceneEditController::Category::Painter, String( "ex" ) );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );
		c.RefreshProperties();

		const unsigned int n = c.PropertyCountFor( SceneEditController::Category::Painter );
		Check( n > 0, "the Painter category snapshot is non-empty (the shells' read path)" );

		bool sawType = false, sawPipe = false, sawExpr = false, sawParamOcc = false;
		bool exprEditable = false, paramOccEditable = false;
		for( unsigned int i = 0; i < n; ++i ) {
			const String nm = c.PropertyNameFor( SceneEditController::Category::Painter, i );
			const bool ed   = c.PropertyEditableFor( SceneEditController::Category::Painter, i );
			if( nm == String( "type" ) )     sawType = true;
			if( nm == String( "pipe" ) )     sawPipe = true;
			if( nm == String( "expr" ) )   { sawExpr = true;     exprEditable = ed; }
			if( nm == String( "param[0]" ) ) { sawParamOcc = true; paramOccEditable = ed; }
		}
		Check( sawType && sawPipe, "identity + pipe rows reach the snapshot" );
		Check( sawExpr && exprEditable,
		       "the editable `expr` row reaches the snapshot MARKED EDITABLE (the shells render a field)" );
		Check( sawParamOcc && paramOccEditable,
		       "the `param[0]` occurrence row reaches the snapshot MARKED EDITABLE (S4b: the shells render a field/slider)" );

		// ...and a write through the same category surface round-trips.
		Check( c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "seed" ), String( "9.5" ) ), "a write through the category surface applies" );
		c.RefreshProperties();
		bool seedIsNine = false;
		const unsigned int n2 = c.PropertyCountFor( SceneEditController::Category::Painter );
		for( unsigned int i = 0; i < n2; ++i ) {
			if( c.PropertyNameFor( SceneEditController::Category::Painter, i ) != String( "seed" ) ) continue;
			const String v = c.PropertyValueFor( SceneEditController::Category::Painter, i );
			if( std::string( v.c_str() ).find( "9.5" ) != std::string::npos ) seedIsNine = true;
		}
		Check( seedIsNine, "the snapshot re-reads the edited value (full read/write round-trip through the shells' surface)" );
	}
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 11 (review round 1, P3-b #1): Undo of an expression BODY edit
// restores the exact original Document bytes, and the re-derived
// program evaluates IDENTICALLY to the pre-edit one -- not just "some
// value", the SAME value, proving the restore recompiled the original
// text rather than leaving a stale live program behind.
//////////////////////////////////////////////////////////////////////
static void TestExpressionBodyUndo()
{
	std::cout << "Test 11: undo of an expression BODY edit restores bytes + evaluation..." << std::endl;
	const char* tmp = "painterintro_exprundo.RISEscene";
	Job* j = LoadScene( kScene, tmp );
	Check( j != nullptr, "scene loads" );
	if( !j ) return;
	{
		TestController c( *j );
		c.SetSelection( SceneEditController::Category::Painter, String( "ex" ) );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

		const std::string before = DocText( *j );
		Check( before.find( "mix(vec3(0,0,0), vec3(1,1,1), rings)" ) != std::string::npos,
		       "Document carries the original body before the edit" );

		RISEPel origColor;
		Check( PainterColorAt( *j, "ex", 0.4, origColor ),
		       "the expression painter evaluates before the edit" );

		const bool ok = c.SetPropertyForCategory(
			SceneEditController::Category::Painter,
			String( "expr" ), String( "vec3(0.9, 0.2, 0.05)" ) );
		Check( ok, "the expression body edit applies" );

		RISEPel editedColor;
		Check( PainterColorAt( *j, "ex", 0.4, editedColor ),
		       "the expression painter evaluates after the edit" );
		Check( std::abs( (double)editedColor.r - 0.9 ) < 1e-6,
		       "the live painter evaluates the NEW body (sanity check before undoing)" );

		c.Undo();
		Check( DocText( *j ) == before,
		       "Undo restores the Document to BYTE-IDENTICAL bytes (the original body text, verbatim)" );

		RISEPel restoredColor;
		Check( PainterColorAt( *j, "ex", 0.4, restoredColor ),
		       "the expression painter evaluates after Undo" );
		Check( std::abs( (double)restoredColor.r - (double)origColor.r ) < 1e-9
		    && std::abs( (double)restoredColor.g - (double)origColor.g ) < 1e-9
		    && std::abs( (double)restoredColor.b - (double)origColor.b ) < 1e-9,
		       "Undo's re-derived program evaluates IDENTICALLY to the pre-edit program (recompiled from the "
		       "restored text, not a stale live carry-over)" );
	}
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 12 (review round 1, P3-b #2): scalar_painter{expression} panel
// parity with the colour-pipe expression painter -- the `pipe` row
// reports `scalar`, the body row is editable, and an edit applies /
// dirties / undoes exactly like Test 2 / Test 5 do for the colour pipe.
//////////////////////////////////////////////////////////////////////
static void TestScalarPainterPanelParity()
{
	std::cout << "Test 12: scalar_painter{expression} panel parity..." << std::endl;
	const char* tmp = "painterintro_scalarpanel.RISEscene";
	Job* j = LoadScene( kScene, tmp );
	Check( j != nullptr, "scene loads" );
	if( !j ) return;
	{
		TestController c( *j );
		c.SetSelection( SceneEditController::Category::Painter, String( "sp_rough" ) );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

		CameraProperty row;
		const std::vector<CameraProperty> rows = InspectPainter( *j, "sp_rough" );
		Check( RowFor( rows, "pipe", row ) && row.value == String( "scalar" ) && !row.editable,
		       "scalar_painter's `pipe` row reports SCALAR, not colour" );
		Check( RowFor( rows, "expression", row ) && row.editable,
		       "the `expression` body row is editable" );

		Check( !c.HasUnsavedChanges(), "clean before the edit" );
		const std::string before = DocText( *j );

		const bool ok = c.SetPropertyForCategory(
			SceneEditController::Category::Painter,
			String( "expression" ), String( "0.77" ) );
		Check( ok, "editing the scalar expression body applies" );
		Check( DocText( *j ).find( "expression 0.77" ) != std::string::npos,
		       "the Document carries the new scalar expression body" );

		Check( c.HasUnsavedChanges(), "the edit marks the editor dirty" );
		{
			const std::vector<DirtyEntity> ents = c.Editor().Dirty().EntitySnapshot();
			bool found = false;
			for( std::size_t i = 0; i < ents.size(); ++i )
				if( ents[i].first == EntityCategory::Painter && ents[i].second == "sp_rough" ) found = true;
			Check( found, "the dirty mark lands in (Painter, \"sp_rough\")" );
		}

		c.Undo();
		Check( DocText( *j ) == before,
		       "Undo restores the Document to the original scalar expression bytes" );
	}
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 13 (review round 1, P2-a): a BARE repeatable-role name (no
// "[i]" occurrence suffix) is refused too -- not just the bracketed
// occurrence rows Test 9 covers.  Without this, `SetPropertyForCategory
// (Painter, "stop", ...)` fell through to the generic CST edit route
// and silently wrote occurrence 0, contradicting the read-only-
// repeatables contract just as much as editing "stop[1]" would.
//////////////////////////////////////////////////////////////////////
static void TestBareRepeatableRoleRefused()
{
	std::cout << "Test 13: bare repeatable-role names refused (not just bracketed occurrence rows)..." << std::endl;
	{
		const char* tmp = "painterintro_barerep_ramp.RISEscene";
		Job* j = LoadScene( kScene, tmp );
		Check( j != nullptr, "ramp scene loads" );
		if( j ) {
			TestController c( *j );
			c.SetSelection( SceneEditController::Category::Painter, String( "rmp" ) );
			c.Start();
			Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

			const std::string before = DocText( *j );
			Check( !c.SetPropertyForCategory( SceneEditController::Category::Painter,
				String( "stop" ), String( "1.0 0.0 0.0 0.0" ) ),
			       "a BARE repeatable role name (`stop`, no bracket) is refused, not silently applied to "
			       "occurrence 0" );
			Check( DocText( *j ) == before,
			       "the refused bare-name edit left the Document byte-identical" );
			Check( !c.HasUnsavedChanges(), "no dirty mark from the refused bare-name edit" );

			j->release();
		}
		std::remove( tmp );
	}
	{
		const char* tmp = "painterintro_barerep_expr.RISEscene";
		Job* j = LoadScene( kScene, tmp );
		Check( j != nullptr, "expression scene loads" );
		if( j ) {
			TestController c( *j );
			c.SetSelection( SceneEditController::Category::Painter, String( "ex" ) );
			c.Start();
			Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

			const std::string before = DocText( *j );
			Check( !c.SetPropertyForCategory( SceneEditController::Category::Painter,
				String( "param" ), String( "ring_scale 99" ) ),
			       "a BARE repeatable role name (`param`, no bracket) is refused on expression_painter too" );
			Check( DocText( *j ) == before,
			       "the refused bare `param` edit left the Document byte-identical" );
			Check( !c.HasUnsavedChanges(), "no dirty mark from the refused bare `param` edit" );

			// Control: a NON-repeatable param on the SAME chunk kind still applies --
			// proves the refusal is keyed on `repeatable`, not a blanket lockout of
			// the whole chunk.
			Check( c.SetPropertyForCategory( SceneEditController::Category::Painter,
				String( "seed" ), String( "42" ) ),
			       "control: a non-repeatable param (`seed`) on the SAME chunk still applies" );
			Check( DocText( *j ).find( "seed 42" ) != std::string::npos,
			       "control: the Document carries the applied `seed` edit" );

			j->release();
		}
		std::remove( tmp );
	}
}


//////////////////////////////////////////////////////////////////////
// Test 14 (doc 88 S4b): a `stop[1]` edit rewrites THAT line and nothing
// else -- byte-exactly -- and the change reaches the live scene.
//
// The whole-Document compare in the other tests proves "something
// changed".  This one proves the surgical claim the slice actually
// makes: exactly ONE line of the serialized Document differs, it is the
// SECOND `stop` of a three-stop ramp, and the neighbours on both sides
// are byte-identical.  A capture/write pair that disagreed on the
// occurrence would land on stop 1 or stop 3 and fail here.
//////////////////////////////////////////////////////////////////////
static void TestOccurrenceEditIsSurgical()
{
	std::cout << "Test 14: `stop[1]` edit rewrites exactly that line..." << std::endl;
	const char* tmp = "painterintro_occ_surgical.RISEscene";
	Job* j = LoadScene( kScene, tmp );
	Check( j != nullptr, "scene loads" );
	if( !j ) return;
	{
		TestController c( *j );
		c.SetSelection( SceneEditController::Category::Painter, String( "rmp3" ) );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );
		Check( !c.HasUnsavedChanges(), "clean before the edit" );

		const std::string before = DocText( *j );
		RISEPel wasColor;
		const bool hadColor = PainterColorAt( *j, "rmp3", 0.37, wasColor );
		Check( hadColor, "rmp3 evaluates before the edit" );

		Check( c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "stop[1]" ), String( "0.5 1.0 0.0 0.0" ) ),
		       "the middle stop's occurrence row applies" );

		const std::string after = DocText( *j );
		const std::vector<int> changed = ChangedLineIndices( before, after );
		Check( changed.size() == 1,
		       "EXACTLY ONE line of the whole Document changed (not zero, not two, no insert)" );
		if( changed.size() == 1 ) {
			Check( LineAt( before, changed[0] ) == "stop 0.5 0.44 0.54 0.64",
			       "the changed line was the SECOND stop of the three (occurrence 1)" );
			Check( LineAt( after, changed[0] ) == "stop 0.5 1.0 0.0 0.0",
			       "the changed line now carries the full four-token new value" );
		}
		// The neighbours, stated positively rather than inferred from the count.
		Check( after.find( "stop 0.0 0.11 0.21 0.31" ) != std::string::npos,
		       "occurrence 0 is byte-identical after the edit" );
		Check( after.find( "stop 1.0 0.91 0.81 0.71" ) != std::string::npos,
		       "occurrence 2 is byte-identical after the edit" );
		// ...and the two-stop `rmp` in the same document, which shares the role
		// name, must not have been touched either.
		Check( after.find( "name rmp\ninput" ) != std::string::npos
		    || after.find( "name rmp\n" ) != std::string::npos,
		       "the OTHER ramp painter is still present" );

		// The derive really ran: the live painter evaluates differently.
		RISEPel nowColor;
		Check( PainterColorAt( *j, "rmp3", 0.37, nowColor ), "rmp3 evaluates after the edit" );
		Check( PelsDiffer( wasColor, nowColor ),
		       "the LIVE painter changed -- the occurrence edit re-derived, it did not only touch text" );

		Check( c.HasUnsavedChanges(), "the occurrence edit marks the editor dirty" );
		{
			const std::vector<DirtyEntity> ents = c.Editor().Dirty().EntitySnapshot();
			Check( ents.size() == 1
			    && ents[0].first == EntityCategory::Painter
			    && ents[0].second == "rmp3",
			       "the dirty mark is (Painter, \"rmp3\") -- the occurrence row reuses the painter channel" );
		}
	}
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 15 (doc 88 S4b): an expression `param[1]` value edit changes the
// evaluation, leaves `param[0]` alone, and does not disturb param[0]'s
// ParamSpec metadata (the slider bounds must survive a sibling's edit).
//////////////////////////////////////////////////////////////////////
static void TestExpressionParamOccurrenceEdit()
{
	std::cout << "Test 15: expression `param[1]` occurrence edit..." << std::endl;
	const char* tmp = "painterintro_occ_param.RISEscene";
	// A body that READS both params, so an edit to either is observable.
	static const std::string sceneText =
		std::string( kScene ).substr( 0, std::string( kScene ).find( "expression_painter" ) )
		+ "expression_painter\n{\nname ex2\n"
		  "param ring_scale 4.0 min 0.5 max 20 step 0.25 label \"Ring density\"\n"
		  "param wob 0.25\n"
		  "def rings clamp(0.5+0.5*sin(P.x*ring_scale)*wob, 0, 1)\n"
		  "expr mix(vec3(0,0,0), vec3(1,1,1), rings)\n"
		  "seed 3.0\ntime 0.0\n}\n"
		+ std::string( kScene ).substr( std::string( kScene ).find( "expression_painter" ) );
	Job* j = LoadScene( sceneText.c_str(), tmp );
	Check( j != nullptr, "scene loads" );
	if( !j ) return;
	{
		TestController c( *j );
		c.SetSelection( SceneEditController::Category::Painter, String( "ex2" ) );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

		RISEPel was;
		Check( PainterColorAt( *j, "ex2", 0.41, was ), "ex2 evaluates before" );
		const std::string before = DocText( *j );

		Check( c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "param[1]" ), String( "wob 0.9" ) ),
		       "`param[1]` (the metadata-free param) applies" );

		const std::string after = DocText( *j );
		const std::vector<int> changed = ChangedLineIndices( before, after );
		Check( changed.size() == 1 && LineAt( after, changed[0] ) == "param wob 0.9",
		       "exactly the second `param` line changed" );
		Check( after.find( "param ring_scale 4.0 min 0.5 max 20 step 0.25 label \"Ring density\"" ) != std::string::npos,
		       "`param[0]` -- name, value AND its min/max/step/label metadata -- is byte-identical" );

		RISEPel now;
		Check( PainterColorAt( *j, "ex2", 0.41, now ), "ex2 evaluates after" );
		Check( PelsDiffer( was, now ),
		       "the recompiled program evaluates differently (the param edit reached the VM)" );

		// The ParamSpec metadata still reaches the row -- as fields AND prose.
		CameraProperty row;
		Check( RowFor( InspectPainter( *j, "ex2" ), "param[0]", row ) && row.hasRange,
		       "`param[0]` still carries its authored range after a SIBLING param was edited" );
		Check( row.hasRange && std::abs( (double)row.rangeMin - 0.5 ) < 1e-12
		    && std::abs( (double)row.rangeMax - 20.0 ) < 1e-12
		    && std::abs( (double)row.rangeStep - 0.25 ) < 1e-12,
		       "`param[0]`'s min/max/step survived the sibling edit unchanged" );
		Check( RowFor( InspectPainter( *j, "ex2" ), "param[1]", row )
		    && std::string( row.value.c_str() ).find( "wob 0.9" ) != std::string::npos,
		       "the `param[1]` row re-reads the edited value" );

		// And a `param[0]` VALUE edit that keeps the metadata keeps the range.
		Check( c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "param[0]" ),
			String( "ring_scale 9.5 min 0.5 max 20 step 0.25 label \"Ring density\"" ) ),
		       "`param[0]` applies with its metadata carried through the write" );
		Check( DocText( *j ).find( "param ring_scale 9.5 min 0.5 max 20 step 0.25 label \"Ring density\"" ) != std::string::npos,
		       "the re-tokenised write preserved the quoted label and every metadata token" );
		Check( RowFor( InspectPainter( *j, "ex2" ), "param[0]", row ) && row.hasRange
		    && std::abs( (double)row.rangeMax - 20.0 ) < 1e-12,
		       "the range survives a value edit that carries the metadata (the slider does not vanish mid-scrub)" );
	}
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 15b (S5): a HAND-AUTHORED `label` with an interior double space is
// exactly what a scene on disk can carry -- nothing upstream of the
// inspector enforces single-spacing.  The inspector must read it back
// SINGLE-spaced (ExpressionParamSpec::ReadQuoted's own normalization: see
// its doc), even though the raw document text still has two; and a WRITE
// to that same param line must ALSO end up single-spaced (Cst.cpp's
// WithParamValue independently collapses via SplitWs), so read and write
// agree with each other from the very first read, not just after the
// first edit happens to reflow it.
//////////////////////////////////////////////////////////////////////
static void TestLabelWhitespaceNormalized()
{
	std::cout << "Test 15b: label whitespace is single-space normalized on read, and stays that way "
	             "across a write..." << std::endl;
	const char* tmp = "painterintro_label_ws.RISEscene";
	static const std::string sceneText =
		std::string( kScene ).substr( 0, std::string( kScene ).find( "expression_painter" ) )
		+ "expression_painter\n{\nname ex2\n"
		  "param ring_scale 4.0 min 0.5 max 20 step 0.25 label \"Vein  frequency\"\n"
		  "param wob 0.25\n"
		  "def rings clamp(0.5+0.5*sin(P.x*ring_scale)*wob, 0, 1)\n"
		  "expr mix(vec3(0,0,0), vec3(1,1,1), rings)\n"
		  "seed 3.0\ntime 0.0\n}\n"
		+ std::string( kScene ).substr( std::string( kScene ).find( "expression_painter" ) );
	Job* j = LoadScene( sceneText.c_str(), tmp );
	Check( j != nullptr, "scene loads" );
	if( !j ) return;
	{
		TestController c( *j );
		c.SetSelection( SceneEditController::Category::Painter, String( "ex2" ) );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

		// The raw DOCUMENT still carries the authored double space -- this
		// scene was hand-written, never round-tripped through a CST write.
		Check( DocText( *j ).find( "label \"Vein  frequency\"" ) != std::string::npos,
		       "sanity: the raw document text has the authored double space" );

		// But the INSPECTOR's ParamSpec-derived description reads it SINGLE-
		// spaced: ReadQuoted normalizes on the way OUT, independent of
		// whether the document has ever been written by this editor.
		CameraProperty row;
		Check( RowFor( InspectPainter( *j, "ex2" ), "param[0]", row )
		    && DescriptionContains( row, "Vein frequency" ),
		       "S5 MONEY: the inspector's description reports the label SINGLE-spaced even though "
		       "the document text itself still has two -- ReadQuoted's normalization applies "
		       "regardless of write history" );
		Check( !DescriptionContains( row, "Vein  frequency" ),
		       "S5: ...and specifically NOT the raw double-spaced form" );

		// A write to this SAME param line (same value, same metadata, same
		// AUTHORED double-space label) re-serializes through Cst.cpp's
		// WithParamValue, which collapses the whitespace via SplitWs
		// independently of ReadQuoted -- so the DOCUMENT itself now agrees
		// with what the inspector already reported before this write ran.
		Check( c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "param[0]" ),
			String( "ring_scale 9.5 min 0.5 max 20 step 0.25 label \"Vein  frequency\"" ) ),
		       "the write, typed with the SAME double-spaced label, applies" );
		Check( DocText( *j ).find( "label \"Vein frequency\"" ) != std::string::npos,
		       "S5 MONEY: the WRITE collapses the double space too -- the document is now "
		       "single-spaced, matching what the inspector already reported pre-write" );
		Check( DocText( *j ).find( "label \"Vein  frequency\"" ) == std::string::npos,
		       "S5: ...the double-spaced form is gone from the document entirely" );
		Check( RowFor( InspectPainter( *j, "ex2" ), "param[0]", row )
		    && DescriptionContains( row, "Vein frequency" ),
		       "...and a re-read after the write still reports the same single-spaced label -- read "
		       "and write were never going to disagree about this text" );
	}
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 16 (doc 88 S4b): a `def[0]` occurrence edit recompiles, and an
// INVALID def body is refused by the same full-derivability gate every
// other painter param edit passes through -- Document byte-identical.
//////////////////////////////////////////////////////////////////////
static void TestDefOccurrenceEditAndRefusal()
{
	std::cout << "Test 16: `def[0]` occurrence edit + invalid-body refusal..." << std::endl;
	const char* tmp = "painterintro_occ_def.RISEscene";
	Job* j = LoadScene( kScene, tmp );
	Check( j != nullptr, "scene loads" );
	if( !j ) return;
	{
		TestController c( *j );
		c.SetSelection( SceneEditController::Category::Painter, String( "ex" ) );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

		RISEPel was;
		Check( PainterColorAt( *j, "ex", 0.29, was ), "ex evaluates before" );

		Check( c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "def[0]" ), String( "rings clamp(0.5+0.5*sin(P.x*ring_scale*3.0), 0, 1)" ) ),
		       "a `def[0]` body edit applies" );
		Check( DocText( *j ).find( "ring_scale*3.0" ) != std::string::npos,
		       "the Document carries the new def body" );

		RISEPel now;
		Check( PainterColorAt( *j, "ex", 0.29, now ), "ex evaluates after" );
		Check( PelsDiffer( was, now ),
		       "the def edit RECOMPILED the program (a text-only edit would evaluate identically)" );

		// INVALID body: the derive gate must refuse, leaving the Document alone.
		const std::string before = DocText( *j );
		const bool wasDirty = c.HasUnsavedChanges();
		Check( !c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "def[0]" ), String( "rings this is not an expression ((((" ) ),
		       "an INVALID def body is refused" );
		Check( DocText( *j ) == before,
		       "the refused def edit left the Document byte-identical (the derive gate ran before the commit)" );
		Check( c.HasUnsavedChanges() == wasDirty,
		       "the refused def edit did not move the dirty state" );

		RISEPel stillNow;
		Check( PainterColorAt( *j, "ex", 0.29, stillNow ), "ex still evaluates after the refusal" );
		Check( !PelsDiffer( now, stillNow ),
		       "the LIVE program is untouched by the refused edit" );
	}
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 17 (doc 88 S4b): Undo/Redo of an occurrence edit restores the
// right LINE, whole.
//
// The capture half reads occurrence N and the write half writes
// occurrence N; if they ever disagree, an Undo silently restores a
// neighbour.  Pinned positionally (which line changed) rather than by
// substring, because "the value is somewhere in the document" is
// exactly what a wrong-line restore also satisfies.
//////////////////////////////////////////////////////////////////////
static void TestOccurrenceUndoRedo()
{
	std::cout << "Test 17: undo/redo at an occurrence restores the right line..." << std::endl;
	const char* tmp = "painterintro_occ_undo.RISEscene";
	Job* j = LoadScene( kScene, tmp );
	Check( j != nullptr, "scene loads" );
	if( !j ) return;
	{
		TestController c( *j );
		c.SetSelection( SceneEditController::Category::Painter, String( "rmp3" ) );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

		const std::string pristine = DocText( *j );

		Check( c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "stop[1]" ), String( "0.5 1.0 0.0 0.0" ) ), "occurrence edit applies" );
		const std::string edited = DocText( *j );

		c.Undo();
		const std::string undone = DocText( *j );
		Check( undone == pristine,
		       "Undo restored the Document BYTE-EXACTLY -- the whole four-token stop line, at its own "
		       "occurrence (a first-token-only capture would land `stop 0.5` here)" );

		c.Redo();
		Check( DocText( *j ) == edited, "Redo re-applied the occurrence edit byte-exactly" );
		{
			const std::vector<DirtyEntity> ents = c.Editor().Dirty().EntitySnapshot();
			bool found = false;
			for( std::size_t i = 0; i < ents.size(); ++i )
				if( ents[i].first == EntityCategory::Painter && ents[i].second == "rmp3" ) found = true;
			Check( found, "Redo re-marks the (Painter, \"rmp3\") channel" );
		}

		// A SECOND occurrence, edited and undone while the first stays edited --
		// proves the two history entries address their own lines independently.
		Check( c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "stop[2]" ), String( "1.0 0.2 0.3 0.4" ) ), "a second occurrence edit applies" );
		c.Undo();
		const std::string afterSecondUndo = DocText( *j );
		Check( afterSecondUndo == edited,
		       "undoing the SECOND occurrence edit left the FIRST one's line intact" );
	}
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 18 (doc 88 S4b): the DRIFT GUARD.
//
// The U2 lesson at param-line granularity: an occurrence index captured
// at edit time is not a promise about the document later.  Both halves
// below move the occurrence layout out from under a pending Undo through
// the SAME checked Job primitives an agent-originated edit uses -- and,
// exactly like an agent chunk-CRUD verb, they leave no EditHistory record
// of their own, so nothing invalidates the param edit's entry.  That is
// the real-world shape of this hazard, not a contrivance.
//
//   (a) IDENTITY -- occurrence 1 still EXISTS, but now holds a value no
//       history entry wrote.  Without the identity check the Undo
//       overwrites that line with the edit's prior value.
//   (b) EXISTENCE -- occurrence 1 is gone entirely.
//
// Both assert the Job returned code 2 (D2: a FULL re-derive, rebuilding the
// Scene's managers from the mutated Document).  A `stop`/`param` edit through
// ApplyCstParamEditChecked / ApplyCstParamRemoveChecked always goes through the
// full-derivability gate -- it is never the code-1 incremental fast path -- so
// asserting `rc == 2` pins that shape rather than merely tolerating it under
// `rc >= 1`.  The refusal path that follows (SceneEditor::Undo/Redo's drift
// guard) never dereferences the STALE cached manager pointers a D2 leaves
// behind on the editor side: the guard's own checks (existence, identity) all
// run against the CST Document alone -- the very thing the drift-inducing
// call just rebuilt from -- and a refusal short-circuits before any manager
// pointer is ever touched, so a test that leaves those pointers stale is safe
// by construction.  If a future change made either call incremental instead,
// this assertion is the one that would need to move, not a `>= 1` that would
// have silently absorbed the shift.
//
// RED-PROVE (a): disabling the identity comparison in
// SceneEditor::OccurrenceEditStillAddressable_ makes the "Document is
// byte-identical after the refused Undo" assertion fail, with the
// drifted line clobbered by `stop 0.5 0.4 0.5 0.6`.
//////////////////////////////////////////////////////////////////////
static void TestOccurrenceDriftGuard()
{
	std::cout << "Test 18: occurrence drift guard refuses a shifted Undo..." << std::endl;

	// ---- (a) IDENTITY drift: the index survives, the line underneath changed.
	{
		const char* tmp = "painterintro_occ_drift.RISEscene";
		Job* j = LoadScene( kScene, tmp );
		Check( j != nullptr, "scene loads" );
		if( j ) {
			TestController c( *j );
			c.SetSelection( SceneEditController::Category::Painter, String( "rmp3" ) );
			c.Start();
			Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

			Check( c.SetPropertyForCategory( SceneEditController::Category::Painter,
				String( "stop[1]" ), String( "0.5 1.0 0.0 0.0" ) ), "occurrence edit applies" );

			// Someone else rewrites that very line, with no history record.
			const int rc = j->ApplyCstParamEditChecked( "rmp3", "painter", "stop", 1, "0.5 0.3 0.3 0.3" );
			Check( rc == 2, "the drift-inducing rewrite landed (D2: full re-derive)" );

			const std::string drifted = DocText( *j );
			Check( drifted.find( "stop 0.5 0.3 0.3 0.3" ) != std::string::npos,
			       "occurrence 1 now holds a value NO history entry wrote" );

			c.Undo();
			Check( DocText( *j ) == drifted,
			       "the Undo was REFUSED -- Document byte-identical, the drifted line not clobbered" );
			Check( DocText( *j ).find( "stop 0.5 0.44 0.54 0.64" ) == std::string::npos,
			       "the edit's PRIOR value was not written over the drifted line" );
			Check( c.Editor().History().UndoDepth() >= 1,
			       "the history entry STAYS after an honest refusal (the user can retry, not lose the edit)" );
			j->release();
		}
		std::remove( tmp );
	}

	// ---- (a2) The same drift shape produced by an INSERT-BEFORE, which is
	// what actually shifts every later occurrence down: remove occurrence 0.
	// Occurrence 1 then names what used to be occurrence 2 -- it exists, it is
	// simply somebody else's line.
	{
		const char* tmp = "painterintro_occ_drift_shift.RISEscene";
		Job* j = LoadScene( kScene, tmp );
		Check( j != nullptr, "scene loads (shift drift)" );
		if( j ) {
			TestController c( *j );
			c.SetSelection( SceneEditController::Category::Painter, String( "rmp3" ) );
			c.Start();
			Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

			Check( c.SetPropertyForCategory( SceneEditController::Category::Painter,
				String( "stop[1]" ), String( "0.5 1.0 0.0 0.0" ) ), "occurrence edit applies" );

			const int rc = j->ApplyCstParamRemoveChecked( "rmp3", "painter", "stop", 0 );
			Check( rc == 2, "the leading-stop removal landed (D2: full re-derive)" );

			const std::string drifted = DocText( *j );
			Check( drifted.find( "stop 0.0 0.11 0.21 0.31" ) == std::string::npos,
			       "the leading stop is gone -- every later occurrence shifted down by one" );
			Check( drifted.find( "stop 1.0 0.91 0.81 0.71" ) != std::string::npos,
			       "the final stop -- now occurrence 1 -- is present and unedited" );

			c.Undo();
			Check( DocText( *j ) == drifted,
			       "the shifted Undo was REFUSED -- no neighbouring stop clobbered" );
			Check( DocText( *j ).find( "stop 1.0 0.91 0.81 0.71" ) != std::string::npos,
			       "the line occurrence 1 now names survived intact" );
			Check( DocText( *j ).find( "stop 0.5 0.44 0.54 0.64" ) == std::string::npos,
			       "the edit's prior value was not written anywhere" );
			j->release();
		}
		std::remove( tmp );
	}

	// ---- (b) EXISTENCE drift: occurrence 1 is gone entirely.  Uses the
	// expression painter's `param` rather than a ramp `stop`, because
	// ramp_painter REFUSES to derive with fewer than two stops -- so a ramp can
	// never actually lose its occurrence 1.  `param wob` is unreferenced by the
	// fixture's def/expr, so removing it derives cleanly.
	{
		const char* tmp = "painterintro_occ_drift2.RISEscene";
		Job* j = LoadScene( kScene, tmp );
		Check( j != nullptr, "scene loads (existence half)" );
		if( j ) {
			TestController c( *j );
			c.SetSelection( SceneEditController::Category::Painter, String( "ex" ) );
			c.Start();
			Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

			Check( c.SetPropertyForCategory( SceneEditController::Category::Painter,
				String( "param[1]" ), String( "wob 0.9" ) ), "occurrence edit applies" );
			Check( j->ApplyCstParamRemoveChecked( "ex", "painter", "param", 1 ) == 2,
			       "the EDITED param occurrence was removed out of band (D2: full re-derive)" );

			const std::string drifted = DocText( *j );
			Check( drifted.find( "param wob" ) == std::string::npos,
			       "occurrence 1 of `param` no longer exists" );

			c.Undo();
			Check( DocText( *j ) == drifted,
			       "an Undo whose occurrence no longer EXISTS is refused, Document byte-identical" );
			Check( DocText( *j ).find( "param wob 0.25" ) == std::string::npos,
			       "the prior value was not re-inserted as a phantom line" );
			Check( c.Editor().History().UndoDepth() >= 1,
			       "the history entry stays on the existence refusal too" );
			j->release();
		}
		std::remove( tmp );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 19 (doc 88 S4b): the row RANGE metadata both bridges now carry.
//
// The Mac panel decides "slider or not" from `hasRange` alone, so the
// three questions that matter are: does an authored min/max/step reach
// the row as FIELDS, does a param with no metadata correctly report NO
// range (rather than a 0..0 track), and do the fields survive the trip
// through the per-category snapshot the shells actually read.
//////////////////////////////////////////////////////////////////////
static void TestRowRangeMetadata()
{
	std::cout << "Test 19: param rows carry hasRange/min/max/step..." << std::endl;
	const char* tmp = "painterintro_range.RISEscene";
	Job* j = LoadScene( kScene, tmp );
	Check( j != nullptr, "scene loads" );
	if( !j ) return;
	{
		CameraProperty row;
		const std::vector<CameraProperty> rows = InspectPainter( *j, "ex" );

		Check( RowFor( rows, "param[0]", row ) && row.hasRange,
		       "the metadata-carrying `param[0]` row reports hasRange" );
		Check( row.hasRange && std::abs( (double)row.rangeMin  -  0.5  ) < 1e-12,
		       "rangeMin matches the authored `min 0.5`" );
		Check( row.hasRange && std::abs( (double)row.rangeMax  - 20.0  ) < 1e-12,
		       "rangeMax matches the authored `max 20`" );
		Check( row.hasRange && std::abs( (double)row.rangeStep -  0.25 ) < 1e-12,
		       "rangeStep matches the authored `step 0.25`" );

		Check( RowFor( rows, "param[1]", row ) && !row.hasRange,
		       "a param with NO authored metadata reports hasRange = false (no 0..0 slider track)" );
		Check( !row.hasRange && (double)row.rangeMin == 0.0 && (double)row.rangeMax == 0.0
		    && (double)row.rangeStep == 0.0,
		       "...and its range numbers are a defined 0, not indeterminate" );

		// Rows that are not expression params carry no range at all.
		Check( RowFor( rows, "seed", row ) && !row.hasRange,
		       "an ordinary single-occurrence row carries no range" );
		Check( RowFor( InspectPainter( *j, "rmp3" ), "stop[0]", row ) && !row.hasRange,
		       "a ramp `stop` occurrence row carries no range (only expression params do)" );

		// ...and the same facts through the per-category snapshot the two GUI
		// bridges read (RISEViewportBridge.mm / ViewportBridge.cpp both call the
		// *For accessors; this is what "the panel can draw a slider" rests on).
		TestController c( *j );
		c.SetSelection( SceneEditController::Category::Painter, String( "ex" ) );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );
		c.RefreshProperties();

		const SceneEditController::Category cat = SceneEditController::Category::Painter;
		const unsigned int n = c.PropertyCountFor( cat );
		bool sawRanged = false, sawUnranged = false, occEditable = false;
		for( unsigned int i = 0; i < n; ++i ) {
			const String nm = c.PropertyNameFor( cat, i );
			if( nm == String( "param[0]" ) ) {
				occEditable = c.PropertyEditableFor( cat, i );
				sawRanged = c.PropertyHasRangeFor( cat, i )
				         && std::abs( c.PropertyRangeMinFor(  cat, i ) -  0.5  ) < 1e-12
				         && std::abs( c.PropertyRangeMaxFor(  cat, i ) - 20.0  ) < 1e-12
				         && std::abs( c.PropertyRangeStepFor( cat, i ) -  0.25 ) < 1e-12;
			}
			if( nm == String( "param[1]" ) ) sawUnranged = !c.PropertyHasRangeFor( cat, i );
		}
		Check( sawRanged,
		       "the range reaches the per-category snapshot with all three numbers intact (the shells' read path)" );
		Check( sawUnranged, "the metadata-free param reports no range through the snapshot too" );
		Check( occEditable,
		       "the occurrence row reaches the snapshot EDITABLE, so the panel offers the slider AND the field" );
		Check( !c.PropertyHasRangeFor( cat, n + 100 ),
		       "an out-of-range property index answers 'no range' rather than reading past the snapshot" );
	}
	j->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 20 (round-1 P1 fix): the drift guard's WHITESPACE NORMALISATION.
//
// Cst::WithParamValue -- the sole writer any occurrence edit and its
// Undo/Redo route through -- re-tokenises the incoming value on ANY
// whitespace run and re-emits the tokens joined by a SINGLE space,
// discarding whatever separators the caller sent.  So a column-aligned
// authored line (`stop 0.5   0.44  0.54  0.64`) or a commit that resends
// the rest-of-line bytes verbatim (a slider's `commit()` pattern: replace
// one token's range in the captured line, leave the rest byte-for-byte)
// carries INTERIOR whitespace no write path preserves.  Comparing the
// live Document's read (post-write: single-spaced) against a captured
// value (pre-write: whatever separators its origin used) byte-for-byte
// -- what an ends-only trim did -- refuses a perfectly legitimate,
// unchanged Undo/Redo as "drifted".  OccurrenceEditStillAddressable_ must
// whitespace-NORMALISE (token-split + single-space rejoin) both sides
// before comparing -- see NormalizeParamValueWs_'s doc in SceneEditor.cpp.
//
//   (a) COLUMN-ALIGNED fixture: edit occurrence 1, Undo, then REDO.
//       Redo's drift check compares the CURRENT (post-Undo, single-spaced
//       -- Undo's own revert-write goes through WithParamValue too) line
//       against `edit.prevPropertyValue`, captured VERBATIM from the
//       column-aligned original -- this is where an ends-only trim
//       refused a legitimate Redo.
//   (b) A commit that changes ONE token but resends every other byte of
//       the line verbatim (the slider `commit()` shape): the value SENT
//       for the edit itself carries the original's interior spacing on
//       the untouched tokens, so even the FIRST Undo (no Redo needed)
//       hits the mismatch.
//   (c) CONTROL: a genuine token-level drift (an out-of-band rewrite that
//       changes an actual VALUE, not just spacing) must still be
//       refused -- normalisation must narrow to whitespace, not swallow
//       real drift.
//////////////////////////////////////////////////////////////////////
static void TestOccurrenceDriftGuardWhitespace()
{
	std::cout << "Test 20: occurrence drift guard normalises whitespace, not tokens..." << std::endl;

	// A column-aligned ramp: interior spacing pads every stop's tokens to
	// line up visually, exactly the authoring style a human (or a
	// well-formatted external tool) produces.  Self-contained (not kScene)
	// so its extra interior spaces cannot ripple into other tests' exact
	// substring checks.
	static const char* kSceneAligned =
		"RISE ASCII SCENE 7\n"
		"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
		"uniformcolor_painter\n{\nname basecol\ncolor 0.25 0.5 0.75\n}\n"
		"perlin3d_painter\n{\nname noise\ncolora white\ncolorb basecol\noctaves 4\npersistence 0.5\nscale 2 2 2\n}\n"
		"ramp_painter\n{\nname rmpA\ninput noise\nchannel R\ninterpolation linear\n"
			"stop 0.0   0.11  0.21  0.31\n"
			"stop 0.5   0.44  0.54  0.64\n"
			"stop 1.0   0.91  0.81  0.71\n}\n"
		"scalar_painter\n{\nname sp_rough\nexpression 0.3\n}\n"
		"lambertian_luminaire_material\n{\nname lum\nexitance basecol\nscale 5.0\nmaterial none\n}\n"
		"sphere_geometry\n{\nname s\nradius 1\n}\n"
		"standard_object\n{\nname obj\ngeometry s\nmaterial lum\n}\n";

	// ---- (a) column-aligned original: edit, Undo, then REDO.
	{
		const char* tmp = "painterintro_occ_drift_ws_a.RISEscene";
		Job* j = LoadScene( kSceneAligned, tmp );
		Check( j != nullptr, "scene loads (aligned fixture)" );
		if( j ) {
			TestController c( *j );
			c.SetSelection( SceneEditController::Category::Painter, String( "rmpA" ) );
			c.Start();
			Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

			const std::string original = DocText( *j );
			Check( original.find( "stop 0.5   0.44  0.54  0.64" ) != std::string::npos,
			       "the fixture's column-aligned stop[1] line is present, spacing intact" );

			Check( c.SetPropertyForCategory( SceneEditController::Category::Painter,
				String( "stop[1]" ), String( "0.5 1.0 0.0 0.0" ) ), "occurrence edit applies" );
			const std::string edited = DocText( *j );
			Check( edited.find( "stop 0.5 1.0 0.0 0.0" ) != std::string::npos,
			       "the edit landed, single-spaced (as every write is)" );

			// Undo's own revert-WRITE goes through the same WithParamValue
			// single-space re-tokenisation as any other write, so it does NOT
			// restore the original's column alignment -- only its TOKENS.  The
			// byte-exact expectation is therefore `original` with just the
			// stop[1] line's spacing collapsed, not `original` verbatim.
			std::string expectedAfterUndo = original;
			{
				const std::string alignedLine  = "stop 0.5   0.44  0.54  0.64";
				const std::string collapsedLine = "stop 0.5 0.44 0.54 0.64";
				const std::size_t pos = expectedAfterUndo.find( alignedLine );
				Check( pos != std::string::npos, "the aligned stop[1] line is findable in `original`" );
				if( pos != std::string::npos )
					expectedAfterUndo.replace( pos, alignedLine.size(), collapsedLine );
			}

			c.Undo();
			Check( DocText( *j ) == expectedAfterUndo,
			       "Undo was NOT refused -- restores the prior TOKENS exactly (single-spaced, as any "
			       "write re-emits them), everything else byte-identical to the original "
			       "(pre-fix: an ends-only trim compared the single-spaced post-Undo line against "
			       "the captured multi-spaced original and refused this Undo as drifted)" );

			c.Redo();
			Check( DocText( *j ) == edited,
			       "Redo re-applies -- NOT refused by the aligned original's interior spacing "
			       "(pre-fix: an ends-only trim compared the single-spaced post-Undo line against "
			       "the captured multi-spaced original and refused this Redo as drifted)" );
			j->release();
		}
		std::remove( tmp );
	}

	// ---- (b) a commit that changes one token, resending the rest of the
	// line's bytes -- including interior alignment spacing -- verbatim.
	{
		const char* tmp = "painterintro_occ_drift_ws_b.RISEscene";
		Job* j = LoadScene( kSceneAligned, tmp );
		Check( j != nullptr, "scene loads (aligned fixture, single-token-change half)" );
		if( j ) {
			TestController c( *j );
			c.SetSelection( SceneEditController::Category::Painter, String( "rmpA" ) );
			c.Start();
			Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

			// Only the second token changes (0.44 -> 0.20); the rest of the value,
			// spacing included, is exactly what was authored -- the slider
			// `commit()` shape (`line.replaceSubrange` on the captured line).
			Check( c.SetPropertyForCategory( SceneEditController::Category::Painter,
				String( "stop[1]" ), String( "0.5   0.20  0.54  0.64" ) ),
			       "single-token-change occurrence edit applies" );

			const std::string edited = DocText( *j );
			Check( edited.find( "stop 0.5 0.20 0.54 0.64" ) != std::string::npos,
			       "the write itself is single-spaced regardless of what was sent" );

			// Undo's own revert-write re-tokenises + single-spaces the restored
			// value too (see (a)'s note), so the line reads back single-spaced
			// even though the ORIGINAL was column-aligned -- the TOKENS (the
			// prior value) are what must match, not the original's bytes.
			c.Undo();
			Check( DocText( *j ).find( "stop 0.5 0.44 0.54 0.64" ) != std::string::npos,
			       "Undo was NOT refused -- the prior value's tokens are back "
			       "(pre-fix: comparing the verbatim-spaced sent value against the "
			       "single-spaced Document line refused this Undo outright)" );
			j->release();
		}
		std::remove( tmp );
	}

	// ---- (c) CONTROL: a real token-level drift is still refused.
	{
		const char* tmp = "painterintro_occ_drift_ws_c.RISEscene";
		Job* j = LoadScene( kSceneAligned, tmp );
		Check( j != nullptr, "scene loads (aligned fixture, control half)" );
		if( j ) {
			TestController c( *j );
			c.SetSelection( SceneEditController::Category::Painter, String( "rmpA" ) );
			c.Start();
			Check( c.ForTest_WaitForRenders( 1, 3000 ), "initial render fires" );

			Check( c.SetPropertyForCategory( SceneEditController::Category::Painter,
				String( "stop[1]" ), String( "0.5 1.0 0.0 0.0" ) ), "occurrence edit applies" );

			// Someone else rewrites that line's VALUE out of band -- a real
			// drift, not just spacing.
			const int rc = j->ApplyCstParamEditChecked( "rmpA", "painter", "stop", 1, "0.5 0.3 0.3 0.3" );
			Check( rc == 2, "the drift-inducing rewrite landed (D2: full re-derive)" );
			const std::string drifted = DocText( *j );

			c.Undo();
			Check( DocText( *j ) == drifted,
			       "the Undo is STILL refused -- a genuine token-level value change is drift "
			       "regardless of spacing, and normalisation must not swallow it" );
			j->release();
		}
		std::remove( tmp );
	}
}

int main()
{
	std::cout << "=== PainterIntrospectionRoundTripTest (doc 88 S4 + S4b) ===" << std::endl;

	TestInspectRows();
	TestScalarParamEditRoundTrip();
	TestExpressionBodyEdit();
	TestInvalidExpressionRefused();
	TestUndoRedo();
	TestTransactionRefusal();
	TestMultiTokenCapture();
	TestSaveThenUndoRedoDirty();
	TestOccurrenceRowRefused();
	TestBridgeSnapshotPath();
	TestExpressionBodyUndo();
	TestScalarPainterPanelParity();
	TestBareRepeatableRoleRefused();
	// doc 88 S4b
	TestOccurrenceEditIsSurgical();
	TestExpressionParamOccurrenceEdit();
	TestLabelWhitespaceNormalized();
	TestDefOccurrenceEditAndRefusal();
	TestOccurrenceUndoRedo();
	TestOccurrenceDriftGuard();
	TestRowRangeMetadata();
	TestOccurrenceDriftGuardWhitespace();

	std::cout << "\n" << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
