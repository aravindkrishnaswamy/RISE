//////////////////////////////////////////////////////////////////////
//
//  AgentSkillsTest.cpp - Facet 5 (agentic surface) slice S1: the
//    read_skill verb, the seed skills, and THE SNIPPET CONTRACT.
//
//  What is covered:
//    S0  Skills-root resolution, THE WHOLE PRECEDENCE LADDER:
//        $RISE_SKILLS_PATH > $RISE_MEDIA_PATH + "skills/agent/" >
//        "./skills/agent/" (the cwd fallback -- the suite runs from
//        the repo root).  The middle tier is why both desktop GUIs
//        set tier 1 at startup: RISE_MEDIA_PATH follows the OPEN
//        SCENE, so binding skills to it made them vanish for any
//        scene outside a RISE project root.  Also: an EMPTY index is
//        never silent -- it always carries the advisory note, for a
//        MISSING root and a present-but-empty one alike, while a
//        healthy index carries none.
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
//    S3d THE PROCEDURAL-TEXTURE TEACHING: procedural-textures must
//        carry the intent -> painter-family decision map, the 2D-UV vs
//        3D-solid domain rule, and THE ISCALARPAINTER TRAP (including
//        the empirically-established sub-trap that a 3D solid painter
//        routed through the function2d bridge derives clean and
//        evaluates spatially CONSTANT), and materials-and-media-basics
//        must hand off to it.  Positive assertions only, same
//        discipline as S3c.
//    S4  Chat-loop wiring: every provider codec emits the SAME
//        provider-neutral tool table, at the size TestChatLoopWiring
//        asserts (read_skill present in all three providers' request
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

// The platform temp directory, WITH a trailing slash.
static std::string TempDirBase()
{
	const char* base = std::getenv( "TMPDIR" );
	if( !base ) base = std::getenv( "TMP" );   // Windows spelling
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += '/';
	return dir;
}

// The seed skills (sorted byte-wise -- the index order contract).
// Grew from six to seven with the observe-modes toolkit-slice-4 skill,
// and to EIGHT with procedural-textures (the spatially-varying-painter
// skill: models were reaching for uniformcolor_painter on every surface
// because no skill mentioned any of the other 35 painter kinds)
// (auto-discovered by ListSkillNames -- production never hardcodes
// this list; only the test's own assertions do).
static const char* const kSeedSkills[] = {
	"lighting-recipes",
	"materials-and-media-basics",
	"modeling-from-image-captures",
	"modeling-workflow-and-geometry",
	"object-modeling-recipes",
	"observe-modes",
	"procedural-textures",
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
		// A HEALTHY index carries NO advisory -- the note below is the
		// empty-index signal, not a permanent decoration.
		Check( r.note.empty(), "healthy index carries no advisory note" );
	}

	// Tier 2: $RISE_MEDIA_PATH + "skills/agent/".  This tier is the
	// DEFECT the GUI fix routes around -- a GUI re-points RISE_MEDIA_PATH
	// at the open scene's project root on every load, so skills bound to
	// it disappear for any scene outside such a root.  Pinned here both
	// ways: it resolves when it is the only variable set, and it LOSES to
	// RISE_SKILLS_PATH (which is what each GUI now sets at startup).
	//
	// The media root used here holds ONE skill under skills/agent, so a
	// tier-2 hit is distinguishable from the cwd fallback (which serves
	// kSeedSkillCount) -- otherwise "tier 2 resolved" and "tier 2 was
	// skipped" look identical from the repo root.
	{
		const std::string mediaRoot = TempDirBase() + "rise_agent_media_root";
		const std::string mediaSkills = mediaRoot + "/skills/agent";
#ifdef _WIN32
		system( ( "mkdir \"" + mediaSkills + "\" 2>NUL" ).c_str() );
#else
		system( ( "mkdir -p '" + mediaSkills + "'" ).c_str() );
#endif
		{
			std::ofstream f( ( mediaSkills + "/media-tier-skill.md" ).c_str(), std::ios::binary );
			f << "# Media Tier Skill\n> hook: Reached only through RISE_MEDIA_PATH.\n";
		}

		SetEnvVar( "RISE_MEDIA_PATH", ( mediaRoot + "/" ).c_str() );
		const AgentSkillResult viaMedia = AgentSession::ReadSkill();
		Check( viaMedia.ok && viaMedia.index.size() == 1
		       && viaMedia.index[0].name == "media-tier-skill",
		       "tier 2: $RISE_MEDIA_PATH + skills/agent/ resolves when RISE_SKILLS_PATH is unset" );

		// A media path with NO skills/agent under it: exactly the GUI
		// failure mode (scene opened outside a RISE project tree) --
		// zero skills, and NOT silent.
		SetEnvVar( "RISE_MEDIA_PATH", ( mediaRoot + "/no_skills_here/" ).c_str() );
		const AgentSkillResult stranded = AgentSession::ReadSkill();
		Check( stranded.ok && stranded.index.empty(),
		       "tier 2 pointed at a tree with no skills/agent yields an EMPTY index" );
		Check( stranded.note.find( "NO SKILLS ARE AVAILABLE" ) != std::string::npos,
		       "that empty index is NOT silent -- it carries the loud no-skills advisory" );

		// Tier 1 beats tier 2 -- this is exactly what each GUI now sets at
		// startup, over a media path that follows the open scene.
		SetEnvVar( "RISE_SKILLS_PATH", "./skills/agent" );
		const AgentSkillResult viaSkills = AgentSession::ReadSkill();
		Check( viaSkills.ok && viaSkills.index.size() == kSeedSkillCount,
		       "tier 1 ($RISE_SKILLS_PATH) WINS over a tier-2 media path with no skills" );

		SetEnvVar( "RISE_SKILLS_PATH", nullptr );
		SetEnvVar( "RISE_MEDIA_PATH", nullptr );
		std::remove( ( mediaSkills + "/media-tier-skill.md" ).c_str() );
#ifdef _WIN32
		system( ( "rmdir /s /q \"" + mediaRoot + "\" 2>NUL" ).c_str() );
#else
		system( ( "rm -rf '" + mediaRoot + "'" ).c_str() );
#endif
	}

	// RISE_SKILLS_PATH override wins: a temp root with ONE fake skill.
	{
		std::string dir = TempDirBase() + "rise_agent_skills_test";
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
		Check( r.note.empty(), "present-but-sparse root: a NON-empty index carries no note" );

		// Membership gate: unlisted names are NOT fetchable even though
		// a same-named filesystem entry exists under the root.
		Check( !AgentSession::ReadSkill( ".hidden" ).ok,  "dotfile skill is not fetchable (unlisted)" );
		Check( !AgentSession::ReadSkill( "dirskill" ).ok, "directory named .md is not fetchable (unlisted)" );
#ifndef _WIN32
		Check( !AgentSession::ReadSkill( "pipeskill" ).ok, "FIFO named .md is not fetchable (unlisted; a read would hang)" );
#endif
		Check( AgentSession::ReadSkill( "fake-skill" ).ok, "the listed skill still fetches through the membership gate" );

		// EVERY empty index carries the advisory -- an agent with no
		// skills must never look like an agent with skills.  The two
		// CAUSES stay distinguishable in the wording (the note names the
		// root and says whether it exists), which is what tells a
		// miswired root from "nothing installed here".
		const std::string missing = dir + "/no_such_subdir";
		SetEnvVar( "RISE_SKILLS_PATH", missing.c_str() );
		{
			const AgentSkillResult m = AgentSession::ReadSkill();
			Check( m.ok && m.index.empty(), "missing root: index call still ok + empty" );
			Check( !m.note.empty(), "missing root: the index result carries the advisory note" );
			Check( m.note.find( "NO SKILLS ARE AVAILABLE" ) != std::string::npos,
			       "missing root: the note says plainly that no skills are available" );
			Check( m.note.find( "does not exist" ) != std::string::npos,
			       "missing root: the note reports the root as ABSENT" );
			Check( m.note.find( missing ) != std::string::npos,
			       "missing root: the note names the root that was tried" );
		}

		// A root that EXISTS but holds no skills: same loud advisory,
		// different diagnosis.  (Before this, an existing-but-empty root
		// returned a bare empty list -- indistinguishable from healthy.)
		const std::string emptyRoot = dir + "/empty_root";
#ifdef _WIN32
		system( ( "mkdir \"" + emptyRoot + "\" 2>NUL" ).c_str() );
#else
		system( ( "mkdir -p '" + emptyRoot + "'" ).c_str() );
#endif
		SetEnvVar( "RISE_SKILLS_PATH", emptyRoot.c_str() );
		{
			const AgentSkillResult e = AgentSession::ReadSkill();
			Check( e.ok && e.index.empty(), "empty root: index call still ok + empty" );
			Check( e.note.find( "NO SKILLS ARE AVAILABLE" ) != std::string::npos,
			       "empty root: the note says plainly that no skills are available" );
			Check( e.note.find( "exists but holds no readable" ) != std::string::npos,
			       "empty root: the note reports the root as PRESENT but skill-less" );
		}

		SetEnvVar( "RISE_SKILLS_PATH", nullptr );
#ifdef _WIN32
		system( ( "rmdir \"" + emptyRoot + "\" 2>NUL" ).c_str() );
#else
		system( ( "rmdir '" + emptyRoot + "'" ).c_str() );
#endif
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
	// A HEALTHY index carries no `note` on the wire -- the field is the
	// empty-index signal, so its presence must MEAN something.
	Check( env.get( "result" ).find( "note" ) == nullptr,
	       "healthy index: the RPC result has NO note field" );
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

	// THE EMPTY INDEX REACHES THE AGENT AS AN ADVISORY, not a bare empty
	// list.  Same shape the in-app chat drivers read (both GUIs surface
	// this note to the user); this is the wire-level half of that.
	{
		SetEnvVar( "RISE_SKILLS_PATH", "./no_such_skills_root_for_test" );
		const JsonValue empty = ParseLine( rpc.HandleLine( SkillRequest( 5, nullptr ) ) );
		const JsonValue& res = empty.get( "result" );
		Check( res.get( "skills" ).isArray() && res.get( "skills" ).size() == 0,
		       "empty-index RPC: skills is an empty array (still a success, not an error)" );
		const JsonValue* note = res.find( "note" );
		Check( note != nullptr && note->isString()
		       && note->asString().find( "NO SKILLS ARE AVAILABLE" ) != std::string::npos,
		       "empty-index RPC: the result carries the loud no-skills advisory" );
		SetEnvVar( "RISE_SKILLS_PATH", nullptr );
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

	// A rot guard for the extraction itself: the eight seed skills ship
	// SEVENTEEN snippets total (lighting-recipes 3, materials-and-media-
	// basics 3, modeling-from-image-captures 1, modeling-workflow-and-
	// geometry 2, object-modeling-recipes 4, observe-modes 1,
	// procedural-textures 2, scene-skeleton-and-conventions 1) -- if the
	// fence tag or extraction regresses, this trips before a snippet
	// silently escapes checking.  object-modeling-recipes gained Recipe 4
	// (the turned-vessel profile: sdf_geometry roundcone/smin chain + a
	// sweep_geometry neck) with the lathe-forms guidance; procedural-
	// textures added the nested-perlin3d wood top and the domainwarp3d
	// marble slab (whose scalar-pipe roughness is the ISCALARPAINTER-trap
	// worked example).
	Check( totalSnippets == 17, "the seed skills carry the expected 17 ```rise snippets in total (got " +
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
// S6: observe-modes (Toolkit slice 4) -- factual spot-checks tying the
// skill's claims to the actual verb constants/behaviour, PLUS the
// be-the-agent check: drive each decision-table row through a LIVE
// dispatcher (a real loaded session, not the stateless null-session
// `rpc` used above) and confirm the claimed result shape actually
// holds -- "derive-checked is NOT render-checked" (the S1 lesson)
// extends here to "documented is not DRIVEN".
//----------------------------------------------------------------------
static void TestObserveModesTeaching( AgentRpcDispatcher& statelessRpc )
{
	std::printf( "S6: observe-modes -- factual spot-checks + be-the-agent verification per decision-table row...\n" );

	// Spot-checks: tie the skill's stated numbers/strings to the actual
	// constants/behaviour baked into AgentSession.cpp (kDraftMaxSamples,
	// the read_viewport reason strings, the width/height clamp range) --
	// the constant itself is private/unexported, so this is a literal
	// cross-reference rather than a shared-symbol comparison.
	const JsonValue fenv = ParseLine( statelessRpc.HandleLine( SkillRequest( 7, "observe-modes" ) ) );
	const std::string md = fenv.get( "result" ).get( "markdown" ).asString();
	Check( !md.empty(), "observe-modes: markdown fetched" );
	Check( md.find( "capped at 4" ) != std::string::npos,
	       "observe-modes states the draft samples cap as 4 (matches AgentSession.cpp's kDraftMaxSamples)" );
	// Round-10 finding 4: the skill used to name only two of
	// read_viewport's unavailability reasons and this assertion said
	// "both".  There are SEVEN (see AgentSession.h's ReadViewport doc
	// and the switch in AgentSession.cpp that emits them).  Assert ALL
	// seven verbatim, so the skill cannot silently fall behind the wire
	// values again -- a model-facing doc that enumerates a closed set
	// incompletely is worse than one that does not enumerate it.
	static const char* const kViewportReasons[] = {
		"no_controller", "no_frame_yet", "editor_transaction_in_progress",
		"render_in_progress", "editor_shutting_down",
		"editor_interaction_finalize_failed", "editor_interaction_unrecoverable"
	};
	for( size_t ri = 0; ri < sizeof( kViewportReasons ) / sizeof( kViewportReasons[0] ); ++ri ) {
		Check( md.find( kViewportReasons[ri] ) != std::string::npos,
		       ( std::string( "observe-modes names read_viewport reason \"" )
		         + kViewportReasons[ri] + "\" verbatim" ).c_str() );
	}
	// Round-10 finding 4: the skill's OLD guidance told the model to
	// "fall back to a `render` call instead of retrying" on ANY
	// available:false.  That is wrong for the editor/admission reasons --
	// a `render` call passes through the SAME gates and is refused too.
	//
	// ROUND-12 finding 2: round 10 then OVER-corrected, asserting the
	// fallback was a "guaranteed second refusal" for all five non-
	// no-viewport reasons, and THIS assertion pinned that literal string
	// -- making a false claim load-bearing.  It is false for exactly one
	// reason, `render_in_progress`: read_viewport reports that from
	// RunPreviewRenderParked's mAgentRenderBlocksInteractive gate, but a
	// PLAIN `render {}` (no width+height pair, no camera/view) does not
	// take that path at all -- RenderCore_ routes on
	// `wantFilmOverride || wantCameraOverrideForRouting`
	// (AgentSession.cpp), so with no override it goes to
	// SubmitAgentRenderSync, which WAITS on the fairness ticket instead of
	// refusing outright.
	//
	// ROUND-14 finding 2: round 12's replacement was ITSELF false in the
	// other direction -- it taught the plain-render fallback as one that
	// "normally runs", and this assertion pinned that.  A plain
	// `render {}` is refused on TWO further paths, both verified in tree:
	//   (a) the fairness wait TIMES OUT.  SubmitAgentRenderSync waits
	//       timeoutMs (30000, passed by RenderCore_) and on expiry reports
	//       CoordinatedRenderBusy.  SubmitProductionRenderSync forwards
	//       straight onto SubmitAgentRenderSync, so the USER'S OWN
	//       production render occupies the same single slot and routinely
	//       outlives 30 s -- the taught fallback then costs a 30-second
	//       block AND collects a refusal, which is exactly the chattiness
	//       this branch exists to remove.
	//   (b) a DIRECT PARKED render holds the gate.  RunPreviewRenderParked
	//       sets mAgentRenderBlocksInteractive but NOT mAgentRenderPending,
	//       so the fairness predicate passes IMMEDIATELY (no wait) and
	//       SubmitAgentRenderAsync_Locked's own gate check refuses --
	//       witnessed by AgentRenderAsyncTest's "coordinated submission
	//       refuses while a direct parked render owns admission".
	// Pin the corrected guidance: the warning, the two no-viewport
	// reasons, the plain-vs-override routing split, BOTH refusal paths,
	// and the action the model should actually take (retry the free
	// read_viewport, not `render`).
	Check( md.find( "Do NOT reflexively fall back to `render`" ) != std::string::npos,
	       "observe-modes explicitly warns against the reflexive render fallback" );
	Check( md.find( "the two no-viewport reasons (`no_controller`," ) != std::string::npos,
	       "observe-modes names render as the fallback for the two no-viewport reasons" );
	Check( md.find( "`render_in_progress` is the ONE reason where a `render` call does" ) != std::string::npos,
	       "observe-modes flags render_in_progress as the exception to the same-gate rule" );
	Check( md.find( "does NOT take the parked path read_viewport takes" ) != std::string::npos,
	       "observe-modes explains that a PLAIN render reaches the slot rather than the parked path" );
	Check( md.find( "film override (`width` AND `height`)" ) != std::string::npos,
	       "observe-modes states which render shapes DO take the refused parked path" );
	// Round-14 finding 2 (a): the timeout branch must be taught, not just
	// the happy path -- no surface mentioned it before this round.
	Check( md.find( "WAITS up to 30 s" ) != std::string::npos
	       && md.find( "REFUSED" ) != std::string::npos,
	       "MONEY: observe-modes states that a plain render WAITS up to 30 s and can be REFUSED" );
	Check( md.find( "USER'S OWN production render" ) != std::string::npos,
	       "MONEY: observe-modes names the user's own production render as the common 30 s-outliving occupant" );
	// Round-14 finding 2 (b): the direct-parked-render path refuses with
	// no wait at all.
	Check( md.find( "**NO wait at all**" ) != std::string::npos
	       && md.find( "**direct parked render**" ) != std::string::npos,
	       "MONEY: observe-modes states the direct-parked-render path refuses with no wait" );
	// Round-14 finding 2: the guidance must end in a concrete action, or
	// it only tells the model what NOT to do.
	Check( md.find( "**What to do instead.**" ) != std::string::npos
	       && md.find( "short retry of `read_viewport` is the cheap poll" ) != std::string::npos,
	       "MONEY: observe-modes recommends retrying the free read_viewport over the 30 s render block" );
	Check( md.find( "[16,512]" ) != std::string::npos,
	       "observe-modes states the width/height clamp range verbatim" );

	// A small two-object scene (sphere left, box right) for the
	// be-the-agent renders below.
	static const char* const kScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples 16\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n"
		"film\n{\n\twidth 64\n\theight 64\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 2 6\n\tlookat 0 0.5 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor 0.5 0.5 0.5\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_sphere\n\tcolor 0.8 0.2 0.2\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_box\n\tcolor 0.2 0.3 0.8\n}\n\n"
		"lambertian_material\n{\n\tname mat_floor\n\treflectance pnt_floor\n}\n\n"
		"lambertian_material\n{\n\tname mat_sphere\n\treflectance pnt_sphere\n}\n\n"
		"lambertian_material\n{\n\tname mat_box\n\treflectance pnt_box\n}\n\n"
		"clippedplane_geometry\n{\n\tname floor\n\tpta -4 0 -4\n\tptb 4 0 -4\n\tptc 4 0 4\n\tptd -4 0 4\n}\n\n"
		"standard_object\n{\n\tname obj_floor\n\tgeometry floor\n\tmaterial mat_floor\n}\n\n"
		"sphere_geometry\n{\n\tname sph\n\tradius 0.6\n}\n\n"
		"standard_object\n{\n\tname obj_sphere\n\tgeometry sph\n\tmaterial mat_sphere\n\tposition -1.1 0.6 0\n}\n\n"
		"box_geometry\n{\n\tname box\n\twidth 1.0\n\theight 1.0\n\tdepth 1.0\n}\n\n"
		"standard_object\n{\n\tname obj_box\n\tgeometry box\n\tmaterial mat_box\n\tposition 1.1 0.5 0\n}\n\n"
		"directional_light\n{\n\tname key\n\tpower 3.0\n\tcolor 1 1 1\n\tdirection 0.3 0.6 0.7\n}\n";

	const std::string scenePath = TempDir() + "rise_agent_observe_modes_test.RISEscene";
	{
		std::ofstream f( scenePath.c_str(), std::ios::binary );
		f << kScene;
	}

	// Row 1: "what is the user seeing right now" -- read_viewport on a
	// headless (LoadFromFile, no controller) session -> available:false,
	// reason "no_controller", exactly as the skill's row 1 states.
	{
		std::unique_ptr<AgentSession> session = AgentSession::LoadFromFile( scenePath );
		Check( session != nullptr, "row1: headless session loads" );
		if( session ) {
			AgentRpcDispatcher rpc( std::move( session ) );
			const JsonValue env = ParseLine( rpc.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"read_viewport\",\"params\":{}}" ) );
			Check( !env.find( "error" ), "row1: read_viewport is not an error on a headless session" );
			const JsonValue& res = env.get( "result" );
			Check( res.get( "available" ).asBool( true ) == false,
			       "row1: MONEY ASSERTION -- headless read_viewport reports available:false" );
			Check( res.get( "reason" ).asString() == "no_controller",
			       "row1: MONEY ASSERTION -- reason is 'no_controller' exactly as the skill states" );
		}
	}

	// Row 2: "roughly where I want it" -- render{quality:"draft",
	// width, height} -> renderMode "draft", previewWidth/Height echo
	// the request, and an over-requested samples count is honestly
	// capped rather than silently honored.
	{
		std::unique_ptr<AgentSession> session = AgentSession::LoadFromFile( scenePath );
		Check( session != nullptr, "row2: session loads" );
		if( session ) {
			AgentRpcDispatcher rpc( std::move( session ) );
			const JsonValue env = ParseLine( rpc.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"render\","
				"\"params\":{\"quality\":\"draft\",\"width\":48,\"height\":48,\"samples\":64}}" ) );
			Check( !env.find( "error" ), "row2: draft render is not an error" );
			const JsonValue& res = env.get( "result" );
			Check( res.get( "ok" ).asBool( false ), "row2: draft render ok" );
			Check( res.get( "renderMode" ).asString() == "draft",
			       "row2: MONEY ASSERTION -- renderMode reads 'draft'" );
			Check( res.get( "previewWidth" ).asNumber( 0 ) == 48 && res.get( "previewHeight" ).asNumber( 0 ) == 48,
			       "row2: previewWidth/Height echo the requested 48x48" );
			Check( res.get( "effectiveSamples" ).asNumber( -1 ) == 4,
			       "row2: MONEY ASSERTION -- a samples:64 request under draft is honestly capped to 4" );
		}
	}

	// Row 3: "which object is where" -- render{mode:"objectmap"}
	// carries a legend naming both placed objects; query_object_at on
	// the sphere's (camera-left) side never resolves to the box.
	{
		std::unique_ptr<AgentSession> session = AgentSession::LoadFromFile( scenePath );
		Check( session != nullptr, "row3: session loads" );
		if( session ) {
			AgentRpcDispatcher rpc( std::move( session ) );
			const JsonValue env = ParseLine( rpc.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"render\",\"params\":{\"mode\":\"objectmap\"}}" ) );
			Check( !env.find( "error" ), "row3: objectmap render is not an error" );
			const JsonValue& res = env.get( "result" );
			Check( res.get( "renderMode" ).asString() == "objectmap",
			       "row3: MONEY ASSERTION -- renderMode reads 'objectmap'" );
			const JsonValue& legend = res.get( "legend" );
			Check( legend.isArray() && legend.size() >= 2, "row3: legend carries at least the two placed objects" );
			bool sawSphere = false, sawBox = false;
			for( std::size_t i = 0; i < legend.size(); ++i ) {
				const std::string n = legend.at( i ).get( "name" ).asString();
				if( n == "obj_sphere" ) sawSphere = true;
				if( n == "obj_box" )    sawBox    = true;
			}
			Check( sawSphere && sawBox, "row3: MONEY ASSERTION -- legend names both obj_sphere and obj_box" );

			// The sphere sits at world x=-1.1 (camera-left in this
			// lookat-at-origin setup); a pixel on the left edge of the
			// 64-wide frame must never resolve to the right-half box.
			const JsonValue qenv = ParseLine( rpc.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"query_object_at\",\"params\":{\"x\":8,\"y\":40}}" ) );
			Check( !qenv.find( "error" ), "row3: query_object_at is not an error" );
			const JsonValue& qres = qenv.get( "result" );
			if( qres.get( "hit" ).asBool( false ) ) {
				Check( qres.get( "name" ).asString() != "obj_box",
				       "row3: MONEY ASSERTION -- a left-edge pixel never resolves to the right-half box" );
			}
		}
	}

	// Row 4: "does it actually look right" -- a production render (no
	// `quality`) reports renderMode "production" and a non-black,
	// non-washout image (materials/lighting are actually evaluated,
	// unlike draft).
	{
		std::unique_ptr<AgentSession> session = AgentSession::LoadFromFile( scenePath );
		Check( session != nullptr, "row4: session loads" );
		if( session ) {
			AgentRpcDispatcher rpc( std::move( session ) );
			const JsonValue env = ParseLine( rpc.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"render\",\"params\":{}}" ) );
			Check( !env.find( "error" ), "row4: production render is not an error" );
			const JsonValue& res = env.get( "result" );
			Check( res.get( "ok" ).asBool( false ), "row4: production render ok" );
			Check( res.get( "renderMode" ).asString() == "production",
			       "row4: MONEY ASSERTION -- renderMode reads 'production' with no quality override" );
			const double luma = 0.2126 * res.get( "meanR" ).asNumber( 0 ) +
			                     0.7152 * res.get( "meanG" ).asNumber( 0 ) +
			                     0.0722 * res.get( "meanB" ).asNumber( 0 );
			Check( luma > 0.02 && luma < 0.98,
			       "row4: production render shows something (mean luma " + std::to_string( luma ) + " in-range)" );
		}
	}

	std::remove( scenePath.c_str() );
}

//----------------------------------------------------------------------
// S3c: THE OUTPUT-QUALITY RULES.  Three guidance rules were added after
// a measured from-scratch build ("an alchemist's workbench") came back
// fast but cartoonish: it built every lathe form -- a retort, a mortar,
// three bottles -- out of stacked cylinders (14 cylinder_geometry
// against 2 sweep_geometry), and it rendered only FOUR times in
// thirty-three turns, so the prompt's relational constraints ("pestle
// resting against the rim", "the neck crosses in front of the book")
// were never once looked at.  The rules:
//
//   1. TURNED FORMS ARE A PROFILE.  A solid of revolution is authored
//      as an sdf_geometry roundcone/smin chain, never a cylinder stack.
//   2. BUILD CADENCE.  Look after each OBJECT GROUP, not once at the
//      end.
//   3. RELATIONAL CONSTRAINTS MUST BE SEEN.  Verify "resting against /
//      behind / in front of" against a rendered IMAGE, not coordinates.
//
// These are POSITIVE assertions -- each names something the skills must
// SAY.  Deliberately not written as bans on words: a previous guard in
// this repo banned a noun phrase and thereby rejected true statements.
// The rules must survive rewording of the surrounding prose, so each
// check below is anchored on a term the rule cannot be stated without.
//----------------------------------------------------------------------
static void TestOutputQualityRules( AgentRpcDispatcher& rpc )
{
	std::printf( "S3c: output-quality rules (turned-form profiles, build cadence, relational checks)...\n" );

	auto fetch = []( AgentRpcDispatcher& r, int id, const char* name ) {
		const JsonValue env = ParseLine( r.HandleLine( SkillRequest( id, name ) ) );
		return env.get( "result" ).get( "markdown" ).asString();
	};

	const std::string omr = fetch( rpc, 300, "object-modeling-recipes" );
	const std::string mwg = fetch( rpc, 301, "modeling-workflow-and-geometry" );
	const std::string obs = fetch( rpc, 302, "observe-modes" );
	Check( !omr.empty() && !mwg.empty() && !obs.empty(),
	       "S3c: the three guidance skills fetched" );

	// ---- Rule 1: turned forms are a profile, not a cylinder stack ----
	//
	// The vessel nouns are the trigger the model pattern-matches on, so
	// the rule is worthless if it does not name them.  Assert the ones
	// the failing build actually got wrong (retort, mortar, bottle) plus
	// the common rest of the family.
	static const char* const kLatheNouns[] = {
		"bottle", "jar", "flask", "retort", "vase", "cup", "bowl",
		"mortar", "candlestick", "goblet", "urn", "barrel"
	};
	for( size_t i = 0; i < sizeof( kLatheNouns ) / sizeof( kLatheNouns[0] ); ++i ) {
		Check( omr.find( kLatheNouns[i] ) != std::string::npos,
		       ( std::string( "S3c: object-modeling-recipes names \"" ) + kLatheNouns[i]
		         + "\" as a turned form" ).c_str() );
	}
	Check( omr.find( "solid of revolution" ) != std::string::npos,
	       "S3c: object-modeling-recipes states the turned-form rule in terms of a solid of revolution" );
	Check( omr.find( "stack of cylinders" ) != std::string::npos,
	       "S3c: object-modeling-recipes names the cylinder-stack anti-pattern it is replacing" );
	// The PRESCRIPTION, not just the prohibition: the actual verbs.  A
	// rule that says "do not stack cylinders" without naming what to do
	// instead is the failure mode this whole change exists to fix.
	Check( omr.find( "roundcone" ) != std::string::npos && omr.find( "smin" ) != std::string::npos,
	       "S3c: object-modeling-recipes prescribes sdf_geometry roundcone parts joined by smin" );
	// And the honest scope limit: sweep_geometry sweeps a FIXED
	// cross-section (point_width scales the x axis alone, end_scale_y is
	// linear-only), so it is the curved-tube verb, NOT the lathe verb.
	// Getting this backwards would send a model to a chunk that cannot
	// express a varying-radius revolve at all.
	Check( omr.find( "NOT the lathe verb" ) != std::string::npos,
	       "S3c: object-modeling-recipes states that sweep_geometry is not the lathe verb" );
	// Not blanket cargo-culting: cylinders stay right for constant-radius
	// parts, and the skill must say so.
	Check( omr.find( "When a cylinder IS the right answer" ) != std::string::npos,
	       "S3c: object-modeling-recipes keeps a cylinder-is-correct carve-out" );
	// The rule is reachable from the geometry-selection skill too -- a
	// model that reads only modeling-workflow-and-geometry must still
	// hit it.
	Check( mwg.find( "solid of revolution" ) != std::string::npos
	       && mwg.find( "roundcone" ) != std::string::npos,
	       "S3c: modeling-workflow-and-geometry carries the turned-form rule as well" );

	// ---- Rule 2: build cadence -- look after each object group ----
	Check( mwg.find( "OBJECT GROUP" ) != std::string::npos,
	       "S3c: modeling-workflow-and-geometry sets the build cadence per object group" );
	Check( mwg.find( "not** one look at the end" ) != std::string::npos
	       || mwg.find( "not one look at the end" ) != std::string::npos,
	       "S3c: modeling-workflow-and-geometry rules out the one-look-at-the-end build" );
	Check( obs.find( "BUILDING a scene from scratch" ) != std::string::npos,
	       "S3c: observe-modes carries a decision-table row for a from-scratch build" );

	// ---- The param-vs-placement tension, resolved in BOTH directions ----
	//
	// Cadence guidance must NOT be allowed to erode the rule that a
	// parameter edit is confirmed by its apply response.  Both halves
	// have to be present, in the same skill, or the two rules read as
	// contradictory and a model picks whichever it saw last.
	Check( mwg.find( "Do NOT render to confirm a parameter took" ) != std::string::npos,
	       "S3c: the param-confirmation rule SURVIVES in modeling-workflow-and-geometry" );
	Check( mwg.find( "are not param confirmations" ) != std::string::npos,
	       "S3c: modeling-workflow-and-geometry distinguishes placement/shape/composition FROM param confirmation" );
	// And the economies the cadence change must not weaken.
	Check( mwg.find( "Batching is where you\nsave round-trips" ) != std::string::npos
	       || mwg.find( "Batching is where you" ) != std::string::npos,
	       "S3c: the batching economy is explicitly preserved alongside the raised cadence" );

	// ---- Rule 3: relational constraints are verified against an image ----
	static const char* const kRelationalTerms[] = {
		"resting against", "behind", "in front of", "overlap"
	};
	for( size_t i = 0; i < sizeof( kRelationalTerms ) / sizeof( kRelationalTerms[0] ); ++i ) {
		Check( mwg.find( kRelationalTerms[i] ) != std::string::npos,
		       ( std::string( "S3c: modeling-workflow-and-geometry names the relational form \"" )
		         + kRelationalTerms[i] + "\"" ).c_str() );
	}
	Check( mwg.find( "Reasoning about\ncoordinates is not verification" ) != std::string::npos
	       || mwg.find( "coordinates is not verification" ) != std::string::npos,
	       "S3c: modeling-workflow-and-geometry states that coordinate reasoning is not verification" );
	Check( mwg.find( "query_object_at" ) != std::string::npos,
	       "S3c: modeling-workflow-and-geometry points at query_object_at as the cheap positional check" );
	// What to DO on failure -- a check with no recovery step is advice,
	// not a procedure.
	Check( mwg.find( "patch the position and look again" ) != std::string::npos,
	       "S3c: modeling-workflow-and-geometry says what to do when the relational check FAILS" );
	Check( obs.find( "RELATIONAL" ) != std::string::npos,
	       "S3c: observe-modes carries a decision-table row for relational constraints" );
}

//----------------------------------------------------------------------
// S3d: THE PROCEDURAL-TEXTURE TEACHING.  Measured problem: across every
// recorded from-scratch build, models bound `uniformcolor_painter` to
// every surface -- wood tables with no grain, benches with no wear --
// because the ONLY painters any skill mentioned were uniformcolor,
// checker, scalar and the image loaders, while the parser registry
// declares THIRTY-SIX painter kinds.  procedural-textures closes that;
// these assertions pin the parts a model must actually receive.
//
// Same discipline as S3c: POSITIVE assertions only (each names something
// the skill must SAY), each anchored on a term the claim cannot be
// stated without, so the prose can be rewritten without false failures.
// The two ```rise snippets are already covered by S3's derive + render
// contract; what needs pinning here is the PROSE that cannot be checked
// by rendering -- the decision map's coverage, the IScalarPainter trap,
// and the two facts that are counter-intuitive enough that a model
// getting them backwards would silently produce a wrong scene.
//----------------------------------------------------------------------
static void TestProceduralTextureTeaching( AgentRpcDispatcher& rpc )
{
	std::printf( "S3d: procedural-textures -- decision map, the IScalarPainter trap, domain rules...\n" );

	auto fetch = []( AgentRpcDispatcher& r, int id, const char* name ) {
		const JsonValue env = ParseLine( r.HandleLine( SkillRequest( id, name ) ) );
		return env.get( "result" ).get( "markdown" ).asString();
	};

	const std::string pt  = fetch( rpc, 400, "procedural-textures" );
	const std::string mat = fetch( rpc, 401, "materials-and-media-basics" );
	Check( !pt.empty() && !mat.empty(), "S3d: procedural-textures + materials-and-media-basics fetched" );

	// ---- The WHEN: the texture cues a model pattern-matches on ----
	// A decision map is worthless if the request's own noun is missing
	// from it, so assert the cue vocabulary explicitly.
	static const char* const kCues[] = {
		"wood", "stone", "marble", "concrete", "rust", "wear", "grime",
		"water", "clouds", "fabric", "leather", "weathered", "brushed"
	};
	for( size_t i = 0; i < sizeof( kCues ) / sizeof( kCues[0] ); ++i )
		Check( pt.find( kCues[i] ) != std::string::npos,
		       ( std::string( "S3d: procedural-textures names the texture cue \"" )
		         + kCues[i] + "\"" ).c_str() );

	// ---- The DECISION MAP: intent -> painter family ----
	// Every family the brief's map calls for must be REACHABLE by name;
	// a model cannot call a chunk it was never shown.
	static const char* const kFamilies[] = {
		"perlin3d_painter", "turbulence3d_painter", "worley3d_painter",
		"reactiondiffusion3d_painter", "iridescent_painter",
		"gerstnerwave_painter", "domainwarp3d_painter", "blend_painter",
		"channel_painter", "gabor3d_painter", "voronoi2d_painter",
		"expression_function2d"
	};
	for( size_t i = 0; i < sizeof( kFamilies ) / sizeof( kFamilies[0] ); ++i )
		Check( pt.find( kFamilies[i] ) != std::string::npos,
		       ( std::string( "S3d: the decision map reaches \"" )
		         + kFamilies[i] + "\"" ).c_str() );
	// And the map is a MAP -- an intent column, not just a name dump.
	Check( pt.find( "Decision map: surface intent -> painter family" ) != std::string::npos,
	       "S3d: procedural-textures carries the intent -> family decision map by name" );
	// The two worley modes are the pair that is easiest to get backwards
	// (f1 = cells, f2-f1 = boundaries) and the parser's own enum spelling
	// is `f2-f1`, not `f2_minus_f1`.
	Check( pt.find( "output f2-f1" ) != std::string::npos && pt.find( "output f1" ) != std::string::npos,
	       "S3d: both worley output modes are named, in the parser's own spelling" );

	// ---- THE FLAT-COLOUR VERDICT ----
	// The behaviour being corrected is reflexive uniformcolor_painter, so
	// the skill has to say plainly that it is wrong on a hero surface.
	Check( pt.find( "uniformcolor_painter" ) != std::string::npos,
	       "S3d: procedural-textures names uniformcolor_painter as the thing being replaced" );
	Check( pt.find( "amateur" ) != std::string::npos,
	       "S3d: MONEY -- the skill states plainly that a flat hero surface is the amateur look" );

	// ---- 2D (UV) vs 3D (solid) ----
	// The domain rule, and the non-obvious half of it: the 3D painters
	// sample the WORLD-SPACE hit point, so a moved object re-samples.
	Check( pt.find( "WORLD-SPACE intersection point" ) != std::string::npos,
	       "S3d: MONEY -- the skill states that 3D painters sample the world-space intersection point" );
	Check( pt.find( "surface UV" ) != std::string::npos,
	       "S3d: the skill states that the 2D painters sample the surface UV" );
	Check( pt.find( "CARVED OUT" ) != std::string::npos,
	       "S3d: the skill gives the reason to prefer a solid painter (carved-from-the-material)" );

	// ---- THE ISCALARPAINTER TRAP (the section that must not be lost) ----
	Check( pt.find( "THE ISCALARPAINTER TRAP" ) != std::string::npos,
	       "S3d: MONEY -- procedural-textures carries the IScalarPainter trap as its own section" );
	// (a) the reason read_schema alone cannot answer it: ONE reference
	//     category serves two managers.
	Check( pt.find( "references:[\"painter\"]" ) != std::string::npos,
	       "S3d: MONEY -- the trap explains that colour and scalar slots report the SAME reference category" );
	// (b) both slot families named, with real slot names from the
	//     descriptors (a model binds by name, not by category).
	Check( pt.find( "base_color" ) != std::string::npos && pt.find( "reflectance" ) != std::string::npos,
	       "S3d: the trap names real COLOUR slots" );
	Check( pt.find( "alphax" ) != std::string::npos && pt.find( "roughness" ) != std::string::npos
	       && pt.find( "scattering" ) != std::string::npos,
	       "S3d: the trap names real PHYSICAL-SCALAR slots" );
	// (c) the JH-uplift reason, which is WHY the two pipes exist.
	Check( pt.find( "spectral uplift" ) != std::string::npos || pt.find( "spectrally" ) != std::string::npos,
	       "S3d: the trap gives the spectral-uplift reason the two pipes exist" );
	// (d) the PRESCRIPTION.  A trap with no correct-usage recipe is the
	//     failure mode this skill exists to fix, and the recipe has to be
	//     the one the chunk language ACTUALLY expresses today.
	// Anchored on the PRESCRIPTION's own spelling (the full affine form),
	// not on the bare "scalar_painter { function2d" prefix: the sub-trap
	// paragraph below mentions that prefix too, so the loose form stayed
	// green when the prescription itself was deleted (caught in this
	// section's red-prove round).
	Check( pt.find( "scalar_painter { function2d <name> scale <s> bias <b> }" ) != std::string::npos,
	       "S3d: MONEY -- the trap prescribes scalar_painter { function2d <name> scale <s> bias <b> } for varying roughness" );
	Check( pt.find( "scalar_painter { texture" ) != std::string::npos,
	       "S3d: the trap also names the texture form (a roughness map on disk)" );
	// (e) the trap INSIDE the trap, empirically established: a 3D solid
	//     painter is ACCEPTED as a function2d source and silently
	//     evaluates constant (Painter::Evaluate synthesises a hit at the
	//     origin, which is what a 3D painter reads).  Derives clean,
	//     renders clean, wrong result -- so it must be taught.
	Check( pt.find( "spatially CONSTANT" ) != std::string::npos,
	       "S3d: MONEY -- the skill warns that a 3D painter through the function2d bridge goes CONSTANT" );

	// ---- blend_painter's mask direction ----
	// BlendPainter is colora*mask + colorb*(1-mask): mask 1 -> colora.
	// Most readers assume the opposite, and getting it backwards inverts
	// a material silently, so the skill must state the formula.
	Check( pt.find( "colora * mask + colorb * (1 - mask)" ) != std::string::npos,
	       "S3d: MONEY -- the skill states blend_painter's actual formula" );
	Check( pt.find( "mask 1 selects colora" ) != std::string::npos,
	       "S3d: MONEY -- the skill states which end of the mask selects colora" );

	// ---- The honest limit: no contrast control on the 3D noises ----
	// Measured while authoring the recipes; without it a model chases a
	// washed-out texture with more octaves forever.
	Check( pt.find( "no contrast, gain, or remap control" ) != std::string::npos,
	       "S3d: the skill states the missing-contrast-control limit" );
	Check( pt.find( "further apart" ) != std::string::npos,
	       "S3d: the skill gives the workaround (separate colora/colorb further than the target look)" );

	// ---- Consistency with materials-and-media-basics ----
	// The two skills must not diverge: the material skill is where a
	// model lands for "add a material", so it has to hand off rather than
	// leave uniformcolor_painter as the only painter it ever mentions.
	Check( mat.find( "procedural-textures" ) != std::string::npos,
	       "S3d: MONEY -- materials-and-media-basics cross-links procedural-textures" );
	Check( pt.find( "materials-and-media-basics" ) != std::string::npos,
	       "S3d: procedural-textures cross-links back to materials-and-media-basics" );
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
	std::printf( "S4: chat-loop tool table (fourteen tools, three providers) + SetSkillIndex...\n" );

	// The count below is asserted, not narrated: every provider's request
	// body must carry the SAME kToolDefs table, so a tool added to one codec
	// and not the others fails here.  (Round 20: the prose that used to run
	// alongside these assertions said "thirteen", listed the additions in
	// ordinal order, and had lost propose_patches -- three assertions of 14
	// sat five lines below it.  A narrated count next to an asserted one is
	// pure drift surface, so the narration is gone.)
	//
	// Anthropic: read_skill present with a schema.
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Anthropic );
		loop.AddUserMessage( "hello" );
		JsonValue root = ParseBody( loop.BuildRequest( "sk-test" ).body );
		const JsonValue& tools = root.get( "tools" );
		Check( tools.isArray() && tools.size() == 14, "anthropic body carries fourteen tools" );
		bool saw = false;
		for( std::size_t i = 0; i < tools.size(); ++i ) {
			if( tools.at( i ).get( "name" ).asString() != "read_skill" ) continue;
			saw = true;
			Check( tools.at( i ).get( "input_schema" ).isObject(), "read_skill has an input_schema" );
			const std::string desc = tools.at( i ).get( "description" ).asString();
			// 2026-07-28: this used to assert the description TAUGHT the
			// no-name-first index call.  Measured waste: it did, while the
			// system prompt already carried the index, so models spent a
			// round-trip fetching a list they held (gemini-3.5-flash in 9 of
			// 18 recorded sessions).  The chat description now sends them
			// straight to a name; the listing form still exists and is still
			// documented, just not as the opening move.
			Check( desc.find( "NO name first" ) == std::string::npos,
			       "read_skill description no longer opens with the list-first sequence" );
			Check( desc.find( "name" ) != std::string::npos,
			       "read_skill description still tells the model to pass a name" );
		}
		Check( saw, "anthropic tool list includes read_skill" );
	}

	// Gemini: fourteen functionDeclarations, read_skill present.
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Gemini );
		loop.AddUserMessage( "hello" );
		JsonValue root = ParseBody( loop.BuildRequest( "sk-test" ).body );
		const JsonValue& decls = root.get( "tools" ).at( 0 ).get( "functionDeclarations" );
		Check( decls.isArray() && decls.size() == 14, "gemini body carries fourteen functionDeclarations" );
		bool saw = false;
		for( std::size_t i = 0; i < decls.size(); ++i )
			if( decls.at( i ).get( "name" ).asString() == "read_skill" ) saw = true;
		Check( saw, "gemini functionDeclarations include read_skill" );
	}

	// OpenAI Responses: fourteen flat function tools, read_skill present.
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::OpenAI );
		loop.AddUserMessage( "hello" );
		JsonValue root = ParseBody( loop.BuildRequest( "sk-test" ).body );
		const JsonValue& tools = root.get( "tools" );
		Check( tools.isArray() && tools.size() == 14, "openai body carries fourteen tools" );
		bool saw = false;
		for( std::size_t i = 0; i < tools.size(); ++i ) {
			if( tools.at( i ).get( "type" ).asString() == "function" &&
			    tools.at( i ).get( "name" ).asString() == "read_skill" ) {
				saw = true;
				Check( tools.at( i ).get( "parameters" ).isObject(),
				       "openai read_skill has parameters" );
			}
		}
		Check( saw, "openai tools include read_skill" );
	}

	// SetSkillIndex: "" omits the section; a set index appears verbatim
	// in the NEXT BuildRequest's system prompt (base prompt unchanged).
	{
		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Anthropic );
		loop.AddUserMessage( "hello" );

		// Anthropic emits system as a one-block array carrying a
		// cache_control:ephemeral breakpoint (prompt caching); the prompt
		// text is the single block's "text".
		auto anthroSys = []( const JsonValue& r ) {
			return r.get( "system" ).at( 0 ).get( "text" ).asString();
		};

		loop.SetSkillIndex( "" );
		JsonValue root = ParseBody( loop.BuildRequest( "sk-test" ).body );
		Check( anthroSys( root ) == AgentChatLoop::SystemPrompt(),
		       "SetSkillIndex(\"\") sends the base system prompt unchanged (section omitted)" );
		Check( anthroSys( root ).find( "Available skills:" ) == std::string::npos,
		       "empty index -> no 'Available skills:' section" );

		const std::string index =
			"scene-skeleton-and-conventions -- Read before authoring a scene from scratch.";
		loop.SetSkillIndex( index );
		root = ParseBody( loop.BuildRequest( "sk-test" ).body );
		const std::string sys = anthroSys( root );
		Check( sys.rfind( AgentChatLoop::SystemPrompt(), 0 ) == 0,
		       "skills section is APPENDED (the base prompt is the prefix)" );
		Check( sys.find( "Available skills:\n" + index ) != std::string::npos,
		       "system prompt carries the 'Available skills:' section with the index text" );
		Check( sys.find( "call read_skill" ) != std::string::npos
		       && sys.find( "with a NAME directly" ) != std::string::npos,
		       "system prompt carries the read_skill call-to-action (by NAME)" );
		Check( sys.find( "Do NOT call it with no arguments" ) != std::string::npos,
		       "system prompt tells the model not to re-list the index it was just given" );

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
	loop.SetProvider( ChatProvider::Anthropic );
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
	TestOutputQualityRules( rpc );
	TestProceduralTextureTeaching( rpc );
	TestObserveModesTeaching( rpc );
	TestChatLoopWiring();
	TestToolRound( rpc );

	std::printf( "=== AgentSkillsTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
