// SceneEditorSuggestionsTest — exercises the scene grammar, cursor
// context resolver, and suggestion engine.  Asserts the core contract:
// all 126 chunk keywords are enumerable, context resolution picks the
// right scope in representative scene fragments, and the suggestion
// engine returns chunk keywords at scene root and rejects already-
// authored non-repeatable parameters inside a block.
//
// Standalone executable (matches tests/ convention).  No framework.

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

#include "../src/Library/SceneEditorSuggestions/SceneGrammar.h"
#include "../src/Library/SceneEditorSuggestions/CompletionContext.h"
#include "../src/Library/SceneEditorSuggestions/NameIndex.h"
#include "../src/Library/SceneEditorSuggestions/SuggestionEngine.h"

using namespace RISE;
using namespace RISE::SceneEditorSuggestions;

static int g_fails = 0;

#define EXPECT(cond) do { if(!(cond)) { std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " " #cond << std::endl; ++g_fails; } } while(0)

static std::size_t FindStr( const std::string& s, const char* needle )
{
	return s.find( needle );
}

void TestGrammarCoverage()
{
	std::cout << "GrammarCoverage\n";
	const SceneGrammar& g = SceneGrammar::Instance();
	const auto& kws = g.AllChunkKeywords();
	// 126 chunk types per the parser registry (including legacy alias
	// mis_pathtracing_shaderop which shares a class with pathtracing_shaderop).
	EXPECT( kws.size() == 126 );

	// Every chunk must have a non-empty descriptor (Phase 1c invariant:
	// Describe() is pure-virtual, so every registered parser has its own
	// implementation).  No fallback to an empty placeholder is allowed.
	std::size_t chunks_with_keyword    = 0;
	std::size_t chunks_with_parameters = 0;
	for( const ChunkDescriptor* d : g.AllChunks() ) {
		if( !d->keyword.empty() )     ++chunks_with_keyword;
		if( !d->parameters.empty() )  ++chunks_with_parameters;
	}
	// 125 unique descriptors (mis_pathtracing_shaderop alias shares a
	// parser with pathtracing_shaderop and therefore the same descriptor;
	// both appear in AllChunks() as two entries pointing at one descriptor).
	EXPECT( chunks_with_keyword == kws.size() );
	// Every parser's descriptor lists at least one parameter.
	EXPECT( chunks_with_parameters == kws.size() );

	// A handful of specific keywords that must be present.
	bool has_pt = false, has_bdpt = false, has_mat = false, has_light = false, has_alias = false;
	for( const std::string& k : kws ) {
		if( k == "pathtracing_pel_rasterizer" ) has_pt = true;
		if( k == "bdpt_pel_rasterizer" )         has_bdpt = true;
		if( k == "lambertian_material" )         has_mat = true;
		if( k == "ambient_light" )               has_light = true;
		if( k == "mis_pathtracing_shaderop" )    has_alias = true;
	}
	EXPECT( has_pt );
	EXPECT( has_bdpt );
	EXPECT( has_mat );
	EXPECT( has_light );
	EXPECT( has_alias );

	// Alias lookup resolves to the same descriptor as the primary keyword.
	const ChunkDescriptor* primary = g.FindChunk( "pathtracing_shaderop" );
	const ChunkDescriptor* aliased = g.FindChunk( "mis_pathtracing_shaderop" );
	EXPECT( primary != nullptr );
	EXPECT( aliased != nullptr );
	if( primary && aliased ) {
		EXPECT( primary == aliased );
	}

	// FindChunk round-trip for AmbientLight (fully populated descriptor).
	const ChunkDescriptor* d = g.FindChunk( "ambient_light" );
	EXPECT( d != nullptr );
	if( d ) {
		EXPECT( d->keyword == "ambient_light" );
		EXPECT( d->category == ChunkCategory::Light );
		bool has_name = false, has_power = false, has_color = false;
		for( const ParameterDescriptor& p : d->parameters ) {
			if( p.name == "name" )  has_name = true;
			if( p.name == "power" ) has_power = true;
			if( p.name == "color" ) has_color = true;
		}
		EXPECT( has_name );
		EXPECT( has_power );
		EXPECT( has_color );
	}

	// Every chunk category referenced in the grammar yields a non-empty
	// display name.  This catches a new ChunkCategory enum value being
	// added without updating the display-name switch.
	for( const ChunkDescriptor* cd : g.AllChunks() ) {
		const char* cat = g.CategoryDisplayName( cd->category );
		EXPECT( cat != nullptr );
		if( cat ) EXPECT( cat[0] != '\0' );
	}
}

void TestContextSceneRoot()
{
	std::cout << "ContextSceneRoot\n";
	const std::string buf = "RISE ASCII SCENE 5\n\npng_pain";
	CompletionContext c = ResolveCompletionContext( buf, buf.size() );
	EXPECT( c.scope == Scope::SceneRoot );
	EXPECT( c.partialToken == "png_pain" );
}

void TestContextInBlockParamName()
{
	std::cout << "ContextInBlockParamName\n";
	const std::string buf =
		"RISE ASCII SCENE 5\n"
		"ambient_light\n"
		"{\n"
		"\tpow";
	CompletionContext c = ResolveCompletionContext( buf, buf.size() );
	EXPECT( c.scope == Scope::InBlockParamName );
	EXPECT( c.chunkKeyword == "ambient_light" );
	EXPECT( c.partialToken == "pow" );
}

void TestContextInBlockParamValue()
{
	std::cout << "ContextInBlockParamValue\n";
	const std::string buf =
		"RISE ASCII SCENE 5\n"
		"ambient_light\n"
		"{\n"
		"\tpower 1";
	CompletionContext c = ResolveCompletionContext( buf, buf.size() );
	EXPECT( c.scope == Scope::InBlockParamValue );
	EXPECT( c.chunkKeyword == "ambient_light" );
	EXPECT( c.paramNameOnLine == "power" );
}

void TestContextInComment()
{
	std::cout << "ContextInComment\n";
	const std::string buf =
		"RISE ASCII SCENE 5\n"
		"# ambient_lig";
	CompletionContext c = ResolveCompletionContext( buf, buf.size() );
	EXPECT( c.scope == Scope::InComment );
}

void TestSuggestChunkKeywordsAtRoot()
{
	std::cout << "SuggestChunkKeywordsAtRoot\n";
	SuggestionEngine engine;
	const std::string buf = "RISE ASCII SCENE 5\n\n";
	auto sugs = engine.GetSuggestions( buf, buf.size(), SuggestionMode::ContextMenu );
	EXPECT( sugs.size() == 126 );
	bool found_ambient = false;
	for( const Suggestion& s : sugs ) {
		if( s.insertText == "ambient_light" ) { found_ambient = true; break; }
	}
	EXPECT( found_ambient );
}

void TestSuggestParametersInAmbientLight()
{
	std::cout << "SuggestParametersInAmbientLight\n";
	SuggestionEngine engine;
	const std::string buf =
		"RISE ASCII SCENE 5\n"
		"ambient_light\n"
		"{\n"
		"\t";
	auto sugs = engine.GetSuggestions( buf, buf.size(), SuggestionMode::ContextMenu );
	// AmbientLight has name/power/color.  Display text is the bare
	// parameter name; insert text adds a value placeholder for context-
	// menu mode (verified separately in TestParameterContextMenuIncludesValuePlaceholder).
	EXPECT( sugs.size() >= 3 );
	bool has_name = false, has_power = false, has_color = false;
	for( const Suggestion& s : sugs ) {
		if( s.displayText == "name" )  has_name = true;
		if( s.displayText == "power" ) has_power = true;
		if( s.displayText == "color" ) has_color = true;
	}
	EXPECT( has_name );
	EXPECT( has_power );
	EXPECT( has_color );
}

void TestNonRepeatableParameterFilteredAfterUse()
{
	std::cout << "NonRepeatableParameterFilteredAfterUse\n";
	SuggestionEngine engine;
	// `name` is already present — should be suppressed from the suggestions
	// at the next line's start (not currently being typed).
	const std::string buf =
		"RISE ASCII SCENE 5\n"
		"ambient_light\n"
		"{\n"
		"\tname my_light\n"
		"\t";
	auto sugs = engine.GetSuggestions( buf, buf.size(), SuggestionMode::ContextMenu );
	bool has_name = false;
	for( const Suggestion& s : sugs ) {
		if( s.displayText == "name" ) { has_name = true; break; }
	}
	EXPECT( !has_name );
}

void TestNameIndexSkipsCommentedOutNames()
{
	std::cout << "NameIndexSkipsCommentedOutNames\n";
	const std::string buf =
		"RISE ASCII SCENE 5\n"
		"lambertian_material\n"
		"{\n"
		"\tname real_mat\n"
		"}\n"
		"/*\n"
		"lambertian_material\n"
		"{\n"
		"\tname commented_mat\n"
		"}\n"
		"*/\n"
		"lambertian_material\n"
		"{\n"
		"\t# name also_commented\n"
		"\tname hash_commented_should_lose\n"
		"}\n";
	NameIndex idx( buf );
	bool has_real = false, has_commented = false;
	for( const DefinedName& dn : idx.AllNames() ) {
		if( dn.name == "real_mat" )                        has_real = true;
		if( dn.name == "commented_mat" )                   has_commented = true;
		if( dn.name == "also_commented" )                  has_commented = true;
	}
	EXPECT( has_real );
	EXPECT( !has_commented );
}

void TestAliasShowsAsItsOwnSuggestion()
{
	std::cout << "AliasShowsAsItsOwnSuggestion\n";
	SuggestionEngine engine;
	const std::string buf = "RISE ASCII SCENE 5\n\n";
	auto sugs = engine.GetSuggestions( buf, buf.size(), SuggestionMode::ContextMenu );
	std::size_t pathtracing = 0, alias = 0;
	for( const Suggestion& s : sugs ) {
		if( s.insertText == "pathtracing_shaderop" )    ++pathtracing;
		if( s.insertText == "mis_pathtracing_shaderop" ) ++alias;
	}
	// The alias must appear as a separately-insertable choice, AND
	// the primary must not be duplicated.
	EXPECT( pathtracing == 1 );
	EXPECT( alias       == 1 );
}

void TestParameterContextMenuIncludesValuePlaceholder()
{
	std::cout << "ParameterContextMenuIncludesValuePlaceholder\n";
	SuggestionEngine engine;
	const std::string buf =
		"RISE ASCII SCENE 5\n"
		"ambient_light\n"
		"{\n"
		"\t";
	auto ctx_sugs = engine.GetSuggestions( buf, buf.size(), SuggestionMode::ContextMenu );
	bool name_with_value = false, color_with_value = false;
	for( const Suggestion& s : ctx_sugs ) {
		if( s.displayText == "name"  && s.insertText.find("name ")  == 0 && s.insertText.size() > 5 ) name_with_value = true;
		if( s.displayText == "color" && s.insertText.find("color ") == 0 && s.insertText.size() > 6 ) color_with_value = true;
	}
	// Right-click insertions embed a value placeholder so the inserted
	// line is `name noname` / `color 0 0 0`, not just the bare keyword.
	EXPECT( name_with_value );
	EXPECT( color_with_value );

	// Inline-completion mode should NOT embed the placeholder so
	// typing `na` and accepting completes to just `name`, not
	// `name noname`.
	auto inline_sugs = engine.GetSuggestions( buf, buf.size(), SuggestionMode::InlineCompletion );
	bool name_bare = false;
	for( const Suggestion& s : inline_sugs ) {
		if( s.displayText == "name" && s.insertText == "name" ) name_bare = true;
	}
	EXPECT( name_bare );
}

void TestUnambiguousCompletionMarked()
{
	std::cout << "UnambiguousCompletionMarked\n";
	SuggestionEngine engine;
	// Typing `branc` inside a pixelpel_rasterizer should match exactly one
	// parameter (branching_threshold) by prefix, so that candidate should
	// be marked as the unambiguous ghost-text completion.
	const std::string buf =
		"RISE ASCII SCENE 5\n"
		"pixelpel_rasterizer\n"
		"{\n"
		"\tbranc";
	auto sugs = engine.GetSuggestions( buf, buf.size(), SuggestionMode::InlineCompletion );
	EXPECT( !sugs.empty() );
	if( !sugs.empty() ) {
		EXPECT( sugs[0].insertText == "branching_threshold" );
		EXPECT( sugs[0].isUnambiguousCompletion );
	}
}

int main()
{
	TestGrammarCoverage();
	TestContextSceneRoot();
	TestContextInBlockParamName();
	TestContextInBlockParamValue();
	TestContextInComment();
	TestSuggestChunkKeywordsAtRoot();
	TestSuggestParametersInAmbientLight();
	TestNonRepeatableParameterFilteredAfterUse();
	TestNameIndexSkipsCommentedOutNames();
	TestAliasShowsAsItsOwnSuggestion();
	TestParameterContextMenuIncludesValuePlaceholder();
	TestUnambiguousCompletionMarked();

	if( g_fails ) {
		std::cerr << g_fails << " failure(s)\n";
		return 1;
	}
	std::cout << "All tests passed\n";
	return 0;
}
