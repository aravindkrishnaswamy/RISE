//////////////////////////////////////////////////////////////////////
//
//  PainterIntrospectionRoundTripTest.cpp - doc 88 S4: prove a painter's
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
		Check( RowFor( rows, "param[0]", row ) && !row.editable,
		       "an occurrence row is READ-ONLY (occ-addressed editing is deferred)" );
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
		Check( RowFor( rows, "def[0]", row ) && !row.editable
		    && std::string( row.value.c_str() ).find( "rings" ) != std::string::npos,
		       "repeatable `def` occurrence surfaces read-only" );
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
		Check( RowFor( rows, "stop[0]", row ) && !row.editable
		    && std::string( row.value.c_str() ).find( "0.0 0.1 0.2 0.3" ) != std::string::npos,
		       "ramp `stop[0]` surfaces read-only with its full 4-token value" );
		Check( RowFor( rows, "stop[1]", row )
		    && std::string( row.value.c_str() ).find( "1.0 0.9 0.8 0.7" ) != std::string::npos,
		       "ramp `stop[1]` is the SECOND stop, not a repeat of the first" );
		Check( RowFor( rows, "stop[0]", row ) && DescriptionContains( row, "REPEATABLE" ),
		       "the occurrence row says WHY it is read-only" );
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
// Test 9: the edit route refuses a synthetic occurrence row name.
//////////////////////////////////////////////////////////////////////
static void TestOccurrenceRowRefused()
{
	std::cout << "Test 9: occurrence row names refused by the edit route..." << std::endl;
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
		Check( !c.SetPropertyForCategory( SceneEditController::Category::Painter,
			String( "stop[1]" ), String( "1.0 0.0 0.0 0.0" ) ),
		       "an occurrence row name is refused" );
		Check( DocText( *j ) == before,
		       "the refused occurrence edit left the Document byte-identical (no `stop[1]` line inserted)" );
		Check( !c.HasUnsavedChanges(), "no dirty mark from the refused occurrence edit" );
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
		bool exprEditable = false, paramOccEditable = true;
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
		Check( sawParamOcc && !paramOccEditable,
		       "the `param[0]` occurrence row reaches the snapshot MARKED READ-ONLY (the shells render it inert)" );

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

int main()
{
	std::cout << "=== PainterIntrospectionRoundTripTest (doc 88 S4) ===" << std::endl;

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

	std::cout << "\n" << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
