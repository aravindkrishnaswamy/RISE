//////////////////////////////////////////////////////////////////////
//
//  CstDescriptorBindTest.cpp - transfer-gate item 5: bind the CST derive
//  through the LIVE chunk-parser descriptor registry.
//
//  Item 2 derived only sphere_geometry, via a hand-written validator + a direct
//  Job::AddSphereGeometry call. Item 5 generalises: DeriveToJob now looks each
//  chunk up in the live registry (CreateAllChunkParsers), validates its params
//  through the SAME DispatchChunkParameters the legacy parser runs, and applies
//  via the SAME IAsciiChunkParser::Finalize. So ANY registry chunk type derives,
//  and the CST path and the legacy path build an identical Job for the canonical
//  scenes the CST is fed (see DeriveToJob's doc in Cst.h for the exact scope:
//  macro-free, directive-free, comments on their own lines, single-space values).
//
//  This suite proves:
//    * [equiv]      a multi-type scene (painter + material + object + geometry)
//                   derives through the CST to a Job whose canonical dump equals
//                   the legacy parse's -- reference wiring + multi-token
//                   transforms included (the object's world bbox encodes the
//                   multi-token position/scale, so a value mis-capture diverges).
//    * [multitoken] a multi-token param value (`color 0.2 0.4 0.6`, `position
//                   1 2 3`) is captured WHOLE, not truncated to the first token.
//    * [validate]   descriptor-driven validation refuses a malformed scene and
//                   applies NOTHING (refuse-all): unknown parameter, unknown
//                   chunk type, non-finite / non-numeric numeric value,
//                   value-less parameter line.
//
//////////////////////////////////////////////////////////////////////

#include "CstRenderEquivalence.h"
#include "../src/Library/Cst/Cst.h"

using namespace RISE;
using namespace RISE::Cst;
using namespace risequiv;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

// Derive a scene through the CST path (ParseToCst -> DeriveToJob) into `job`.
static int DeriveCst( const std::string& scene, Job& job, std::vector<std::string>* diags = nullptr )
{
	Document d = ParseToCst( scene );
	return DeriveToJob( d, job, diags );
}

// Join a param node's pvalue tokens (mirrors the kernel's internal joiner) to
// assert multi-token value capture directly at the CST level.
static std::string JoinParamValue( const NodeRef& p )
{
	std::string v; bool inVal = false;
	if( p ) for( const auto& k : p->kids ) {
		if( !inVal && k->kind == NodeKind::Token && k->role == "pvalue" ) inVal = true;
		if( inVal ) v += k->text;
	}
	return v;
}

int main()
{
	std::printf( "CstDescriptorBindTest -- transfer-gate item 5 (descriptor-registry binding)\n" );

	// painter (multi-token colour) + material (reference) + geometry + object
	// (multi-token position/scale, reference wiring). Authored in the canonical
	// keyword + name + single-space-value shape the v6->v7 serializer produces.
	const std::string scene =
		"RISE ASCII SCENE 6\n"
		"uniformcolor_painter\n{\nname red\ncolor 0.2 0.4 0.6\n}\n"
		"lambertian_material\n{\nname redmat\nreflectance red\n}\n"
		"sphere_geometry\n{\nname ball\nradius 0.25\n}\n"
		"standard_object\n{\nname obj\ngeometry ball\nmaterial redmat\nposition 1 2 3\nscale 2 2 2\n}\n";

	//----------------------------------------------------------------------
	// [equiv] the descriptor-bound derive is faithful: CST Job == legacy Job.
	//----------------------------------------------------------------------
	std::printf( "[equiv] multi-type scene: CST derive == legacy derive\n" );
	{
		Job* lj = new Job(); bool okl = ParseLegacy( scene, *lj );
		Job* cj = new Job(); std::vector<std::string> diags; int n = DeriveCst( scene, *cj, &diags );
		Check( okl, "legacy parse ok" );
		Check( n == 4 && diags.empty(), "CST derive applied all 4 chunks (painter+material+geometry+object), no diagnostics" );
		std::string dl = DumpJob( *lj ), dc = DumpJob( *cj );
		Check( dl == dc, "CST-derived Job dump == legacy Job dump (descriptor-bound derive is faithful)" );
		if( dl != dc ) std::printf( "    legacy=[%s]\n    cst   =[%s]\n", dl.c_str(), dc.c_str() );
		Check( dc.find( "geometry=ball" )   != std::string::npos, "object references geometry 'ball'" );
		Check( dc.find( "material=redmat" ) != std::string::npos, "object references material 'redmat'" );
		lj->release(); cj->release();
	}

	//----------------------------------------------------------------------
	// [equiv-ws] each param line is whitespace-normalised exactly like the legacy
	// parser (TokenizeString collapses " \t\r" runs, rejoin single-space). Tabs /
	// multi-space in a string-valued param -- ESPECIALLY a reference -- must NOT
	// drift the Job (the round-1 review's silent-object-drop on `geometry  ball`).
	//----------------------------------------------------------------------
	std::printf( "[equiv-ws] tab / multi-space param values normalise like legacy\n" );
	{
		const std::string ws =
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname red\ncolor 0.2 0.4 0.6\n}\n"
			"lambertian_material\n{\nname redmat\nreflectance red\n}\n"
			"sphere_geometry\n{\nname\tball\nradius 0.25\n}\n"
			"standard_object\n{\nname obj\ngeometry  ball\nmaterial\tredmat\nposition 1  2   3\n}\n";
		Job* lj = new Job(); bool okl = ParseLegacy( ws, *lj );
		Job* cj = new Job(); std::vector<std::string> diags; int n = DeriveCst( ws, *cj, &diags );
		Check( okl && n == 4 && diags.empty(), "tab/multi-space scene derives (both paths accept it)" );
		std::string dl = DumpJob( *lj ), dc = DumpJob( *cj );
		Check( dl == dc, "tab/multi-space normalised identically to legacy (object wired, not dropped)" );
		if( dl != dc ) std::printf( "    legacy=[%s]\n    cst   =[%s]\n", dl.c_str(), dc.c_str() );
		Check( dc.find( "geometry=ball" )   != std::string::npos, "double-space `geometry  ball` resolves to 'ball', not ' ball'" );
		Check( dc.find( "material=redmat" ) != std::string::npos, "tab `material\\tredmat` resolves to 'redmat'" );
		lj->release(); cj->release();
	}

	//----------------------------------------------------------------------
	// [apply-abort] an APPLY-time Finalize failure (a dangling reference, which
	// PASS-1 validation cannot detect) is NOT silently swallowed: the derive stops
	// at the first failing chunk with a diagnostic, leaving earlier chunks applied
	// -- matching the legacy parser's abort-on-first-failure, so the applied-prefix
	// Job state agrees even on this diverging-input case.
	//----------------------------------------------------------------------
	std::printf( "[apply-abort] dangling reference: stop + diagnose, never silently half-derive\n" );
	{
		const std::string dangling =
			"RISE ASCII SCENE 6\n"
			"sphere_geometry\n{\nname s\nradius 0.25\n}\n"
			"standard_object\n{\nname o\ngeometry nonexistent\nposition 0 0 0\n}\n"
			"sphere_geometry\n{\nname t\nradius 0.5\n}\n";
		Job* cj = new Job(); std::vector<std::string> diags; int n = DeriveCst( dangling, *cj, &diags );
		Check( n == 1 && !diags.empty(), "applies only the chunk before the failure (s), with a diagnostic" );
		std::string dc = DumpJob( *cj );
		Check( dc.find( "\n  s " ) != std::string::npos, "geometry 's' (before the failure) IS applied" );
		Check( dc.find( "\n  t " ) == std::string::npos, "geometry 't' (after the failure) is NOT applied (stopped, not continued)" );
		Check( dc.find( "\n  o " ) == std::string::npos, "the dangling object 'o' is not in the scene" );
		Job* lj = new Job(); ParseLegacy( dangling, *lj );
		Check( DumpJob( *lj ) == dc, "applied-prefix Job state matches legacy (both abort at the dangling ref)" );
		lj->release(); cj->release();
	}

	//----------------------------------------------------------------------
	// [multitoken] multi-token values captured whole (not truncated to token 1).
	//----------------------------------------------------------------------
	std::printf( "[multitoken] multi-token param value captured whole\n" );
	{
		Document d = ParseToCst( scene );
		Check( SerializeCst( d ) == scene, "multi-token scene round-trips byte-identical" );
		NodeId painter = DocFindByName( d, "uniformcolor_painter/red" );
		Check( painter != 0, "painter 'red' addressable by keyword/name" );
		NodeRef colorNode = DocResolveNodeId( d, DocParamId( d, painter, "color" ) );
		Check( JoinParamValue( colorNode ) == "0.2 0.4 0.6", "color value == '0.2 0.4 0.6' (all three tokens, not just the first)" );
		NodeId obj = DocFindByName( d, "standard_object/obj" );
		NodeRef posNode = DocResolveNodeId( d, DocParamId( d, obj, "position" ) );
		Check( JoinParamValue( posNode ) == "1 2 3", "position value == '1 2 3' (multi-token)" );
	}

	//----------------------------------------------------------------------
	// [validate] descriptor-driven validation refuses malformed scenes, and a
	// refusal applies NOTHING (refuse-all boundary) -- even valid sibling chunks.
	//----------------------------------------------------------------------
	std::printf( "[validate] descriptor-driven validation refuses malformed scenes (apply NOTHING)\n" );
	// A fresh Job is not empty: InitializeContainers() seeds "none" defaults in
	// some managers. Refuse-all means the counts stay at this baseline.
	int baseGeo, basePnt;
	{ Job* b = new Job(); baseGeo = b->GetGeometries()->getItemCount(); basePnt = b->GetPainters()->getItemCount(); b->release(); }
	auto RefusesApplyingNothing = [&]( const std::string& s, const char* what ) {
		Job* j = new Job(); std::vector<std::string> diags; int n = DeriveCst( s, *j, &diags );
		bool refused = ( n == 0 ) && !diags.empty()
			&& j->GetGeometries()->getItemCount() == baseGeo
			&& j->GetPainters()->getItemCount()  == basePnt;   // valid sibling NOT applied
		Check( refused, what );
		j->release();
	};
	RefusesApplyingNothing(
		"RISE ASCII SCENE 6\n"
		"sphere_geometry\n{\nname s\nradius 1\nbogus 5\n}\n"
		"uniformcolor_painter\n{\nname p\ncolor 1 1 1\n}\n",
		"unknown parameter -> refuse-all (the valid sibling painter is NOT applied)" );
	RefusesApplyingNothing(
		"RISE ASCII SCENE 6\n"
		"not_a_real_chunk\n{\nname x\n}\n"
		"sphere_geometry\n{\nname s\nradius 1\n}\n",
		"unknown chunk type -> refuse-all" );
	RefusesApplyingNothing(
		"RISE ASCII SCENE 6\n"
		"sphere_geometry\n{\nname s\nradius nan\n}\n",
		"non-finite numeric value (nan) -> refuse-all" );
	RefusesApplyingNothing(
		"RISE ASCII SCENE 6\n"
		"sphere_geometry\n{\nname s\nradius abc\n}\n",
		"non-numeric value for a numeric param -> refuse-all" );
	RefusesApplyingNothing(
		"RISE ASCII SCENE 6\n"
		"sphere_geometry\n{\nname s\nradius\n}\n",
		"value-less parameter line -> refuse-all" );

	//----------------------------------------------------------------------
	// [valid] a well-formed scene applies cleanly through the registry.
	//----------------------------------------------------------------------
	std::printf( "[valid] well-formed scene applies through the registry\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		int n = DeriveCst( "RISE ASCII SCENE 6\nsphere_geometry\n{\nname s\nradius 0.6\n}\n", *j, &diags );
		Check( n == 1 && diags.empty() && j->GetGeometries()->getItemCount() == 1,
		       "valid sphere derives (1 geometry, no diagnostics)" );
		j->release();
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
