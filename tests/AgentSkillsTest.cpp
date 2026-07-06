//////////////////////////////////////////////////////////////////////
//
//  AgentSkillsTest.cpp - Facet 5 (agentic surface) slice S1: the
//    read_skill verb, the seed skills, and THE SNIPPET CONTRACT.
//
//  What is covered:
//    S0  Skills-root resolution: the ./skills/agent cwd fallback (the
//        suite runs from the repo root with no RISE_MEDIA_PATH), and
//        the RISE_SKILLS_PATH override taking precedence.
//    S1  The verb, STATELESS (dispatcher built with a NULL session):
//        the no-arg index lists exactly the seed skills, each
//        with a non-empty title + hook; a named fetch returns the
//        markdown containing the indexed title.
//    S2  Unknown / rejected names: an unknown skill -> -32602; the
//        traversal shapes "../x", "a/b", "..\\x" -> -32602 (path
//        safety); a ".md"-suffixed name is NOT served (the verb
//        appends .md itself -- "<name>.md" would resolve
//        "<name>.md.md", which does not exist).  S1 review round 1:
//        the named fetch is MEMBERSHIP-gated (the fetchable set IS
//        the listed set), closing the dotfile / directory-named-.md /
//        FIFO / Windows-device edges -- exercised in S0's override
//        root; and the index distinguishes a MISSING skills root
//        (note field set) from a present-but-empty one (no note).
//    S3  THE SNIPPET CONTRACT (the keystone): every ```rise fenced
//        block in every skill is a COMPLETE scene that parses to a
//        native-v7 CST and derives into a Job with ZERO diagnostics
//        and a non-null derived scene.  This test FAILS if anyone
//        edits a snippet into invalidity -- the skills can never rot.
//        (RED-proven during slice S1: a bogus parameter injected into
//        one snippet made this section fail with an "invalid
//        parameter(s)" derive diagnostic; reverted.)
//        S1 review round 1 adds THE RENDER CONTRACT: each snippet is
//        also RENDERED (AgentSession::LoadFromFile + Render on the
//        extracted scene) and its linear mean luma must land in
//        (0.02, 0.98) -- a snippet that renders black (the lone-glass-
//        under-a-delta-light anti-pattern) or a solid-white washout
//        FAILS.  Renders are not bit-deterministic across thread
//        schedules, so the contract is THRESHOLDS, never exact values.
//        (RED-proven: removing the crown-glass snippet's backdrop wall
//        + dome makes that snippet render 100% black and this section
//        fail; reverted.)  Plus the untagged-fence escape sweep: NO
//        non-```rise fenced block in any skill may contain
//        "RISE ASCII SCENE" -- scene content cannot dodge the
//        contract by dropping the fence tag.
//    S4  Chat-loop wiring: the provider-neutral tool table now carries
//        SEVEN tools (read_skill present in BOTH providers' request
//        bodies); SetSkillIndex("") omits the skills section (the
//        system prompt is byte-identical to SystemPrompt());
//        SetSkillIndex(text) appends the stable section to the next
//        BuildRequest's system prompt.
//    S5  End-to-end tool round: a canned tool_use fixture calling
//        read_skill round-trips ToolCallToJsonRpcLine -> the LIVE
//        dispatcher -> AddToolResult, and the NEXT request body
//        carries the skill markdown in the packed tool result.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentChatLoop.h"
#include "../src/Library/Agent/AgentChatCodecs.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/Json.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Interfaces/IJobPriv.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>   // mkfifo for the S0 FIFO-hardening fixture
#include <sys/types.h>
#endif

using namespace RISE;
using namespace RISE::Agent;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) ++g_pass;
	else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); }
}

// Portable set/unset env (the test pins the skills root deterministically:
// run_all_tests.sh runs from the repo root, so ./skills/agent resolves).
static void SetEnvVar( const char* name, const char* value )
{
#ifdef _WIN32
	_putenv_s( name, value ? value : "" );
#else
	if( value ) setenv( name, value, 1 );
	else        unsetenv( name );
#endif
}

// The seed skills (sorted byte-wise -- the index order contract).
// Grew from four to six with the object-modeling-recipes +
// modeling-from-image-captures pair (both auto-discovered by
// ListSkillNames -- production never hardcodes this list; only the
// test's own assertions do).
static const char* const kSeedSkills[] = {
	"lighting-recipes",
	"materials-and-media-basics",
	"modeling-from-image-captures",
	"modeling-workflow-and-geometry",
	"object-modeling-recipes",
	"scene-skeleton-and-conventions",
};
static const std::size_t kSeedSkillCount = sizeof( kSeedSkills ) / sizeof( kSeedSkills[0] );

static JsonValue ParseLine( const std::string& line )
{
	JsonValue v;
	std::string err;
	if( !JsonParse( line, v, err ) ) return JsonValue::MakeNull();
	return v;
}

// One read_skill request line (no name when `name` is null).
static std::string SkillRequest( int id, const char* name )
{
	JsonValue params = JsonValue::MakeObject();
	if( name ) params.set( "name", JsonValue::MakeString( name ) );
	JsonValue req = JsonValue::MakeObject();
	req.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
	req.set( "id", JsonValue::MakeNumber( id ) );
	req.set( "method", JsonValue::MakeString( "read_skill" ) );
	req.set( "params", params );
	return JsonSerialize( req );
}

// Extract EVERY fenced block from a markdown text as (tag, content)
// pairs -- tag is whatever follows the opening ``` ("" for untagged).
// The snippet contract consumes the "rise"-tagged blocks; the escape
// sweep audits everything else.
static std::vector< std::pair<std::string, std::string> > ExtractFencedBlocks( const std::string& md )
{
	std::vector< std::pair<std::string, std::string> > blocks;
	std::string cur, tag;
	bool inside = false;
	std::size_t pos = 0;
	while( pos <= md.size() ) {
		std::size_t eol = md.find( '\n', pos );
		if( eol == std::string::npos ) eol = md.size();
		std::string line = md.substr( pos, eol - pos );
		if( !line.empty() && line[line.size()-1] == '\r' ) line.erase( line.size()-1 );
		if( !inside && line.rfind( "```", 0 ) == 0 ) {
			inside = true;
			tag = line.substr( 3 );
			cur.clear();
		}
		else if( inside && line == "```" ) {
			inside = false;
			blocks.push_back( std::make_pair( tag, cur ) );
		}
		else if( inside ) {
			cur += line;
			cur += '\n';
		}
		if( eol >= md.size() ) break;
		pos = eol + 1;
	}
	return blocks;
}

// The ```rise-tagged blocks only (the snippet contract's input).
static std::vector<std::string> ExtractRiseBlocks( const std::string& md )
{
	std::vector<std::string> blocks;
	const std::vector< std::pair<std::string, std::string> > all = ExtractFencedBlocks( md );
	for( std::size_t i = 0; i < all.size(); ++i )
		if( all[i].first == "rise" ) blocks.push_back( all[i].second );
	return blocks;
}

// A writable temp directory (the same resolution the S0 fixture uses).
static std::string TempDir()
{
	const char* base = std::getenv( "TMPDIR" );
	if( !base ) base = std::getenv( "TMP" );   // Windows spelling
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += '/';
	return dir;
}

//----------------------------------------------------------------------
// S0: skills-root resolution.
//----------------------------------------------------------------------
static void TestRootResolution()
{
	std::printf( "S0: skills-root resolution...\n" );

	// The cwd fallback: with neither env var set, ./skills/agent serves
	// the seed skills (the suite runs from the repo root).
	SetEnvVar( "RISE_SKILLS_PATH", nullptr );
	SetEnvVar( "RISE_MEDIA_PATH", nullptr );
	{
		const AgentSkillResult r = AgentSession::ReadSkill();
		Check( r.ok, "fallback root: index read ok" );
		Check( r.index.size() == kSeedSkillCount,
		       "fallback root (./skills/agent) serves the seed skills" );
	}

	// RISE_SKILLS_PATH override wins: a temp root with ONE fake skill.
	{
		const char* base = std::getenv( "TMPDIR" );
		if( !base ) base = std::getenv( "TMP" );   // Windows spelling
		std::string dir = base ? base : "/tmp";
		if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += '/';
		dir += "rise_agent_skills_test";
#ifdef _WIN32
		system( ( "mkdir \"" + dir + "\" 2>NUL" ).c_str() );
#else
		system( ( "mkdir -p '" + dir + "'" ).c_str() );
#endif
		const std::string fake = dir + "/fake-skill.md";
		{
			std::ofstream f( fake.c_str(), std::ios::binary );
			f << "# Fake Skill\n> hook: A test-only skill.\n\nbody\n";
		}
		// S1 review round 1 hardening fixtures: a dotfile skill, a
		// DIRECTORY named "<x>.md", and (POSIX) a FIFO named "<x>.md".
		// None may index; none may be fetched by name (the fetchable
		// set IS the listed set -- a FIFO fetch would otherwise HANG).
		const std::string dotfile = dir + "/.hidden.md";
		{
			std::ofstream f( dotfile.c_str(), std::ios::binary );
			f << "# Hidden\n> hook: Should never index.\n";
		}
		const std::string dirmd = dir + "/dirskill.md";
#ifdef _WIN32
		system( ( "mkdir \"" + dirmd + "\" 2>NUL" ).c_str() );
#else
		system( ( "mkdir -p '" + dirmd + "'" ).c_str() );
		const std::string fifomd = dir + "/pipeskill.md";
		std::remove( fifomd.c_str() );
		Check( mkfifo( fifomd.c_str(), 0600 ) == 0, "test fixture: created the FIFO" );
#endif

		SetEnvVar( "RISE_SKILLS_PATH", dir.c_str() );
		const AgentSkillResult r = AgentSession::ReadSkill();
		Check( r.ok && r.index.size() == 1, "RISE_SKILLS_PATH override root serves exactly the fake skill (dotfile / dir / FIFO excluded)" );
		if( r.index.size() == 1 ) {
			Check( r.index[0].name == "fake-skill", "override index carries the bare name (no .md)" );
			Check( r.index[0].title == "Fake Skill", "title parsed from the '# ' first line" );
			Check( r.index[0].hook == "A test-only skill.", "hook parsed from the '> hook:' second line" );
		}
		Check( r.note.empty(), "present-but-sparse root carries NO missing-root note" );

		// Membership gate: unlisted names are NOT fetchable even though
		// a same-named filesystem entry exists under the root.
		Check( !AgentSession::ReadSkill( ".hidden" ).ok,  "dotfile skill is not fetchable (unlisted)" );
		Check( !AgentSession::ReadSkill( "dirskill" ).ok, "directory named .md is not fetchable (unlisted)" );
#ifndef _WIN32
		Check( !AgentSession::ReadSkill( "pipeskill" ).ok, "FIFO named .md is not fetchable (unlisted; a read would hang)" );
#endif
		Check( AgentSession::ReadSkill( "fake-skill" ).ok, "the listed skill still fetches through the membership gate" );

		// Missing-root vs empty-root: a nonexistent root returns an
		// EMPTY index WITH the advisory note; the sparse-but-present
		// root above returned no note.
		const std::string missing = dir + "/no_such_subdir";
		SetEnvVar( "RISE_SKILLS_PATH", missing.c_str() );
		{
			const AgentSkillResult m = AgentSession::ReadSkill();
			Check( m.ok && m.index.empty(), "missing root: index call still ok + empty" );
			Check( !m.note.empty(), "missing root: the index result carries the advisory note" );
		}

		SetEnvVar( "RISE_SKILLS_PATH", nullptr );
		std::remove( fake.c_str() );
		std::remove( dotfile.c_str() );
#ifdef _WIN32
		system( ( "rmdir \"" + dirmd + "\" 2>NUL" ).c_str() );
#else
		std::remove( fifomd.c_str() );
		system( ( "rmdir '" + dirmd + "'" ).c_str() );
#endif
	}
}

//----------------------------------------------------------------------
// S1: the verb, stateless, through the dispatcher.
//----------------------------------------------------------------------
static void TestVerbIndexAndFetch( AgentRpcDispatcher& rpc )
{
	std::printf( "S1: read_skill index + named fetch (null-session dispatcher)...\n" );

	// Index: exactly the seed skills, non-empty titles + hooks.
	const JsonValue env = ParseLine( rpc.HandleLine( SkillRequest( 1, nullptr ) ) );
	Check( env.isObject() && !env.find( "error" ), "no-arg read_skill succeeds with NO session (stateless)" );
	const JsonValue& skills = env.get( "result" ).get( "skills" );
	Check( skills.isArray() && skills.size() == kSeedSkillCount,
	       "index lists exactly the seed skills" );
	for( std::size_t i = 0; i < kSeedSkillCount && i < skills.size(); ++i ) {
		const JsonValue& e = skills.at( i );
		Check( e.get( "name" ).asString() == kSeedSkills[i],
		       std::string( "index[" ) + std::to_string( i ) + "] is " + kSeedSkills[i] + " (sorted)" );
		Check( !e.get( "title" ).asString().empty(),
		       std::string( kSeedSkills[i] ) + " has a non-empty title" );
		Check( !e.get( "hook" ).asString().empty(),
		       std::string( kSeedSkills[i] ) + " has a non-empty hook" );
	}

	// Named fetch: the markdown opens with the indexed title.
	for( std::size_t i = 0; i < kSeedSkillCount && i < skills.size(); ++i ) {
		const std::string title = skills.at( i ).get( "title" ).asString();
		const JsonValue fenv = ParseLine( rpc.HandleLine( SkillRequest( 2, kSeedSkills[i] ) ) );
		Check( fenv.isObject() && !fenv.find( "error" ),
		       std::string( "named fetch of " ) + kSeedSkills[i] + " succeeds" );
		const JsonValue& res = fenv.get( "result" );
		Check( res.get( "name" ).asString() == kSeedSkills[i], "result echoes the name" );
		const std::string md = res.get( "markdown" ).asString();
		Check( !md.empty() && md.find( title ) != std::string::npos,
		       std::string( kSeedSkills[i] ) + " markdown contains its indexed title" );
	}
}

//----------------------------------------------------------------------
// S2: unknown + rejected names.
//----------------------------------------------------------------------
static void TestVerbRejections( AgentRpcDispatcher& rpc )
{
	std::printf( "S2: unknown / traversal / non-md names are rejected...\n" );

	const char* bad[] = { "no-such-skill",                       // unknown
	                      "../x", "a/b", "..\\x",                // traversal shapes
	                      "scene-skeleton-and-conventions.md" }; // .md-suffixed (verb appends .md itself)
	for( std::size_t i = 0; i < sizeof( bad ) / sizeof( bad[0] ); ++i ) {
		const JsonValue env = ParseLine( rpc.HandleLine( SkillRequest( 3, bad[i] ) ) );
		const JsonValue* err = env.find( "error" );
		Check( err != nullptr && err->get( "code" ).asNumber() == -32602.0,
		       std::string( "'" ) + bad[i] + "' -> -32602 error" );
	}

	// A non-string name is an invalid-params error too.
	const JsonValue env = ParseLine( rpc.HandleLine(
		"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"read_skill\",\"params\":{\"name\":42}}" ) );
	const JsonValue* err = env.find( "error" );
	Check( err != nullptr && err->get( "code" ).asNumber() == -32602.0,
	       "non-string 'name' -> -32602 error" );
}

//----------------------------------------------------------------------
// S3: THE SNIPPET CONTRACT.
//----------------------------------------------------------------------
static void TestSnippetContract( AgentRpcDispatcher& rpc )
{
	std::printf( "S3: THE SNIPPET CONTRACT (every ```rise block parses + derives clean + RENDERS non-black)...\n" );

	// THE RENDER CONTRACT thresholds.  Linear mean luma (Rec.709
	// weights over the pre-quantization channel means) must exceed
	// kMinLuma -- a black render (the lone-specular-under-a-delta-light
	// anti-pattern) fails -- and stay under kMaxLuma, so a solid-white
	// washout (e.g. an emissive quad filling the frustum) fails too.
	// THRESHOLDS, never exact values: renders are not bit-deterministic
	// across thread schedules.
	const double kMinLuma = 0.02;
	const double kMaxLuma = 0.98;

	const std::string scenePath = TempDir() + "rise_agent_skills_snippet.RISEscene";
	double renderSeconds = 0.0;

	std::size_t totalSnippets = 0;
	for( std::size_t s = 0; s < kSeedSkillCount; ++s ) {
		const JsonValue fenv = ParseLine( rpc.HandleLine( SkillRequest( 5, kSeedSkills[s] ) ) );
		const std::string md = fenv.get( "result" ).get( "markdown" ).asString();
		Check( !md.empty(), std::string( kSeedSkills[s] ) + ": markdown fetched" );

		const std::vector<std::string> blocks = ExtractRiseBlocks( md );
		Check( !blocks.empty(), std::string( kSeedSkills[s] ) + ": carries at least one ```rise snippet" );
		totalSnippets += blocks.size();

		for( std::size_t b = 0; b < blocks.size(); ++b ) {
			const std::string label = std::string( kSeedSkills[s] ) + " snippet " + std::to_string( b );

			// (a) bytes -> CST, and the CST is native-v7 loadable.
			Cst::Document doc = Cst::ParseToCst( blocks[b] );
			Check( Cst::SerializeCst( doc ) == blocks[b], label + ": CST round-trips losslessly" );
			Check( Cst::IsNativeV7Document( doc ), label + ": is a native-v7 document" );

			// (b) derive into a fresh Job: ZERO diagnostics + a non-null
			// derived scene.  THIS is the assertion that fails when a
			// snippet is edited into invalidity.
			IJobPriv* job = nullptr;
			Check( RISE_CreateJobPriv( &job ) && job, label + ": created a fresh Job" );
			if( !job ) continue;
			std::vector<std::string> diags;
			const int applied = Cst::DeriveToJob( doc, *job, &diags );
			for( std::size_t d = 0; d < diags.size(); ++d )
				std::printf( "    %s DIAGNOSTIC: %s\n", label.c_str(), diags[d].c_str() );
			Check( diags.empty(), label + ": derives with ZERO diagnostics" );
			Check( applied > 0, label + ": derive applied at least one chunk" );
			Check( job->GetScene() != nullptr, label + ": derived scene is non-null" );
			job->release();

			// (c) THE RENDER CONTRACT: the snippet renders, and shows
			// SOMETHING -- through the agent surface itself (the same
			// LoadFromFile + Render an agent drives), asserting mean
			// luma inside (kMinLuma, kMaxLuma).
			{
				std::ofstream f( scenePath.c_str(), std::ios::binary );
				f << blocks[b];
			}
			const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
			std::unique_ptr<AgentSession> session = AgentSession::LoadFromFile( scenePath );
			Check( session != nullptr, label + ": loads through the agent surface" );
			if( session ) {
				const AgentRenderResult rr = session->Render();
				Check( rr.ok, label + ": renders ok (" + rr.message + ")" );
				const double luma = 0.2126 * rr.meanR + 0.7152 * rr.meanG + 0.0722 * rr.meanB;
				std::printf( "    %s rendered %dx%d mean RGB (%.4f, %.4f, %.4f) luma %.4f\n",
				             label.c_str(), rr.width, rr.height, rr.meanR, rr.meanG, rr.meanB, luma );
				Check( rr.ok && luma > kMinLuma,
				       label + ": mean luma " + std::to_string( luma ) + " > " + std::to_string( kMinLuma ) + " (not black)" );
				Check( rr.ok && luma < kMaxLuma,
				       label + ": mean luma " + std::to_string( luma ) + " < " + std::to_string( kMaxLuma ) + " (not a washout)" );
			}
			renderSeconds += std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count();
		}
	}
	std::remove( scenePath.c_str() );
	std::printf( "  render contract: %.1f seconds over %d snippets\n",
	             renderSeconds, static_cast<int>( totalSnippets ) );

	// A rot guard for the extraction itself: the six seed skills ship
	// THIRTEEN snippets total (lighting-recipes 3, materials-and-media-
	// basics 3, modeling-from-image-captures 1, modeling-workflow-and-
	// geometry 2, object-modeling-recipes 3, scene-skeleton-and-
	// conventions 1) -- if the fence tag or extraction regresses, this
	// trips before a snippet silently escapes checking.
	Check( totalSnippets == 13, "the seed skills carry the expected 13 ```rise snippets in total (got " +
	       std::to_string( totalSnippets ) + ")" );
}

//----------------------------------------------------------------------
// S3b: the untagged-fence escape sweep.
//----------------------------------------------------------------------
static void TestFenceEscapes( AgentRpcDispatcher& rpc )
{
	std::printf( "S3b: no non-```rise fence carries scene content...\n" );

	for( std::size_t s = 0; s < kSeedSkillCount; ++s ) {
		const JsonValue fenv = ParseLine( rpc.HandleLine( SkillRequest( 6, kSeedSkills[s] ) ) );
		const std::string md = fenv.get( "result" ).get( "markdown" ).asString();
		const std::vector< std::pair<std::string, std::string> > all = ExtractFencedBlocks( md );
		for( std::size_t b = 0; b < all.size(); ++b ) {
			if( all[b].first == "rise" ) continue;
			Check( all[b].second.find( "RISE ASCII SCENE" ) == std::string::npos,
			       std::string( kSeedSkills[s] ) + " fence " + std::to_string( b ) +
			       " (tag '" + all[b].first + "'): scene content must use the ```rise tag (the snippet contract)" );
		}
	}
}

//----------------------------------------------------------------------
// S4: chat-loop tool table + SetSkillIndex.
//----------------------------------------------------------------------
static JsonValue ParseBody( const std::string& body )
{
	JsonValue root;
	std::string err;
	if( !JsonParse( body, root, err ) ) return JsonValue::MakeNull();
	return root;
}

static void TestChatLoopWiring()
{
	std::printf( "S4: chat-loop tool table (nine tools) + SetSkillIndex...\n" );

	// Anthropic: nine tools (S2 added insert_chunk/remove_chunk), read_skill present with a schema.
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "hello" );
		JsonValue root = ParseBody( loop.BuildRequest( "sk-test" ).body );
		const JsonValue& tools = root.get( "tools" );
		Check( tools.isArray() && tools.size() == 9, "anthropic body carries nine tools" );
		bool saw = false;
		for( std::size_t i = 0; i < tools.size(); ++i ) {
			if( tools.at( i ).get( "name" ).asString() != "read_skill" ) continue;
			saw = true;
			Check( tools.at( i ).get( "input_schema" ).isObject(), "read_skill has an input_schema" );
			const std::string desc = tools.at( i ).get( "description" ).asString();
			Check( desc.find( "no name" ) != std::string::npos || desc.find( "NO name" ) != std::string::npos,
			       "read_skill description teaches the no-name-first index call" );
		}
		Check( saw, "anthropic tool list includes read_skill" );
	}

	// Gemini: nine functionDeclarations, read_skill present.
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Gemini );
		loop.AddUserMessage( "hello" );
		JsonValue root = ParseBody( loop.BuildRequest( "sk-test" ).body );
		const JsonValue& decls = root.get( "tools" ).at( 0 ).get( "functionDeclarations" );
		Check( decls.isArray() && decls.size() == 9, "gemini body carries nine functionDeclarations" );
		bool saw = false;
		for( std::size_t i = 0; i < decls.size(); ++i )
			if( decls.at( i ).get( "name" ).asString() == "read_skill" ) saw = true;
		Check( saw, "gemini functionDeclarations include read_skill" );
	}

	// SetSkillIndex: "" omits the section; a set index appears verbatim
	// in the NEXT BuildRequest's system prompt (base prompt unchanged).
	{
		AgentChatLoop loop;
		loop.AddUserMessage( "hello" );

		loop.SetSkillIndex( "" );
		JsonValue root = ParseBody( loop.BuildRequest( "sk-test" ).body );
		Check( root.get( "system" ).asString() == AgentChatLoop::SystemPrompt(),
		       "SetSkillIndex(\"\") sends the base system prompt unchanged (section omitted)" );
		Check( root.get( "system" ).asString().find( "Available skills:" ) == std::string::npos,
		       "empty index -> no 'Available skills:' section" );

		const std::string index =
			"scene-skeleton-and-conventions -- Read before authoring a scene from scratch.";
		loop.SetSkillIndex( index );
		root = ParseBody( loop.BuildRequest( "sk-test" ).body );
		const std::string sys = root.get( "system" ).asString();
		Check( sys.rfind( AgentChatLoop::SystemPrompt(), 0 ) == 0,
		       "skills section is APPENDED (the base prompt is the prefix)" );
		Check( sys.find( "Available skills:\n" + index ) != std::string::npos,
		       "system prompt carries the 'Available skills:' section with the index text" );
		Check( sys.find( "Call read_skill before scene-authoring tasks." ) != std::string::npos,
		       "system prompt carries the read_skill call-to-action" );

		// The setting is provider-neutral config: it survives SetProvider.
		loop.SetProvider( ChatProvider::Gemini );
		loop.AddUserMessage( "hello again" );
		root = ParseBody( loop.BuildRequest( "sk-test" ).body );
		const std::string gsys = root.get( "systemInstruction" ).get( "parts" ).at( 0 ).get( "text" ).asString();
		Check( gsys.find( "Available skills:" ) != std::string::npos,
		       "skill index survives SetProvider (provider-neutral config)" );
	}
}

//----------------------------------------------------------------------
// S5: end-to-end read_skill tool round against the live dispatcher.
//----------------------------------------------------------------------
static void TestToolRound( AgentRpcDispatcher& rpc )
{
	std::printf( "S5: read_skill tool round (fixture -> dispatcher -> next body)...\n" );

	AgentChatLoop loop;
	loop.AddUserMessage( "How do I author a scene?" );

	// A canned Anthropic tool_use turn requesting the skeleton skill.
	const std::string fixture =
		"{\"id\":\"msg_01Skill\",\"type\":\"message\",\"role\":\"assistant\","
		"\"model\":\"claude-sonnet-5\",\"content\":"
		"[{\"type\":\"text\",\"text\":\"Reading the authoring skill first.\"},"
		"{\"type\":\"tool_use\",\"id\":\"toolu_skill1\",\"name\":\"read_skill\","
		"\"input\":{\"name\":\"scene-skeleton-and-conventions\"}}],"
		"\"stop_reason\":\"tool_use\",\"stop_sequence\":null,"
		"\"usage\":{\"input_tokens\":128,\"output_tokens\":64}}";

	ChatStepResult st = loop.HandleResponse( 200, fixture );
	Check( st.kind == ChatStepResult::Kind::ToolCalls && st.toolCalls.size() == 1,
	       "read_skill tool_use fixture -> one ToolCall" );
	if( st.toolCalls.size() != 1 ) return;

	// ToolCallToJsonRpcLine -> the LIVE dispatcher.
	const std::string line = loop.ToolCallToJsonRpcLine( st.toolCalls[0], 11 );
	const JsonValue req = ParseLine( line );
	Check( req.get( "method" ).asString() == "read_skill" &&
	       req.get( "params" ).get( "name" ).asString() == "scene-skeleton-and-conventions",
	       "rpc line carries the verb + name param" );
	const std::string resp = rpc.HandleLine( line );
	const JsonValue renv = ParseLine( resp );
	Check( !renv.find( "error" ), "live dispatcher answers the fixture's call" );
	loop.AddToolResult( st.toolCalls[0], resp );

	// The NEXT body's packed tool result carries the markdown.
	JsonValue root = ParseBody( loop.BuildRequest( "sk-test" ).body );
	const JsonValue& msgs = root.get( "messages" );
	Check( msgs.isArray() && msgs.size() == 3, "transcript is user + assistant + tool-results" );
	const JsonValue& tr = msgs.at( msgs.size() - 1 ).get( "content" ).at( 0 );
	Check( tr.get( "type" ).asString() == "tool_result" &&
	       tr.get( "tool_use_id" ).asString() == "toolu_skill1",
	       "tool result answers the fixture's tool_use id" );
	// The text block is the serialized JSON-RPC result: parse it and
	// check the markdown STRUCTURALLY (no fragile escaped-substring
	// matching against the raw body).
	const std::string text = tr.get( "content" ).at( 0 ).get( "text" ).asString();
	const JsonValue payload = ParseLine( text );
	const std::string md = payload.get( "markdown" ).asString();
	Check( md.find( "FROM-surface-TO-light" ) != std::string::npos,
	       "the next body carries the skill markdown (direction convention present)" );
}

int main()
{
	std::printf( "=== AgentSkillsTest (Facet 5 slice S1: read_skill + seed skills + snippet contract) ===\n" );

	// Pin the skills root to the deterministic ./skills/agent fallback:
	// the suite runs from the repo root, and a dev-shell RISE_MEDIA_PATH
	// pointing elsewhere must not redirect the reads.
	SetEnvVar( "RISE_SKILLS_PATH", nullptr );
	SetEnvVar( "RISE_MEDIA_PATH", nullptr );

	TestRootResolution();

	// One dispatcher with NO session for everything else -- read_skill
	// is STATELESS and must work in the no-head bootstrap.
	AgentRpcDispatcher rpc( std::unique_ptr<AgentSession>( nullptr ) );
	TestVerbIndexAndFetch( rpc );
	TestVerbRejections( rpc );
	TestSnippetContract( rpc );
	TestFenceEscapes( rpc );
	TestChatLoopWiring();
	TestToolRound( rpc );

	std::printf( "=== AgentSkillsTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
