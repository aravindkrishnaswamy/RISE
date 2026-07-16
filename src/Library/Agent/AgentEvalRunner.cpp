//////////////////////////////////////////////////////////////////////
//
//  AgentEvalRunner.cpp - the replay source + scenario loader + scenario
//    runner (see AgentEvalRunner.h).
//
//  NO ENVIRONMENT-VARIABLE / CREDENTIAL READS ANYWHERE IN THIS FILE --
//  the replay source is keyless by construction (BuildRequest is called
//  with an empty api key), and nothing here ever calls getenv.  The E4
//  LIVE path (RunScenarioLive / RunEvalMatrix) receives the api key as a
//  PARAMETER: RunScenarioLive takes it via AgentEvalLiveRunOptions.apiKey
//  and forwards it to BuildRequest (auth header ONLY); RunEvalMatrix
//  resolves it through an INJECTED envLookup callback (the CLI passes a
//  getenv adapter -- getenv stays in commandconsole.cpp, out of this TU).
//  The key is never logged, never written to any trajectory/result file,
//  and never handed to the matrix's `log` sink.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "AgentEvalRunner.h"

#include "AgentSession.h"
#include "AgentRpc.h"
#include "AgentDiagnostic.h"
#include "../Cst/Cst.h"   // Eval-harness slice E3: the "document"/"untouched" checkpoint kinds walk the CST directly
#include "../Version.h"   // RISE_VER_* -- the run manifest's riseBuild string
#include "../Interfaces/IJobPriv.h"                // propose-mode mock-Owner: RISE_CreateJobPriv + IJobPriv::release/LoadAsciiSceneViaCst
#include "../SceneEditor/SceneEditController.h"    // propose-mode mock-Owner: the unstarted headless controller ProposePatch stages against

#include <cctype>
#include <cmath>     // std::isfinite -- numeric param_equals tolerance
#include <cstdio>
#include <cstdlib>   // std::strtod -- numeric param_equals token parse
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

namespace RISE
{
	namespace Agent
	{
		namespace
		{
			//----------------------------------------------------------
			// Headless propose-mode mock-Owner.
			//----------------------------------------------------------

			//! Headless mock-Owner: a SceneEditController that never renders
			//! (never Start()ed) -- it exists only so an External-authority
			//! Propose session has a live controller to STAGE proposals against
			//! (StageProposal / ListProposals are pure mutex ops; no render
			//! thread is needed).  DoOneRenderPass is overridden to a no-op
			//! purely as belt-and-suspenders (it is never called when unstarted).
			class EvalMockOwnerController : public RISE::SceneEditController
			{
			public:
				explicit EvalMockOwnerController( IJobPriv& job )
					: SceneEditController( job, /*interactiveRasterizer*/nullptr ) {}
			protected:
				void DoOneRenderPass() override {}
			};

			//----------------------------------------------------------
			// Small file-IO helpers (kept local -- no other TU needs
			// them).
			//----------------------------------------------------------

			bool ReadNonBlankLines( const std::string& path, std::vector<std::string>& outLines,
			                        std::string& err )
			{
				std::ifstream f( path.c_str(), std::ios::binary );
				if( !f ) { err = "cannot open file: " + path; return false; }
				std::string line;
				while( std::getline( f, line ) ) {
					if( !line.empty() && line.back() == '\r' ) line.pop_back();   // CRLF-authored files
					bool blank = true;
					for( std::size_t i = 0; i < line.size(); ++i ) {
						if( !std::isspace( static_cast<unsigned char>( line[i] ) ) ) { blank = false; break; }
					}
					if( blank ) continue;
					outLines.push_back( line );
				}
				return true;
			}

			bool ReadWholeFile( const std::string& path, std::string& out, std::string& err )
			{
				std::ifstream f( path.c_str(), std::ios::binary );
				if( !f ) { err = "cannot open file: " + path; return false; }
				std::ostringstream ss;
				ss << f.rdbuf();
				out = ss.str();
				return true;
			}

			std::string JoinPath( const std::string& dir, const std::string& leaf )
			{
				std::filesystem::path p( dir );
				p /= leaf;
				return p.string();
			}

			//----------------------------------------------------------
			// Git-revision capture, for the run manifest (see RunEvalMatrix's
			// end-of-run write below).
			//----------------------------------------------------------

			//! The repo state a manifest needs to trace a run directory back
			//! to a commit.  `available` is false (sha stays "unknown") when
			//! git isn't on PATH, the cwd isn't a git checkout, or the popen
			//! round-trip otherwise fails -- an honest fallback, never a
			//! guess.
			struct GitRevision
			{
				std::string sha = "unknown";
				bool        dirty = false;
				bool        available = false;
			};

			//! Runs `git rev-parse HEAD` and `git status --porcelain` via
			//! popen to capture the commit + dirty-tree state.  Both command
			//! strings below are FIXED LITERALS -- no external input (scene
			//! paths, scenario ids, config contents, env vars) is ever
			//! interpolated into them, so there is no shell-injection surface
			//! here despite the popen call.  Never throws; any popen/read
			//! failure or unexpected output (missing git, non-40-hex sha,
			//! detached/shallow-clone oddities) leaves the honest
			//! "unknown"/unavailable default rather than fabricating a sha.
			GitRevision CaptureGitRevision()
			{
				GitRevision rev;

#if defined(_WIN32)
	#define RISE_EVAL_POPEN   _popen
	#define RISE_EVAL_PCLOSE  _pclose
	#define RISE_EVAL_DEVNULL "NUL"    //!< _popen shells via cmd.exe, whose null device is NUL (not /dev/null)
#else
	#define RISE_EVAL_POPEN   popen
	#define RISE_EVAL_PCLOSE  pclose
	#define RISE_EVAL_DEVNULL "/dev/null"
#endif

				// (1) HEAD sha -- accepted only if it's exactly 40 lowercase
				// hex chars (a well-formed full sha; anything else -- an
				// error message that slipped past the 2>/dev/null redirect,
				// a truncated read, etc. -- is rejected rather than stored).
				{
					FILE* p = RISE_EVAL_POPEN( "git rev-parse HEAD 2>" RISE_EVAL_DEVNULL, "r" );
					if( p ) {
						std::string out;
						char buf[256];
						while( std::fgets( buf, sizeof( buf ), p ) ) out += buf;
						RISE_EVAL_PCLOSE( p );

						while( !out.empty() &&
						       std::isspace( static_cast<unsigned char>( out.back() ) ) ) out.pop_back();

						bool validHex = out.size() == 40;
						if( validHex ) {
							for( std::size_t i = 0; i < out.size(); ++i ) {
								const char c = out[i];
								if( !( ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) ) ) { validHex = false; break; }
							}
						}
						if( validHex ) { rev.sha = out; rev.available = true; }
					}
				}

				// (2) dirty-tree check -- any non-blank porcelain line means a
				// tracked-file change is pending.  Drains the WHOLE pipe
				// (rather than stopping at the first hit) so a large status
				// output can't leave the child process blocked writing to a
				// full pipe while we've already stopped reading.
				if( rev.available ) {
					FILE* p = RISE_EVAL_POPEN( "git status --porcelain --untracked-files=no 2>" RISE_EVAL_DEVNULL, "r" );
					if( p ) {
						char buf[512];
						while( std::fgets( buf, sizeof( buf ), p ) ) {
							if( rev.dirty ) continue;
							bool blank = true;
							for( char* c = buf; *c; ++c ) {
								if( !std::isspace( static_cast<unsigned char>( *c ) ) ) { blank = false; break; }
							}
							if( !blank ) rev.dirty = true;
						}
						RISE_EVAL_PCLOSE( p );
					}
				}

#undef RISE_EVAL_POPEN
#undef RISE_EVAL_PCLOSE
#undef RISE_EVAL_DEVNULL

				return rev;
			}
		}

		//====================================================================
		// 1. AgentEvalReplaySource.
		//====================================================================
		bool AgentEvalReplaySource::NextBody( std::string& outBody )
		{
			if( mIndex >= mBodies.size() ) { outBody.clear(); return false; }
			outBody = mBodies[mIndex++];
			return true;
		}

		bool AgentEvalReplaySource::LoadFromFile( const std::string& path, AgentEvalReplaySource& out,
		                                           std::string& err )
		{
			out = AgentEvalReplaySource();

			std::vector<std::string> lines;
			if( !ReadNonBlankLines( path, lines, err ) ) return false;
			if( lines.empty() ) {
				err = "replay fixture '" + path + "' has no non-blank lines";
				return false;
			}

			JsonValue first;
			std::string perr;
			if( !JsonParse( lines[0], first, perr ) ) {
				err = "replay fixture '" + path + "' line 1 does not parse as JSON: " + perr;
				return false;
			}
			if( !first.isObject() ) {
				err = "replay fixture '" + path + "' line 1 is not a JSON object";
				return false;
			}

			// Auto-detect: a recorded E1 trajectory carries "run_type" on
			// every line; a raw fixture line does not.
			if( first.has( "run_type" ) ) {
				std::string provider;
				std::vector<std::string> bodies;
				for( std::size_t i = 0; i < lines.size(); ++i ) {
					JsonValue j;
					std::string lerr;
					if( !JsonParse( lines[i], j, lerr ) ) {
						err = "replay fixture '" + path + "' (trajectory) line " + std::to_string( i + 1 ) +
							" does not parse as JSON: " + lerr;
						return false;
					}
					if( !j.isObject() ) continue;   // tolerate a stray non-object line (never emitted by the recorder)
					const std::string runType = j.get( "run_type" ).asString();
					if( runType == "session" ) {
						// Review-round P2: the replay source models exactly ONE
						// provider's wire format, but a recorded trajectory can
						// carry MULTIPLE session records -- a mid-chat provider
						// switch (on-by-default; SetProvider resets the transcript
						// and rolls a fresh session).  Collecting every llm body
						// under the first provider would silently feed
						// gemini-shaped bodies to the anthropic codec.  Refuse
						// loudly and tell the user to replay one provider-segment
						// at a time.
						const std::string thisProvider = j.get( "provider" ).asString();
						if( provider.empty() ) {
							provider = thisProvider;
						}
						else if( thisProvider != provider ) {
							err = "replay fixture '" + path + "' (trajectory) switches provider mid-file ('" +
							      provider + "' -> '" + thisProvider + "'): a provider switch resets the "
							      "transcript and changes the wire format, so the recording must be replayed "
							      "one provider-segment at a time (split it at the session boundary)";
							return false;
						}
					}
					else if( runType == "llm" ) {
						bodies.push_back( j.get( "response_body" ).asString() );
					}
				}
				if( provider.empty() ) {
					err = "replay fixture '" + path + "' (trajectory) has no `session` record (provider unknown)";
					return false;
				}
				if( bodies.empty() ) {
					err = "replay fixture '" + path + "' (trajectory) has no `llm` records to replay";
					return false;
				}
				out.mProvider = provider;
				out.mBodies = bodies;
				return true;
			}

			// Raw fixture: each line {"provider":"...", "body":"..."}.
			std::string provider;
			std::vector<std::string> bodies;
			for( std::size_t i = 0; i < lines.size(); ++i ) {
				JsonValue j;
				std::string lerr;
				if( !JsonParse( lines[i], j, lerr ) ) {
					err = "replay fixture '" + path + "' line " + std::to_string( i + 1 ) +
						" does not parse as JSON: " + lerr;
					return false;
				}
				if( !j.isObject() || !j.has( "provider" ) || !j.get( "provider" ).isString() ||
				    !j.has( "body" ) || !j.get( "body" ).isString() ) {
					err = "replay fixture '" + path + "' line " + std::to_string( i + 1 ) +
						" must be a JSON object with string fields \"provider\" and \"body\"";
					return false;
				}
				const std::string lineProvider = j.get( "provider" ).asString();
				if( provider.empty() ) provider = lineProvider;
				else if( lineProvider != provider ) {
					err = "replay fixture '" + path + "' line " + std::to_string( i + 1 ) +
						" names provider '" + lineProvider + "' but the fixture's leading provider is '" +
						provider + "' -- a replay source is single-provider";
					return false;
				}
				bodies.push_back( j.get( "body" ).asString() );
			}
			if( provider.empty() ) {
				err = "replay fixture '" + path + "' has no fixture lines";
				return false;
			}
			out.mProvider = provider;
			out.mBodies = bodies;
			return true;
		}

		//====================================================================
		// 1b. Strict per-checkpoint field-TYPE validation (P1(c) fail-fast).
		//
		// LoadEvalScenario's existing checkpoint pass only requires each
		// checkpoint to be a JSON object; the CheckXKind functions
		// (AgentEvalRunner.cpp's checker section, below) each gate a field on
		// `cp.has(...) && cp.get(...).isX()` -- so a PRESENT-but-WRONG-TYPED
		// field (e.g. "maxToolCalls":"7") is silently treated as ABSENT and
		// the assertion is skipped, weakening the rubric while the checkpoint
		// still reports a pass.  This section mirrors the CheckXKind
		// functions' field/type expectations exactly (by inspection -- see
		// each kind's doc comment on its checker) and fails LOUDLY at LOAD
		// time on any present-but-wrong-typed RECOGNIZED field.  A field that
		// is simply ABSENT stays legal (every field below is optional per its
		// reader).  Lives here (ahead of the CheckXKind functions, which are
		// declared much later in this TU) as small, self-contained
		// validators -- deliberately NOT sharing code with CheckXKind so this
		// pass has zero risk of calling into not-yet-declared functions.
		//====================================================================
		namespace
		{
			const char* JsonTypeName( JsonValue::Type t )
			{
				switch( t ) {
					case JsonValue::Type::Null:   return "null";
					case JsonValue::Type::Bool:   return "bool";
					case JsonValue::Type::Number: return "number";
					case JsonValue::Type::String: return "string";
					case JsonValue::Type::Array:  return "array";
					case JsonValue::Type::Object: return "object";
				}
				return "<unknown>";
			}

			//! `label` is the field's DISPLAY name for the error text (a
			//! dotted path for a nested field, e.g. "queryAt.x"); `obj`/`key`
			//! are where the field is actually looked up.  Absent is always
			//! legal; only a PRESENT, wrong-typed field fails.
			bool RequireFieldType( const JsonValue& obj, const std::string& key, JsonValue::Type want,
			                       const std::string& scenarioId, std::size_t cpIndex, const std::string& label,
			                       std::string& err )
			{
				if( !obj.has( key ) ) return true;
				const JsonValue& v = obj.get( key );
				if( v.type() == want ) return true;
				err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( cpIndex ) + "].\"" + label +
				      "\" must be a " + JsonTypeName( want ) + " (got " + JsonTypeName( v.type() ) + ")";
				return false;
			}

			//! As RequireFieldType, but the field itself must be an ARRAY
			//! whose elements are all of `elemWant`.
			bool RequireArrayOfType( const JsonValue& obj, const std::string& key, JsonValue::Type elemWant,
			                         const std::string& scenarioId, std::size_t cpIndex, const std::string& label,
			                         std::string& err )
			{
				if( !obj.has( key ) ) return true;
				const JsonValue& v = obj.get( key );
				if( !v.isArray() ) {
					err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( cpIndex ) + "].\"" + label +
					      "\" must be an array (got " + JsonTypeName( v.type() ) + ")";
					return false;
				}
				for( std::size_t i = 0; i < v.size(); ++i ) {
					if( v.at( i ).type() != elemWant ) {
						err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( cpIndex ) + "].\"" + label +
						      "\"[" + std::to_string( i ) + "] must be a " + JsonTypeName( elemWant ) +
						      " (got " + JsonTypeName( v.at( i ).type() ) + ")";
						return false;
					}
				}
				return true;
			}

			//! "document" (mirrors CheckDocumentKind): "op"/"target"/"param"/
			//! "value"/"name"/"chunkKind"/"referencedKind" are strings;
			//! "numeric" is a bool; "referencedKinds" is a string array.
			//! "min"/"max" are op-DEPENDENT -- chunk_count reads them as
			//! numbers, param_range reads them as number arrays; every other
			//! op ignores them entirely, so an op-less/unknown-op checkpoint
			//! leaves them unchecked here (an unused stray field is out of
			//! this fix's scope; CheckDocumentKind still runs and grades the
			//! actual op normally).
			bool ValidateDocumentCheckpointTypes( const JsonValue& cp, std::size_t idx, const std::string& scenarioId, std::string& err )
			{
				static const char* const kStringFields[] = { "op", "target", "param", "value", "name", "chunkKind", "referencedKind" };
				for( const char* f : kStringFields )
					if( !RequireFieldType( cp, f, JsonValue::Type::String, scenarioId, idx, f, err ) ) return false;
				if( !RequireFieldType( cp, "numeric", JsonValue::Type::Bool, scenarioId, idx, "numeric", err ) ) return false;
				if( !RequireArrayOfType( cp, "referencedKinds", JsonValue::Type::String, scenarioId, idx, "referencedKinds", err ) ) return false;

				const std::string op = ( cp.has( "op" ) && cp.get( "op" ).isString() ) ? cp.get( "op" ).asString() : std::string();
				if( op == "chunk_count" ) {
					if( !RequireFieldType( cp, "min", JsonValue::Type::Number, scenarioId, idx, "min", err ) ) return false;
					if( !RequireFieldType( cp, "max", JsonValue::Type::Number, scenarioId, idx, "max", err ) ) return false;
				} else if( op == "param_range" ) {
					if( !RequireArrayOfType( cp, "min", JsonValue::Type::Number, scenarioId, idx, "min", err ) ) return false;
					if( !RequireArrayOfType( cp, "max", JsonValue::Type::Number, scenarioId, idx, "max", err ) ) return false;
				}
				return true;
			}

			//! "untouched" (mirrors CheckUntouchedKind): {chunks:[{name?:string,chunkKind?:string},...]}.
			bool ValidateUntouchedCheckpointTypes( const JsonValue& cp, std::size_t idx, const std::string& scenarioId, std::string& err )
			{
				if( !cp.has( "chunks" ) ) return true;
				const JsonValue& chunks = cp.get( "chunks" );
				if( !chunks.isArray() ) {
					err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "].\"chunks\" must be an array (got " +
					      JsonTypeName( chunks.type() ) + ")";
					return false;
				}
				for( std::size_t i = 0; i < chunks.size(); ++i ) {
					const JsonValue& c = chunks.at( i );
					if( !c.isObject() ) {
						err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "].\"chunks\"[" +
						      std::to_string( i ) + "] must be an object (got " + JsonTypeName( c.type() ) + ")";
						return false;
					}
					const std::string label = "chunks[" + std::to_string( i ) + "]";
					if( !RequireFieldType( c, "name", JsonValue::Type::String, scenarioId, idx, label + ".name", err ) ) return false;
					if( !RequireFieldType( c, "chunkKind", JsonValue::Type::String, scenarioId, idx, label + ".chunkKind", err ) ) return false;
				}
				return true;
			}

			//! "render" (mirrors CheckRenderKind): every width/height/band field is a number.
			bool ValidateRenderCheckpointTypes( const JsonValue& cp, std::size_t idx, const std::string& scenarioId, std::string& err )
			{
				static const char* const kNumFields[] = {
					"width", "height",
					"meanLumaMin", "meanLumaMax",
					"meanRMin", "meanRMax", "meanGMin", "meanGMax", "meanBMin", "meanBMax"
				};
				for( const char* f : kNumFields )
					if( !RequireFieldType( cp, f, JsonValue::Type::Number, scenarioId, idx, f, err ) ) return false;
				return true;
			}

			//! "objectmap" (mirrors CheckObjectmapKind):
			//! {legendContains?:string, pixelCountFor?:string,
			//!  pixelCountMin?/Max?:number, queryAt?:{x,y:number, expectName:string}}.
			bool ValidateObjectmapCheckpointTypes( const JsonValue& cp, std::size_t idx, const std::string& scenarioId, std::string& err )
			{
				if( !RequireFieldType( cp, "legendContains", JsonValue::Type::String, scenarioId, idx, "legendContains", err ) ) return false;
				if( !RequireFieldType( cp, "pixelCountFor", JsonValue::Type::String, scenarioId, idx, "pixelCountFor", err ) ) return false;
				if( !RequireFieldType( cp, "pixelCountMin", JsonValue::Type::Number, scenarioId, idx, "pixelCountMin", err ) ) return false;
				if( !RequireFieldType( cp, "pixelCountMax", JsonValue::Type::Number, scenarioId, idx, "pixelCountMax", err ) ) return false;
				if( cp.has( "queryAt" ) ) {
					const JsonValue& qa = cp.get( "queryAt" );
					if( !qa.isObject() ) {
						err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "].\"queryAt\" must be an object (got " +
						      JsonTypeName( qa.type() ) + ")";
						return false;
					}
					if( !RequireFieldType( qa, "x", JsonValue::Type::Number, scenarioId, idx, "queryAt.x", err ) ) return false;
					if( !RequireFieldType( qa, "y", JsonValue::Type::Number, scenarioId, idx, "queryAt.y", err ) ) return false;
					if( !RequireFieldType( qa, "expectName", JsonValue::Type::String, scenarioId, idx, "queryAt.expectName", err ) ) return false;
				}
				return true;
			}

			//! "diagnostics" (mirrors CheckDiagnosticsKind): {expect?:string, code?:string}.
			bool ValidateDiagnosticsCheckpointTypes( const JsonValue& cp, std::size_t idx, const std::string& scenarioId, std::string& err )
			{
				if( !RequireFieldType( cp, "expect", JsonValue::Type::String, scenarioId, idx, "expect", err ) ) return false;
				if( !RequireFieldType( cp, "code", JsonValue::Type::String, scenarioId, idx, "code", err ) ) return false;
				return true;
			}

			//! "trajectory" (mirrors CheckTrajectoryKind):
			//! {maxToolCalls?/maxLlmCalls?:number, terminalStatus?:string,
			//!  noAutonomyRefusal?/noMechanicalLoop?/expectAutonomyRefusal?:bool,
			//!  requiredToolInOrder?:string array,
			//!  toolOutcomes?:[{name:string, occurrence?:string, expect:string},...],
			//!  toolCallAfterUserTurn?:{name:string, minUserTurns:number}}.
			bool ValidateTrajectoryCheckpointTypes( const JsonValue& cp, std::size_t idx, const std::string& scenarioId, std::string& err )
			{
				if( !RequireFieldType( cp, "maxToolCalls", JsonValue::Type::Number, scenarioId, idx, "maxToolCalls", err ) ) return false;
				if( !RequireFieldType( cp, "maxLlmCalls", JsonValue::Type::Number, scenarioId, idx, "maxLlmCalls", err ) ) return false;
				if( !RequireFieldType( cp, "terminalStatus", JsonValue::Type::String, scenarioId, idx, "terminalStatus", err ) ) return false;
				if( !RequireFieldType( cp, "noAutonomyRefusal", JsonValue::Type::Bool, scenarioId, idx, "noAutonomyRefusal", err ) ) return false;
				if( !RequireFieldType( cp, "noMechanicalLoop", JsonValue::Type::Bool, scenarioId, idx, "noMechanicalLoop", err ) ) return false;
				if( !RequireFieldType( cp, "expectAutonomyRefusal", JsonValue::Type::Bool, scenarioId, idx, "expectAutonomyRefusal", err ) ) return false;
				if( !RequireArrayOfType( cp, "requiredToolInOrder", JsonValue::Type::String, scenarioId, idx, "requiredToolInOrder", err ) ) return false;

				if( cp.has( "toolOutcomes" ) ) {
					const JsonValue& arr = cp.get( "toolOutcomes" );
					if( !arr.isArray() ) {
						err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "].\"toolOutcomes\" must be an array (got " +
						      JsonTypeName( arr.type() ) + ")";
						return false;
					}
					// A present-but-empty array asserts nothing -- reject it loudly
					// rather than let it vacuously pass at check time.
					if( arr.size() == 0 ) {
						err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
						      "].\"toolOutcomes\" must be a NON-EMPTY array (an empty array asserts nothing)";
						return false;
					}
					for( std::size_t i = 0; i < arr.size(); ++i ) {
						const JsonValue& o = arr.at( i );
						if( !o.isObject() ) {
							err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "].\"toolOutcomes\"[" +
							      std::to_string( i ) + "] must be an object (got " + JsonTypeName( o.type() ) + ")";
							return false;
						}
						const std::string label = "toolOutcomes[" + std::to_string( i ) + "]";
						// name + expect are load-bearing -- REQUIRED (a missing name never
						// matches; a missing expect never satisfies), not "type-if-present".
						if( !o.has( "name" ) || !o.get( "name" ).isString() || o.get( "name" ).asString().empty() ) {
							err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "]." + label +
							      ".name must be a non-empty string";
							return false;
						}
						if( !o.has( "expect" ) || !o.get( "expect" ).isString() ) {
							err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "]." + label +
							      ".expect must be a string";
							return false;
						}
						const std::string expect = o.get( "expect" ).asString();
						if( expect != "applied" && expect != "staged" && expect != "rejected" && expect != "error" ) {
							err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "]." + label +
							      ".expect must be one of applied|staged|rejected|error (got '" + expect + "')";
							return false;
						}
						// occurrence is OPTIONAL (defaults to "any"); if present it must
						// name a real mode -- a typo like "frist" must NOT silently
						// downgrade to the weakest "any" check.
						if( o.has( "occurrence" ) ) {
							if( !o.get( "occurrence" ).isString() ) {
								err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "]." + label +
								      ".occurrence must be a string";
								return false;
							}
							const std::string occ = o.get( "occurrence" ).asString();
							if( occ != "first" && occ != "last" && occ != "any" ) {
								err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "]." + label +
								      ".occurrence must be one of first|last|any (got '" + occ + "')";
								return false;
							}
						}
					}
				}

				if( cp.has( "toolCallAfterUserTurn" ) ) {
					const JsonValue& tc = cp.get( "toolCallAfterUserTurn" );
					if( !tc.isObject() ) {
						err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "].\"toolCallAfterUserTurn\" must be an object (got " +
						      JsonTypeName( tc.type() ) + ")";
						return false;
					}
					// Both fields are load-bearing -- REQUIRED (a missing name never
					// matches; a missing minUserTurns silently defaults to 0).
					if( !tc.has( "name" ) || !tc.get( "name" ).isString() || tc.get( "name" ).asString().empty() ) {
						err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
						      "].toolCallAfterUserTurn.name must be a non-empty string";
						return false;
					}
					if( !tc.has( "minUserTurns" ) || !tc.get( "minUserTurns" ).isNumber() ) {
						err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
						      "].toolCallAfterUserTurn.minUserTurns must be a number";
						return false;
					}
					if( tc.get( "minUserTurns" ).asNumber() < 0.0 ) {
						err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
						      "].toolCallAfterUserTurn.minUserTurns must be >= 0";
						return false;
					}
				}

				return true;
			}

			//! "finalText" (mirrors CheckFinalTextKind):
			//! {containsAll?/containsAny?/absent?:string array, caseSensitive?:bool}.
			bool ValidateFinalTextCheckpointTypes( const JsonValue& cp, std::size_t idx, const std::string& scenarioId, std::string& err )
			{
				if( !RequireArrayOfType( cp, "containsAll", JsonValue::Type::String, scenarioId, idx, "containsAll", err ) ) return false;
				if( !RequireArrayOfType( cp, "containsAny", JsonValue::Type::String, scenarioId, idx, "containsAny", err ) ) return false;
				if( !RequireArrayOfType( cp, "absent", JsonValue::Type::String, scenarioId, idx, "absent", err ) ) return false;
				if( !RequireFieldType( cp, "caseSensitive", JsonValue::Type::Bool, scenarioId, idx, "caseSensitive", err ) ) return false;
				return true;
			}

			//! Dispatch by "kind" (only when present AND a string -- a
			//! missing/wrong-typed "kind" is caught elsewhere, at
			//! CheckOneCheckpoint's RUNTIME dispatch, as a malformed/failed
			//! checkpoint) plus the "weight" field every kind shares
			//! (CheckScenario's own reader).  An unrecognized kind name is
			//! left unchecked here too -- CheckOneCheckpoint already fails it
			//! loudly at runtime ("unknown checkpoint kind").
			bool ValidateCheckpointFieldTypes( const JsonValue& cp, std::size_t idx, const std::string& scenarioId, std::string& err )
			{
				if( !RequireFieldType( cp, "weight", JsonValue::Type::Number, scenarioId, idx, "weight", err ) ) return false;
				if( !cp.has( "kind" ) || !cp.get( "kind" ).isString() ) return true;
				const std::string kind = cp.get( "kind" ).asString();
				if( kind == "document" )    return ValidateDocumentCheckpointTypes( cp, idx, scenarioId, err );
				if( kind == "untouched" )   return ValidateUntouchedCheckpointTypes( cp, idx, scenarioId, err );
				if( kind == "render" )      return ValidateRenderCheckpointTypes( cp, idx, scenarioId, err );
				if( kind == "objectmap" )   return ValidateObjectmapCheckpointTypes( cp, idx, scenarioId, err );
				if( kind == "diagnostics" ) return ValidateDiagnosticsCheckpointTypes( cp, idx, scenarioId, err );
				if( kind == "trajectory" )  return ValidateTrajectoryCheckpointTypes( cp, idx, scenarioId, err );
				if( kind == "finalText" )   return ValidateFinalTextCheckpointTypes( cp, idx, scenarioId, err );
				return true;
			}
		}

		//====================================================================
		// 2. LoadEvalScenario.
		//====================================================================
		bool LoadEvalScenario( const std::string& path, AgentEvalScenario& out, std::string& err )
		{
			out = AgentEvalScenario();

			std::string text;
			if( !ReadWholeFile( path, text, err ) ) return false;

			JsonValue root;
			std::string perr;
			if( !JsonParse( text, root, perr ) ) {
				err = "scenario '" + path + "' does not parse as JSON: " + perr;
				return false;
			}
			if( !root.isObject() ) {
				err = "scenario '" + path + "' root must be a JSON object";
				return false;
			}

			if( !root.has( "id" ) || !root.get( "id" ).isString() || root.get( "id" ).asString().empty() ) {
				err = "scenario '" + path + "' missing required non-empty string field \"id\"";
				return false;
			}
			out.id = root.get( "id" ).asString();

			// Review-round P2: the id is concatenated into filesystem paths
			// (temp scene, trajectory, result) -- it MUST be a bare token so a
			// scenario can never write outside runDir.  Mirrors the read_skill
			// bare-name rule (AgentRpc.h: '/', '\\', ".." rejected).  The
			// scenario JSON is dev-committed today, but this closes the hole
			// before E4's live runner or any less-trusted scenario source.
			if( out.id.find( '/' ) != std::string::npos ||
			    out.id.find( '\\' ) != std::string::npos ||
			    out.id.find( ".." ) != std::string::npos ) {
				err = "scenario id '" + out.id + "' must be a BARE token -- no '/', '\\\\', or \"..\" "
				      "(it is used to build filesystem paths under the run dir)";
				return false;
			}

			if( !root.has( "title" ) || !root.get( "title" ).isString() ||
			    root.get( "title" ).asString().empty() ) {
				err = "scenario '" + out.id + "' missing required non-empty string field \"title\"";
				return false;
			}
			out.title = root.get( "title" ).asString();

			if( !root.has( "scene" ) || !root.get( "scene" ).isObject() ) {
				err = "scenario '" + out.id + "' missing required object field \"scene\"";
				return false;
			}
			{
				const JsonValue& sceneObj = root.get( "scene" );
				const bool hasPath = sceneObj.has( "path" ) && sceneObj.get( "path" ).isString() &&
					!sceneObj.get( "path" ).asString().empty();
				const bool hasInline = sceneObj.has( "inline" ) && sceneObj.get( "inline" ).isString() &&
					!sceneObj.get( "inline" ).asString().empty();
				if( hasPath == hasInline ) {
					err = "scenario '" + out.id + "': scene must specify EXACTLY ONE of non-empty \"path\" or \"inline\"";
					return false;
				}
				if( hasPath ) out.scenePath = sceneObj.get( "path" ).asString();
				else out.sceneInline = sceneObj.get( "inline" ).asString();
			}

			out.autonomy = "commit";
			if( root.has( "autonomy" ) ) {
				if( !root.get( "autonomy" ).isString() ) {
					err = "scenario '" + out.id + "': \"autonomy\" must be a string";
					return false;
				}
				out.autonomy = root.get( "autonomy" ).asString();
				if( out.autonomy != "commit" && out.autonomy != "propose" && out.autonomy != "read" ) {
					err = "scenario '" + out.id + "': \"autonomy\" must be \"commit\", \"propose\", or \"read\" (got '" +
						out.autonomy + "')";
					return false;
				}
			}

			if( !root.has( "prompts" ) || !root.get( "prompts" ).isArray() || root.get( "prompts" ).size() == 0 ) {
				err = "scenario '" + out.id + "' missing required non-empty array field \"prompts\"";
				return false;
			}
			{
				const JsonValue& prompts = root.get( "prompts" );
				for( std::size_t i = 0; i < prompts.size(); ++i ) {
					if( !prompts.at( i ).isString() ) {
						err = "scenario '" + out.id + "': prompts[" + std::to_string( i ) + "] must be a string";
						return false;
					}
					out.prompts.push_back( prompts.at( i ).asString() );
				}
			}

			out.budgets = AgentEvalBudgets();
			if( root.has( "budgets" ) ) {
				if( !root.get( "budgets" ).isObject() ) {
					err = "scenario '" + out.id + "': \"budgets\" must be an object";
					return false;
				}
				const JsonValue& b = root.get( "budgets" );
				if( b.has( "maxToolCalls" ) ) {
					if( !b.get( "maxToolCalls" ).isNumber() ) {
						err = "scenario '" + out.id + "': budgets.maxToolCalls must be a number";
						return false;
					}
					out.budgets.maxToolCalls = static_cast<int>( b.get( "maxToolCalls" ).asNumber() );
				}
				if( b.has( "maxLlmCalls" ) ) {
					if( !b.get( "maxLlmCalls" ).isNumber() ) {
						err = "scenario '" + out.id + "': budgets.maxLlmCalls must be a number";
						return false;
					}
					out.budgets.maxLlmCalls = static_cast<int>( b.get( "maxLlmCalls" ).asNumber() );
				}
				if( b.has( "maxWallMs" ) ) {
					if( !b.get( "maxWallMs" ).isNumber() ) {
						err = "scenario '" + out.id + "': budgets.maxWallMs must be a number";
						return false;
					}
					out.budgets.maxWallMs = static_cast<long long>( b.get( "maxWallMs" ).asNumber() );
				}
			}

			out.replayFixturePath.clear();
			if( root.has( "replay" ) ) {
				if( !root.get( "replay" ).isObject() ) {
					err = "scenario '" + out.id + "': \"replay\" must be an object";
					return false;
				}
				const JsonValue& rep = root.get( "replay" );
				if( rep.has( "fixture" ) ) {
					if( !rep.get( "fixture" ).isString() || rep.get( "fixture" ).asString().empty() ) {
						err = "scenario '" + out.id + "': replay.fixture must be a non-empty string";
						return false;
					}
					out.replayFixturePath = rep.get( "fixture" ).asString();
				}
			}

			out.checkpoints = JsonValue::MakeArray();
			if( root.has( "checkpoints" ) ) {
				if( !root.get( "checkpoints" ).isArray() ) {
					err = "scenario '" + out.id + "': \"checkpoints\" must be an array";
					return false;
				}
				const JsonValue& cps = root.get( "checkpoints" );
				for( std::size_t i = 0; i < cps.size(); ++i ) {
					if( !cps.at( i ).isObject() ) {
						err = "scenario '" + out.id + "': checkpoints[" + std::to_string( i ) + "] must be an object";
						return false;
					}
					// P1(c) fail-fast: a field the checker RECOGNIZES for this
					// checkpoint's kind but that carries the WRONG JSON type is
					// a load-time error, not a silently-skipped (and therefore
					// silently-weakened) assertion.
					if( !ValidateCheckpointFieldTypes( cps.at( i ), i, out.id, err ) ) return false;
				}
				out.checkpoints = cps;   // carried OPAQUELY -- E3 interprets kinds
			}

			// interventions: OPTIONAL array of scripted co-editor edits (see
			// AgentEvalIntervention).  Validated LOUDLY here so a malformed
			// intervention is a load_error, never a silently-skipped no-op.
			out.interventions.clear();
			if( root.has( "interventions" ) ) {
				if( !root.get( "interventions" ).isArray() ) {
					err = "scenario '" + out.id + "': \"interventions\" must be an array";
					return false;
				}
				const JsonValue& iv = root.get( "interventions" );
				for( std::size_t i = 0; i < iv.size(); ++i ) {
					const std::string idx = std::to_string( i );
					const JsonValue& e = iv.at( i );
					if( !e.isObject() ) {
						err = "scenario '" + out.id + "': interventions[" + idx + "] must be an object";
						return false;
					}
					AgentEvalIntervention it;
					if( !e.has( "afterToolCalls" ) || !e.get( "afterToolCalls" ).isNumber() ) {
						err = "scenario '" + out.id + "': interventions[" + idx +
							"] missing required number field \"afterToolCalls\"";
						return false;
					}
					it.afterToolCalls = static_cast<int>( e.get( "afterToolCalls" ).asNumber() );
					if( it.afterToolCalls < 1 ) {
						err = "scenario '" + out.id + "': interventions[" + idx +
							"].afterToolCalls must be >= 1 (an intervention fires AFTER a tool call; got " +
							std::to_string( it.afterToolCalls ) + ")";
						return false;
					}
					if( !e.has( "op" ) || !e.get( "op" ).isString() ) {
						err = "scenario '" + out.id + "': interventions[" + idx + "] missing string field \"op\"";
						return false;
					}
					it.op = e.get( "op" ).asString();
					if( it.op != "param_edit" ) {
						err = "scenario '" + out.id + "': interventions[" + idx + "].op '" + it.op +
							"' is not supported (only \"param_edit\" today)";
						return false;
					}
					const char* const strFields[] = { "target", "param", "value" };
					std::string* const dests[] = { &it.target, &it.param, &it.value };
					for( int f = 0; f < 3; ++f ) {
						if( !e.has( strFields[f] ) || !e.get( strFields[f] ).isString() ||
						    e.get( strFields[f] ).asString().empty() ) {
							err = "scenario '" + out.id + "': interventions[" + idx + "].op=\"param_edit\" requires a "
								"non-empty string \"" + strFields[f] + "\"";
							return false;
						}
						*dests[f] = e.get( strFields[f] ).asString();
					}
					out.interventions.push_back( it );
				}
			}

			return true;
		}

		//====================================================================
		// 3. RunScenario.
		//====================================================================
		namespace
		{
			// Mirrors commandconsole.cpp's AuthorityForAutonomy: only the
			// Propose posture pairs with an External-authority session (a
			// staged, not committed, edit surface).
			AgentAuthority AuthorityForScenarioAutonomy( const std::string& autonomy )
			{
				return ( autonomy == "propose" ) ? AgentAuthority::External : AgentAuthority::Owner;
			}

			AgentAutonomy AutonomyForScenarioString( const std::string& autonomy )
			{
				if( autonomy == "read" ) return AgentAutonomy::Read;
				if( autonomy == "propose" ) return AgentAutonomy::Propose;
				return AgentAutonomy::Commit;   // "commit", or anything else (LoadEvalScenario already rejects other values)
			}

			bool ParseReplayProviderName( const std::string& name, ChatProvider& out )
			{
				if( name == "anthropic" ) { out = ChatProvider::Anthropic; return true; }
				if( name == "gemini" )    { out = ChatProvider::Gemini;    return true; }
				if( name == "openai" )    { out = ChatProvider::OpenAI;    return true; }
				if( name == "xai" )       { out = ChatProvider::XAI;       return true; }
				if( name == "local" )     { out = ChatProvider::Local;     return true; }
				return false;
			}

			//! One round's outcome from a body source (replay OR live
			//! transport).  `proceed` false STOPS the whole scenario with
			//! `stopStatus` (and `stopError` for a transport_error).  On
			//! `proceed` true the {status, body, elapsedMs} feed
			//! RecordHttpRound + HandleResponse.  This is the ONLY thing
			//! that differs between the replay and live drive paths -- the
			//! rest of the loop (RunScenarioDriven, below) is byte-identical.
			struct FetchOutcome
			{
				bool        proceed = false;
				std::string stopStatus;   //!< terminalStatus to set when !proceed (e.g. "replay_exhausted", "transport_error")
				std::string stopError;    //!< errorMessage to set when !proceed (transport_error carries the header-free category)
				long        status   = 0;
				std::string body;
				int64_t     elapsedMs = 0;
			};

			using FetchFn = std::function<FetchOutcome( const ChatHttpRequest& )>;

			//! The SHARED scenario drive loop for both the replay
			//! (RunScenario) and live (RunScenarioLive) paths.  Everything
			//! below the body source -- scene load, session/dispatcher
			//! construction, the trajectory sink, the turn/tool loop, budget
			//! enforcement, and the trajectory + result file writes -- is
			//! identical; the two callers differ ONLY in `provider`/`apiKey`
			//! and the `fetch` callback (canned bodies vs. a real POST).
			//! Never throws.
			AgentEvalRunHandle RunScenarioDriven(
				const AgentEvalScenario&        scenario,
				const std::string&              runDir,
				const std::function<int64_t()>& optClock,
				ChatProvider                    provider,
				const std::string&              modelId,
				const std::string&              apiKey,
				const FetchFn&                  fetch )
			{
				AgentEvalRunHandle handle;
				handle.result.scenarioId = scenario.id;

				// 1. The scene: path as-is, or inline -> a throwaway temp file
				//    under <runDir>/tmp (never scenes/), deleted immediately
				//    after the load attempt.
				std::string scenePathToLoad = scenario.scenePath;
				std::string tempScenePath;
				if( scenePathToLoad.empty() ) {
					if( scenario.sceneInline.empty() ) {
						handle.result.terminalStatus = "load_error";
						handle.result.errorMessage =
							"scenario '" + scenario.id + "' has neither scene.path nor scene.inline";
						return handle;
					}
					std::error_code ec;
					const std::filesystem::path tmpDir = std::filesystem::path( runDir ) / "tmp";
					std::filesystem::create_directories( tmpDir, ec );
					tempScenePath = ( tmpDir / ( scenario.id + ".inline.RISEscene" ) ).string();
					std::ofstream f( tempScenePath.c_str(), std::ios::binary );
					if( !f ) {
						handle.result.terminalStatus = "load_error";
						handle.result.errorMessage =
							"could not write the inline scene temp file '" + tempScenePath + "'";
						return handle;
					}
					f.write( scenario.sceneInline.data(), static_cast<std::streamsize>( scenario.sceneInline.size() ) );
					f.close();
					scenePathToLoad = tempScenePath;
				}

				const AgentAuthority authority = AuthorityForScenarioAutonomy( scenario.autonomy );

				std::unique_ptr<AgentSession> session;
				if( scenario.autonomy == "propose" ) {
					// Headless propose-mode mock-Owner: an External-authority
					// session's ProposePatch STAGES against an attached controller
					// but REJECTS ("no live controller attached") when
					// mController==nullptr -- which is exactly what a plain
					// LoadFromFile session is.  So load the Job ourselves, wrap a
					// BORROWING (owns=false) session around it, and attach an
					// UNSTARTED EvalMockOwnerController (StageProposal /
					// ListProposals are pure mutex ops -- no render thread needed,
					// so no Start()).  The Job + controller are handed to the
					// handle (below) so both OUTLIVE the borrowing session; the
					// handle's member declaration order gives session -> controller
					// -> Job teardown.  On ANY failure we release the created Job
					// and leave `session` null -> the existing load_error path.
					// try/catch honors RunScenarioDriven's "never throws" contract:
					// the EvalMockOwnerController ctor constructs a std::thread (the
					// idle agent-render worker), which can throw std::system_error
					// under thread/resource exhaustion.  Any throw here degrades to a
					// clean load_error (session stays null) with no Job leak.  The
					// controller is built into a LOCAL unique_ptr BEFORE the Job is
					// handed to the handle, so an exception releases the still-local
					// Job in the catch, never a double-release.
					IJobPriv* job = nullptr;
					try {
						if( RISE_CreateJobPriv( &job ) && job && job->LoadAsciiSceneViaCst( scenePathToLoad.c_str() ) ) {
							std::unique_ptr<AgentSession> wrapped = AgentSession::WrapJob( job, authority );
							if( wrapped ) {
								std::unique_ptr<SceneEditController> ctrl( new EvalMockOwnerController( *job ) );   // may throw
								wrapped->AttachController( ctrl.get() );
								handle.ownedProposeJob = std::unique_ptr<IJobPriv, void(*)(IJobPriv*)>(
									job, +[]( IJobPriv* j ){ if( j ) j->release(); } );
								job = nullptr;   // ownership transferred to the handle
								handle.ownedProposeController = std::move( ctrl );
								session = std::move( wrapped );
							} else {
								job->release(); job = nullptr;   // WrapJob refused -- release the created Job
							}
						} else if( job ) {
							job->release(); job = nullptr;        // created, but LoadAsciiSceneViaCst failed
						}
					} catch( ... ) {
						if( job ) { job->release(); job = nullptr; }   // not yet handle-owned -> release here
						handle.ownedProposeController.reset();
						handle.ownedProposeJob.reset();                // handle-owned (if set) -> deleter releases
						session.reset();
					}
				} else {
					session = AgentSession::LoadFromFile( scenePathToLoad, authority );
				}

				// Hygiene: clean up the throwaway inline-scene temp file
				// regardless of load outcome -- the Job has already parsed its
				// content by the time the load returns.
				if( !tempScenePath.empty() ) std::remove( tempScenePath.c_str() );

				if( !session ) {
					handle.result.terminalStatus = "load_error";
					handle.result.errorMessage =
						"scene '" + scenePathToLoad + "' failed to load (not native-v7, or a derive error)";
					return handle;
				}

				const long long headVersionStart = static_cast<long long>( session->HeadVersion().revision );
				handle.result.headVersionStart = headVersionStart;
				handle.result.headVersionFinal = headVersionStart;

				// Eval-harness slice E3 (the "untouched" checkpoint seam): capture
				// the head's canonical text AS LOADED, before the first turn runs.
				handle.initialDocument = session->ReadDocument();

				std::unique_ptr<AgentRpcDispatcher> dispatcher(
					new AgentRpcDispatcher( std::move( session ), AutonomyForScenarioString( scenario.autonomy ) ) );

				// 2. The trajectory sink + the sans-IO loop.
				const std::string trajectoryPath = JoinPath( runDir, scenario.id + ".trajectory.jsonl" );

				AgentChatLoop loop;
				loop.SetProvider( provider, modelId );
				ChatTrajectoryConfig cfg;
				cfg.clock = optClock;
				cfg.scenePath = scenario.scenePath.empty() ? std::string( "<inline>" ) : scenario.scenePath;
				cfg.sceneHeadVersion = headVersionStart;
				loop.SetTrajectorySink( MakeTrajectoryFileSink( trajectoryPath ), cfg );

				const std::function<int64_t()> clock = optClock ? optClock : &TrajectoryNowMs;
				const int64_t startMs = clock();

				int llmCalls = 0;
				int toolCalls = 0;
				int nextRpcId = 1;
				bool budgetHit = false;
				std::string terminalStatus;
				std::string errorMessage;
				std::string finalText;

				// 3. Drive the turns: BuildRequest -> fetch (replay's next
				//    body OR transport.Post) -> RecordHttpRound ->
				//    HandleResponse -> dispatch any tool calls via the LIVE
				//    dispatcher -> AddToolResult -> repeat, exactly like
				//    tests/AgentChatLoopTest.cpp drives it by hand.
				for( std::size_t pi = 0; pi < scenario.prompts.size() && terminalStatus.empty(); ++pi ) {
					loop.AddUserMessage( scenario.prompts[pi] );

					bool turnDone = false;
					while( !turnDone ) {
						if( scenario.budgets.maxWallMs >= 0 && ( clock() - startMs ) > scenario.budgets.maxWallMs ) {
							terminalStatus = "budget_wall_ms"; budgetHit = true; break;
						}
						if( scenario.budgets.maxLlmCalls >= 0 && llmCalls >= scenario.budgets.maxLlmCalls ) {
							terminalStatus = "budget_llm_calls"; budgetHit = true; break;
						}

						// Build -> fetch -> record -> handle, with at most ONE
						// automatic retry per round, for EITHER of four
						// independent causes:
						//   (a) a text-only model 400-rejects multimodal
						//       content -- HandleResponse elides the images
						//       from the transcript and flags the step
						//       (retryWithoutImages); the rebuilt (now
						//       image-free) request is re-issued once.
						//   (b) the provider answers with a transient 5xx
						//       (observed: Ollama's template parser 500-ing
						//       on malformed tool-call syntax a local model
						//       emitted).  Sampling is nondeterministic, so
						//       one retry of the SAME round often yields a
						//       parseable reply; hosted providers can also
						//       500 transiently.  Never retries a 4xx other
						//       than the multimodal-400 / reasoning_effort-400
						//       cases here.  This cause is checked on
						//       `fo.status` (what the transport actually
						//       returned), not a codec-parsed field, and only
						//       bites on the LIVE path -- the replay fetch
						//       above always synthesizes status 200, so
						//       replay never hits HTTP and never retries here.
						//   (c) an OpenAI-family reasoning model (observed:
						//       gpt-5.6-terra) 400-rejects a function-tools
						//       request because its server-side default
						//       reasoning_effort conflicts with tool calling
						//       over /v1/chat/completions -- HandleResponse
						//       sets the loop's sticky
						//       "reasoning_effort":"none" override and flags
						//       the step (retryReasoningEffortNone); the
						//       rebuilt request (now carrying the override)
						//       is re-issued once.
						//   (d) a transport-class failure -- fo.proceed is
						//       false with fo.stopStatus == "transport_error"
						//       (observed live: NSURLError -1005 connection-
						//       lost / -1009 offline mid-run).  Chat-
						//       completions POSTs are stateless, so re-
						//       issuing the SAME round once is safe.  No HTTP
						//       response reached us, so there is no body to
						//       hand RecordHttpRound -- the failed attempt
						//       is simply never recorded (matches (a)-(c),
						//       which likewise only record attempts that got
						//       a response).  Live path only: the replay
						//       fetch never returns proceed==false with this
						//       stopStatus, so replay never retries here.
						// Every cause fires ONLY on attempt==1, so at most
						// one retry happens per round; a second failure of
						// any kind falls through to the normal
						// provider_error / transport_error path unchanged.
						// A retry that DOES reach RecordHttpRound is
						// recorded as an honest sibling llm record (attempt
						// 2, retryOf 1) via the existing RecordHttpRound
						// attempt/retryOf plumbing.
						ChatStepResult st;
						bool roundStopped = false;
						for( int attempt = 1; ; ++attempt ) {
							// Re-check the llm-calls budget on EVERY attempt, not
							// just the first: the outer check above only guards
							// attempt 1 of a round, but a retry (5xx / image-elide
							// / reasoning-effort-none / transport-error) issues its
							// OWN request and increments llmCalls just like a fresh
							// round -- without this, a maxLlmCalls:1 scenario could
							// still send a 2nd request via the same-round retry
							// path, silently exceeding the cap.  Stops BEFORE
							// building/issuing the retry's request (an honest stop,
							// never a request sent then discarded).
							if( scenario.budgets.maxLlmCalls >= 0 && llmCalls >= scenario.budgets.maxLlmCalls ) {
								terminalStatus = "budget_llm_calls"; budgetHit = true; roundStopped = true; break;
							}
							const ChatHttpRequest req = loop.BuildRequest( apiKey );
							if( req.url.empty() ) {
								terminalStatus = "provider_error";
								errorMessage = "BuildRequest returned an empty request mid-scenario";
								roundStopped = true;
								break;
							}

							const FetchOutcome fo = fetch( req );
							// Count EVERY POST attempted, BEFORE the proceed check.  A
							// request whose response was lost to a transport failure
							// (proceed == false, status 0) may still have reached --
							// and been billed by -- the provider, so it counts against
							// maxLlmCalls; counting it here is also what lets the inner
							// budget check above bound a transport-error RETRY (whose
							// failed attempt returns proceed=false and, if counted only
							// after a received response, would never increment the
							// counter -- so maxLlmCalls:1 could still send a 2nd POST).
							// CONSEQUENCE: llmCalls is REQUESTS SENT, which can exceed
							// the number of RECORDED llm rounds (RecordHttpRound fires
							// only on a received response) by the count of transport-
							// failed attempts -- the honest cost measure.
							++llmCalls;
							if( !fo.proceed ) {
								if( attempt == 1 && fo.stopStatus == "transport_error" )
									continue;   // one same-round retry of a transport failure
								terminalStatus = fo.stopStatus;
								errorMessage = fo.stopError;
								roundStopped = true;
								break;
							}

							loop.RecordHttpRound( fo.status, fo.body, fo.elapsedMs,
							                      attempt, attempt > 1 ? attempt - 1 : -1 );
							st = loop.HandleResponse( fo.status, fo.body );

							if( attempt == 1 && fo.status >= 500 && fo.status <= 599 )
								continue;   // one 5xx retry of this round
							if( st.kind == ChatStepResult::Kind::ProviderError &&
							    st.retryWithoutImages && attempt == 1 )
								continue;   // one image-free retry of this round
							if( st.kind == ChatStepResult::Kind::ProviderError &&
							    st.retryReasoningEffortNone && attempt == 1 )
								continue;   // one reasoning_effort:none retry of this round
							break;
						}
						if( roundStopped ) break;

						if( st.kind == ChatStepResult::Kind::ToolCalls ) {
							for( std::size_t ci = 0; ci < st.toolCalls.size(); ++ci ) {
								if( scenario.budgets.maxToolCalls >= 0 && toolCalls >= scenario.budgets.maxToolCalls ) {
									terminalStatus = "budget_tool_calls"; budgetHit = true; break;
								}
								const ChatToolCall& call = st.toolCalls[ci];
								const std::string line = loop.ToolCallToJsonRpcLine( call, nextRpcId++ );
								const std::string resp = dispatcher->HandleLine( line );
								loop.AddToolResult( call, resp );
								++toolCalls;

								// Scenario interventions: a scripted co-editor edit
								// fires AFTER this (the toolCalls-th) dispatched tool
								// call and BEFORE the next LLM round, mutating the
								// SHARED head through the live session WITHOUT
								// consuming a model turn -- so the agent's next patch,
								// built against the head it last read, genuinely
								// conflicts.  Applied via ProposePatch with NO
								// baseVersion (an unconditional Owner commit that bumps
								// the head's revision -- the SAME edit pathway the GUI
								// property panel / the checker's own inspection use);
								// recorded honestly as a `history_edit` (reason
								// "scenario_intervention").
								for( const AgentEvalIntervention& iv : scenario.interventions ) {
									if( iv.afterToolCalls != toolCalls ) continue;
									AgentSession* sess = dispatcher->Session();
									if( !sess ) continue;
									const long long beforeBytes = static_cast<long long>( sess->ReadDocument().size() );
									AgentSetPatch patch;
									patch.target = iv.target;
									patch.param  = iv.param;
									patch.value  = iv.value;
									// hasBaseVersion stays false -> UNCONDITIONAL commit.
									sess->ProposePatch( patch );
									const long long afterBytes = static_cast<long long>( sess->ReadDocument().size() );
									loop.RecordHistoryEdit( "scenario_intervention", beforeBytes, afterBytes );
								}
							}
							if( !terminalStatus.empty() ) break;   // a budget tripped mid tool-batch -- honest stop
						}
						else if( st.kind == ChatStepResult::Kind::FinalText ) {
							finalText = st.finalText;
							turnDone = true;
						}
						else {   // ProviderError
							terminalStatus = "provider_error";
							errorMessage = st.errorMessage;
							break;
						}
					}
				}

				// Wall-clock completion time of the run -- the whole turn loop
				// (LLM round-trips + tool dispatch), captured BEFORE the post-run
				// checker so it measures the agent's time, not the grader's.
				// Same clock as maxWallMs above (startMs at the top of RunScenario).
				const int64_t endMs = clock();
				if( terminalStatus.empty() ) terminalStatus = "final_text";
				loop.FinishTrajectory( terminalStatus );

				handle.result.terminalStatus = terminalStatus;
				handle.result.llmCalls = llmCalls;
				handle.result.toolCalls = toolCalls;
				handle.result.budgetHit = budgetHit;
				handle.result.wallMs = endMs - startMs;
				handle.result.finalText = finalText;
				handle.result.errorMessage = errorMessage;
				handle.result.headVersionFinal = dispatcher->Session()
					? static_cast<long long>( dispatcher->Session()->HeadVersion().revision )
					: headVersionStart;
				handle.trajectoryPath = trajectoryPath;

				// 4. The one-line result summary.
				JsonValue r = JsonValue::MakeObject();
				r.set( "scenarioId", JsonValue::MakeString( handle.result.scenarioId ) );
				r.set( "terminalStatus", JsonValue::MakeString( handle.result.terminalStatus ) );
				r.set( "llmCalls", JsonValue::MakeNumber( static_cast<double>( handle.result.llmCalls ) ) );
				r.set( "toolCalls", JsonValue::MakeNumber( static_cast<double>( handle.result.toolCalls ) ) );
				r.set( "budgetHit", JsonValue::MakeBool( handle.result.budgetHit ) );
				r.set( "wallMs", JsonValue::MakeNumber( static_cast<double>( handle.result.wallMs ) ) );
				r.set( "headVersionStart", JsonValue::MakeNumber( static_cast<double>( handle.result.headVersionStart ) ) );
				r.set( "headVersionFinal", JsonValue::MakeNumber( static_cast<double>( handle.result.headVersionFinal ) ) );
				r.set( "finalText", JsonValue::MakeString( handle.result.finalText ) );
				if( !handle.result.errorMessage.empty() )
					r.set( "errorMessage", JsonValue::MakeString( handle.result.errorMessage ) );

				const std::string resultPath = JoinPath( runDir, scenario.id + ".result.jsonl" );
				{
					std::error_code ec;
					std::filesystem::create_directories( std::filesystem::path( runDir ), ec );
					std::ofstream rf( resultPath.c_str(), std::ios::binary );
					if( rf ) rf << JsonSerialize( r ) << "\n";
				}
				handle.resultPath = resultPath;

				handle.dispatcher = std::move( dispatcher );
				return handle;
			}
		}

		// AgentEvalRunHandle's special members, DEFINED HERE (not defaulted in
		// the header) so every instantiation over its incomplete-in-a-consumer
		// unique_ptr members (SceneEditController / IJobPriv / AgentRpcDispatcher)
		// happens in THIS TU, where all three are complete -- the pimpl idiom.
		AgentEvalRunHandle::AgentEvalRunHandle() = default;
		AgentEvalRunHandle::~AgentEvalRunHandle() = default;
		AgentEvalRunHandle::AgentEvalRunHandle( AgentEvalRunHandle&& ) = default;
		// move-assignment is =delete'd in the header (see its doc) -- no def here.

		AgentEvalRunHandle RunScenario( const AgentEvalScenario& scenario, const AgentEvalRunOptions& options )
		{
			AgentEvalRunHandle handle;
			handle.result.scenarioId = scenario.id;

			if( options.runDir.empty() ) {
				handle.result.terminalStatus = "load_error";
				handle.result.errorMessage = "AgentEvalRunOptions.runDir is required";
				return handle;
			}

			// The replay source (either the caller's override, or one loaded
			// fresh from the scenario's named fixture) + its provider.
			AgentEvalReplaySource ownedSource;
			AgentEvalReplaySource* source = options.replaySourceOverride;
			if( !source ) {
				if( scenario.replayFixturePath.empty() ) {
					handle.result.terminalStatus = "load_error";
					handle.result.errorMessage = "scenario '" + scenario.id +
						"' names no replay.fixture and no replaySourceOverride was supplied";
					return handle;
				}
				std::string err;
				if( !AgentEvalReplaySource::LoadFromFile( scenario.replayFixturePath, ownedSource, err ) ) {
					handle.result.terminalStatus = "load_error";
					handle.result.errorMessage = "replay fixture load failed: " + err;
					return handle;
				}
				source = &ownedSource;
			}
			ChatProvider provider;
			if( !ParseReplayProviderName( source->Provider(), provider ) ) {
				handle.result.terminalStatus = "load_error";
				handle.result.errorMessage =
					"replay source names an unrecognized provider '" + source->Provider() + "'";
				return handle;
			}

			// The replay fetch: hand out the next canned body (status 200,
			// elapsed 0), or STOP with "replay_exhausted" when the source
			// runs dry.  Keyless -- BuildRequest is called with an empty api
			// key (see the apiKey argument below).
			const FetchFn replayFetch = [source]( const ChatHttpRequest& ) -> FetchOutcome {
				FetchOutcome fo;
				if( !source->HasNext() ) { fo.proceed = false; fo.stopStatus = "replay_exhausted"; return fo; }
				std::string body;
				source->NextBody( body );
				fo.proceed = true; fo.status = 200; fo.body = body; fo.elapsedMs = 0;
				return fo;
			};

			return RunScenarioDriven( scenario, options.runDir, options.clock,
			                          provider, /*modelId=*/std::string(),
			                          /*apiKey=*/std::string(), replayFetch );
		}

		//====================================================================
		// 3b. RunScenarioLive (Eval-harness slice E4).
		//====================================================================
		AgentEvalRunHandle RunScenarioLive( const AgentEvalScenario& scenario,
		                                    const AgentEvalLiveRunOptions& options )
		{
			AgentEvalRunHandle handle;
			handle.result.scenarioId = scenario.id;

			if( options.runDir.empty() ) {
				handle.result.terminalStatus = "load_error";
				handle.result.errorMessage = "AgentEvalLiveRunOptions.runDir is required";
				return handle;
			}
			if( !options.transport ) {
				handle.result.terminalStatus = "load_error";
				handle.result.errorMessage = "AgentEvalLiveRunOptions.transport is required (null)";
				return handle;
			}

			// The live fetch: perform the POST through the transport.  A
			// status <= 0 means NO HTTP response reached us (DNS/connect/TLS/
			// timeout, or the Linux unsupported stub) -- STOP the run with
			// "transport_error" carrying the transport's HEADER-FREE error
			// category.  A status > 0 (any of 2xx..5xx) is a real response
			// the loop/codec interprets (a 4xx becomes a provider_error
			// downstream), NOT a transport failure.
			IChatHttpTransport* transport = options.transport;
			const FetchFn liveFetch = [transport]( const ChatHttpRequest& req ) -> FetchOutcome {
				FetchOutcome fo;
				const ChatHttpResponse resp = transport->Post( req );
				if( resp.status <= 0 ) {
					fo.proceed = false;
					fo.stopStatus = "transport_error";
					fo.stopError = resp.error.empty()
						? std::string( "transport failure (no HTTP status, no error detail)" )
						: resp.error;
					return fo;
				}
				fo.proceed = true;
				fo.status = resp.status;
				fo.body = resp.body;
				fo.elapsedMs = resp.elapsedMs;
				return fo;
			};

			return RunScenarioDriven( scenario, options.runDir, options.clock,
			                          options.provider, options.modelId,
			                          options.apiKey, liveFetch );
		}

		//====================================================================
		// 3c. LoadEvalRunConfig + RunEvalMatrix (Eval-harness slice E4).
		//====================================================================
		bool LoadEvalRunConfig( const std::string& path, AgentEvalRunConfig& out, std::string& err )
		{
			out = AgentEvalRunConfig();

			std::string text;
			if( !ReadWholeFile( path, text, err ) ) return false;

			JsonValue root;
			std::string perr;
			if( !JsonParse( text, root, perr ) ) {
				err = "run config '" + path + "' does not parse as JSON: " + perr;
				return false;
			}
			if( !root.isObject() ) {
				err = "run config '" + path + "' root must be a JSON object";
				return false;
			}

			// scenarios: required non-empty array of non-empty strings (each a
			// path OR a glob -- the CLI expands globs; the loader validates
			// only the shape).
			if( !root.has( "scenarios" ) || !root.get( "scenarios" ).isArray() ||
			    root.get( "scenarios" ).size() == 0 ) {
				err = "run config '" + path + "' missing required non-empty array field \"scenarios\"";
				return false;
			}
			{
				const JsonValue& a = root.get( "scenarios" );
				for( std::size_t i = 0; i < a.size(); ++i ) {
					if( !a.at( i ).isString() || a.at( i ).asString().empty() ) {
						err = "run config '" + path + "': scenarios[" + std::to_string( i ) +
							"] must be a non-empty string";
						return false;
					}
					out.scenarios.push_back( a.at( i ).asString() );
				}
			}

			// providers: required non-empty array of {provider, keyEnvVar,
			// model?} objects.
			if( !root.has( "providers" ) || !root.get( "providers" ).isArray() ||
			    root.get( "providers" ).size() == 0 ) {
				err = "run config '" + path + "' missing required non-empty array field \"providers\"";
				return false;
			}
			{
				const JsonValue& a = root.get( "providers" );
				for( std::size_t i = 0; i < a.size(); ++i ) {
					const std::string idx = std::to_string( i );
					const JsonValue& p = a.at( i );
					if( !p.isObject() ) {
						err = "run config '" + path + "': providers[" + idx + "] must be an object";
						return false;
					}
					AgentEvalProviderConfig pc;
					if( !p.has( "provider" ) || !p.get( "provider" ).isString() ||
					    p.get( "provider" ).asString().empty() ) {
						err = "run config '" + path + "': providers[" + idx +
							"].provider must be a non-empty string";
						return false;
					}
					pc.provider = p.get( "provider" ).asString();
					ChatProvider dummy;
					if( !ParseReplayProviderName( pc.provider, dummy ) ) {
						err = "run config '" + path + "': providers[" + idx + "].provider '" + pc.provider +
							"' is not a known provider (want \"anthropic\", \"gemini\", \"openai\", \"xai\", or \"local\")";
						return false;
					}
					// keyEnvVar is OPTIONAL for the "local" provider (a keyless
					// local server emits no Authorization header) and REQUIRED
					// for every other provider (a live cloud call without a key
					// is refused loudly, never silently attempted).  When
					// PRESENT it must still be a non-empty string for every
					// provider (an empty/whitespace-blank env-var NAME is a
					// config error regardless of provider).
					const bool localProvider = ( pc.provider == "local" );
					if( p.has( "keyEnvVar" ) ) {
						if( !p.get( "keyEnvVar" ).isString() || p.get( "keyEnvVar" ).asString().empty() ) {
							err = "run config '" + path + "': providers[" + idx +
								"].keyEnvVar, when present, must be a non-empty string (the ENV VAR NAME holding the api key, never the key)";
							return false;
						}
						pc.keyEnvVar = p.get( "keyEnvVar" ).asString();
					}
					else if( !localProvider ) {
						err = "run config '" + path + "': providers[" + idx +
							"].keyEnvVar must be a non-empty string (the ENV VAR NAME holding the api key, never the key); "
							"only the \"local\" provider may omit it (keyless local server)";
						return false;
					}
					if( p.has( "model" ) ) {
						if( !p.get( "model" ).isString() ) {
							err = "run config '" + path + "': providers[" + idx + "].model must be a string";
							return false;
						}
						pc.model = p.get( "model" ).asString();
					}
					out.providers.push_back( pc );
				}
			}

			// repeats: optional number >= 1 (default 3).
			out.repeats = 3;
			if( root.has( "repeats" ) ) {
				if( !root.get( "repeats" ).isNumber() ) {
					err = "run config '" + path + "': \"repeats\" must be a number";
					return false;
				}
				const int rep = static_cast<int>( root.get( "repeats" ).asNumber() );
				if( rep < 1 ) {
					err = "run config '" + path + "': \"repeats\" must be >= 1 (got " + std::to_string( rep ) + ")";
					return false;
				}
				out.repeats = rep;
			}

			// runDir: required non-empty string.
			if( !root.has( "runDir" ) || !root.get( "runDir" ).isString() ||
			    root.get( "runDir" ).asString().empty() ) {
				err = "run config '" + path + "' missing required non-empty string field \"runDir\"";
				return false;
			}
			out.runDir = root.get( "runDir" ).asString();

			return true;
		}

		namespace
		{
			//! Map an arbitrary string (scenario id / provider / model) to a
			//! filesystem-safe token for the per-run subdirectory name.
			std::string SanitizeForPath( const std::string& s )
			{
				std::string o;
				o.reserve( s.size() );
				for( std::size_t i = 0; i < s.size(); ++i ) {
					const unsigned char u = static_cast<unsigned char>( s[i] );
					if( std::isalnum( u ) || s[i] == '-' || s[i] == '.' || s[i] == '_' ) o += s[i];
					else o += '_';
				}
				if( o.empty() ) o = "x";
				return o;
			}
		}

		AgentEvalMatrixResult RunEvalMatrix( const AgentEvalRunConfig& config,
		                                     const std::vector<AgentEvalScenario>& scenarios,
		                                     const AgentEvalMatrixOptions& options )
		{
			AgentEvalMatrixResult result;
			auto logLine = [&]( const std::string& m ) { if( options.log ) options.log( m ); };

			if( !options.transport ) {
				logLine( "RunEvalMatrix: no transport supplied -- nothing run" );
				return result;
			}
			if( !options.envLookup ) {
				logLine( "RunEvalMatrix: no envLookup supplied -- nothing run" );
				return result;
			}

			// REFUSE LOUDLY on a duplicate scenario `id`.  Two scenarios with
			// the same id resolve to the SAME per-run subdir leaf, colliding
			// their trajectory.jsonl (append -> two concatenated sessions) and
			// result.jsonl (truncate -> silent overwrite).  The CLI path also
			// canonical-dedups scenario PATHS, but two DIFFERENT files can
			// still declare the same id -- which path-dedup can't catch, so
			// this id-level guard is the belt to that suspenders.  Caught here
			// before any run executes; all counts stay 0.
			{
				std::set<std::string> seenIds;
				for( std::size_t si = 0; si < scenarios.size(); ++si ) {
					if( !seenIds.insert( scenarios[si].id ).second ) {
						result.errorMessage =
							"RunEvalMatrix: duplicate scenario id '" + scenarios[si].id +
							"' -- two loaded scenarios share this id and would collide into the "
							"same per-run output subdir (refusing to run to avoid silent "
							"trajectory-append / result-overwrite corruption)";
						logLine( result.errorMessage );
						return result;
					}
				}
			}

			const int reps = config.repeats > 0 ? config.repeats : 0;

			for( std::size_t pj = 0; pj < config.providers.size(); ++pj ) {
				const AgentEvalProviderConfig& prov = config.providers[pj];

				// Defense-in-depth: LoadEvalRunConfig enforces that only
				// "local" may omit keyEnvVar, but a matrix config built
				// programmatically (bypassing LoadEvalRunConfig) could still
				// hand RunEvalMatrix a non-"local" provider with an empty
				// keyEnvVar -- running that keyless would send an empty
				// Bearer credential ("Bearer ") to a live endpoint.  SKIP
				// loudly here too, same counters/message style as the
				// missing-key case below.
				if( prov.keyEnvVar.empty() && prov.provider != "local" ) {
					++result.providersSkipped;
					result.runsSkipped += static_cast<int>( scenarios.size() ) * reps;
					logLine( "RunEvalMatrix: SKIP provider '" + prov.provider +
						"' -- keyEnvVar is required for this provider (empty/missing keyEnvVar; "
						"only \"local\" may omit it)" );
					continue;
				}

				// The ONE place a key enters the matrix: resolve it from the
				// injected env lookup.  A provider that NAMES a keyEnvVar whose
				// value is missing/empty SKIPS the whole provider column (never
				// a crash) -- logged, never with the key.  A provider with NO
				// keyEnvVar (only "local" may omit it -- LoadEvalRunConfig
				// enforces that) is KEYLESS BY DESIGN, not missing: it is NOT
				// skipped, and BuildRequest emits no Authorization header for
				// the empty key.
				std::string key;
				if( !prov.keyEnvVar.empty() ) {
					const char* keyC = options.envLookup( prov.keyEnvVar );
					key = keyC ? std::string( keyC ) : std::string();
					if( key.empty() ) {
						++result.providersSkipped;
						result.runsSkipped += static_cast<int>( scenarios.size() ) * reps;
						logLine( "RunEvalMatrix: SKIP provider '" + prov.provider + "' -- env var " +
							prov.keyEnvVar + " is unset/empty (no key -> no live run for this provider)" );
						continue;
					}
				}

				ChatProvider providerEnum;
				if( !ParseReplayProviderName( prov.provider, providerEnum ) ) {
					++result.providersSkipped;
					result.runsSkipped += static_cast<int>( scenarios.size() ) * reps;
					logLine( "RunEvalMatrix: SKIP provider '" + prov.provider + "' -- unknown provider name" );
					continue;
				}
				++result.providersUsed;

				for( std::size_t si = 0; si < scenarios.size(); ++si ) {
					const AgentEvalScenario& scenario = scenarios[si];
					for( int rep = 1; rep <= reps; ++rep ) {
						std::string leaf = SanitizeForPath( scenario.id ) + "__" + SanitizeForPath( prov.provider );
						if( !prov.model.empty() ) leaf += "__" + SanitizeForPath( prov.model );
						leaf += "__r" + std::to_string( rep );
						const std::string runDir = ( std::filesystem::path( config.runDir ) / leaf ).string();

						// Cross-invocation idempotent-resume guard.  A run
						// whose subdir already carries a NON-EMPTY
						// <scenarioId>.result.jsonl completed in a PRIOR
						// invocation into this same runDir -- SKIP it rather
						// than re-executing, which would reopen
						// trajectory.jsonl in APPEND mode (concatenating two
						// sessions into one file) while truncate-overwriting
						// result.jsonl.  A subdir that exists but holds no
						// (or an empty) result.jsonl is a crashed/interrupted
						// run: wipe its contents first so the trajectory
						// sink's append can't concatenate onto a partial
						// file, then fall through and re-run it normally.
						const std::string resultPath =
							( std::filesystem::path( runDir ) / ( scenario.id + ".result.jsonl" ) ).string();
						{
							std::error_code ec;
							const bool resultExists = std::filesystem::exists( resultPath, ec ) && !ec;
							const std::uintmax_t resultSize =
								resultExists ? std::filesystem::file_size( resultPath, ec ) : 0;
							if( resultExists && !ec && resultSize > 0 ) {
								++result.runsAlreadyComplete;
								logLine( "RunEvalMatrix: SKIP " + leaf +
									" -- already completed -- skipping (delete the subdir to re-run)" );
								continue;
							}
							ec.clear();
							if( std::filesystem::exists( runDir, ec ) && !ec ) {
								std::filesystem::remove_all( runDir, ec );   // wipe a crashed/partial run's leftovers
							}
						}

						logLine( "RunEvalMatrix: RUN " + leaf );

						AgentEvalLiveRunOptions lo;
						lo.runDir   = runDir;
						lo.transport = options.transport;
						lo.provider = providerEnum;
						lo.modelId  = prov.model;
						lo.apiKey   = key;   // key stays in this local + BuildRequest; never logged/persisted
						lo.clock    = options.clock;

						AgentEvalRunHandle h = RunScenarioLive( scenario, lo );
						CheckScenario( h, scenario );
						++result.runsExecuted;
					}
				}
			}

			//! Emit the per-run reproducibility MANIFEST: an APPEND-ONLY
			//! provenance LOG at <config.runDir>/run.manifest.jsonl, one JSON
			//! object per line, appended once per RunEvalMatrix invocation
			//! into this runDir.  Each record captures enough of the run's
			//! conditions -- git commit, RISE build, the config that was fed
			//! in, the resolved scenario/provider set, and the run outcome
			//! counts -- that a committed run directory can be traced back to
			//! "what code, what config, what commit produced this."  It fires
			//! once the matrix reaches this point (i.e. past the fatal
			//! pre-flight refusals -- no transport, no envLookup, a duplicate
			//! scenario id -- which return early ABOVE and leave no manifest
			//! record), covering every run OUTCOME from here: a record is
			//! appended even when every provider column was key-skipped or
			//! every run was already-complete, so the log still records what
			//! WAS configured and why nothing ran.  APPEND semantics: the LAST
			//! line is the most-recent invocation; the full file is the
			//! provenance CHAIN across incremental/selective re-invocations
			//! into the same runDir (resume-to-add-a-provider, a targeted
			//! re-measurement) -- a selective run no longer erases the
			//! full-matrix record.  The usage model is SERIAL (one matrix
			//! invocation at a time, matching the resume guard's design); the
			//! append is a single unlocked write per invocation, so two TRULY
			//! concurrent matrices into one runDir could interleave records --
			//! out of scope, and still strictly safer than the prior
			//! truncate-overwrite it replaced.
			if( !config.runDir.empty() ) {
				const GitRevision git = CaptureGitRevision();

				JsonValue root = JsonValue::MakeObject();
				root.set( "schemaVersion", JsonValue::MakeNumber( 1 ) );

				const int64_t createdUtcMs = options.clock ? options.clock() : TrajectoryNowMs();
				root.set( "createdUtcMs", JsonValue::MakeNumber( static_cast<double>( createdUtcMs ) ) );

				const std::string riseBuild =
					std::to_string( RISE_VER_MAJOR_VERSION ) + "." +
					std::to_string( RISE_VER_MINOR_VERSION ) + "." +
					std::to_string( RISE_VER_REVISION_VERSION ) + " build " +
					std::to_string( RISE_VER_BUILD_VERSION );
				root.set( "riseBuild", JsonValue::MakeString( riseBuild ) );

				JsonValue gitObj = JsonValue::MakeObject();
				gitObj.set( "sha",       JsonValue::MakeString( git.sha ) );
				gitObj.set( "dirty",     JsonValue::MakeBool( git.dirty ) );
				gitObj.set( "available", JsonValue::MakeBool( git.available ) );
				root.set( "git", gitObj );

				root.set( "runDir",  JsonValue::MakeString( config.runDir ) );
				// `reps` (the clamped loop bound), not the raw config.repeats --
				// so the manifest reports the repeat count actually EXECUTED even
				// if a programmatically-built config passed repeats <= 0 (the CLI
				// path can't: LoadEvalRunConfig rejects non-positive repeats).
				root.set( "repeats", JsonValue::MakeNumber( reps ) );

				JsonValue scenarioSources = JsonValue::MakeArray();
				for( std::size_t i = 0; i < config.scenarios.size(); ++i ) {
					scenarioSources.push_back( JsonValue::MakeString( config.scenarios[i] ) );
				}
				root.set( "scenarioSources", scenarioSources );

				JsonValue scenariosArr = JsonValue::MakeArray();
				for( std::size_t i = 0; i < scenarios.size(); ++i ) {
					JsonValue o = JsonValue::MakeObject();
					o.set( "id", JsonValue::MakeString( scenarios[i].id ) );
					scenariosArr.push_back( o );
				}
				root.set( "scenarios", scenariosArr );

				// keyEnvVar is the ENV VAR NAME ONLY -- NEVER the resolved
				// key VALUE.  This builder never calls options.envLookup, so
				// there is no key material in scope here to leak even by
				// accident; only the provider config's declared NAME is
				// ever written.
				JsonValue providersArr = JsonValue::MakeArray();
				for( std::size_t i = 0; i < config.providers.size(); ++i ) {
					JsonValue o = JsonValue::MakeObject();
					o.set( "provider",  JsonValue::MakeString( config.providers[i].provider ) );
					o.set( "model",     JsonValue::MakeString( config.providers[i].model ) );
					o.set( "keyEnvVar", JsonValue::MakeString( config.providers[i].keyEnvVar ) );
					providersArr.push_back( o );
				}
				root.set( "providers", providersArr );

				JsonValue resultObj = JsonValue::MakeObject();
				resultObj.set( "runsExecuted",        JsonValue::MakeNumber( result.runsExecuted ) );
				resultObj.set( "runsSkipped",         JsonValue::MakeNumber( result.runsSkipped ) );
				resultObj.set( "runsAlreadyComplete", JsonValue::MakeNumber( result.runsAlreadyComplete ) );
				resultObj.set( "providersUsed",       JsonValue::MakeNumber( result.providersUsed ) );
				resultObj.set( "providersSkipped",    JsonValue::MakeNumber( result.providersSkipped ) );
				root.set( "result", resultObj );

				std::error_code ec;
				std::filesystem::create_directories( config.runDir, ec );
				const std::string manifestPath = ( std::filesystem::path( config.runDir ) / "run.manifest.jsonl" ).string();
				std::ofstream f( manifestPath.c_str(), std::ios::binary | std::ios::app );
				if( f ) { f << JsonSerialize( root ) << "\n"; }
				// Best-effort artifact: a write failure (permissions, disk full,
				// runDir colliding with a non-directory) never fails the matrix,
				// but log it so the missing manifest record isn't a silent mystery.
				else    { logLine( "RunEvalMatrix: WARNING -- could not append run manifest record to " + manifestPath ); }
			}

			return result;
		}

			//====================================================================
			// 4. The checker engine (Eval-harness slice E3).
			//====================================================================
			namespace
			{
				using RISE::Cst::Document;
				using RISE::Cst::NodeRef;
				using RISE::Cst::NodeKind;

				//! The pass/fail + explanation a single checkpoint-kind handler
				//! returns; CheckOneCheckpoint folds this into an
				//! AgentEvalCheckpointResult (adding kind/weight).
				struct CheckOutcome { bool passed; std::string detail; };

				//----------------------------------------------------------
				// CST helpers.  AgentSession.cpp has near-identical local
				// helpers (SerializeNode et al.) but they are `static` to
				// that TU -- duplicated here rather than exported, since
				// they are each ~5 lines and exporting them would widen
				// Cst.h's surface for a checker-only need.
				//----------------------------------------------------------

				//! Serialize a green node's bytes (leaves carry text; internal
				//! nodes are the concatenation of their kids) -- the same
				//! lossless-CST contract used throughout src/Library/Cst.
				void CheckerSerializeNode( const NodeRef& n, std::string& out )
				{
					if( !n ) return;
					if( n->kids.empty() ) out += n->text;
					else for( const auto& k : n->kids ) CheckerSerializeNode( k, out );
				}

				std::string CheckerNodeBytes( const NodeRef& n )
				{
					std::string s;
					CheckerSerializeNode( n, s );
					return s;
				}

				//! A chunk's bare `name` param value ("" if unnamed / not found).
				std::string CheckerChunkName( const NodeRef& chunk )
				{
					if( !chunk ) return std::string();
					for( const auto& kid : chunk->kids ) {
						if( kid->kind != NodeKind::Param ) continue;
						std::string pname;
						std::vector<std::string> values;
						for( const auto& tk : kid->kids ) {
							if( tk->kind != NodeKind::Token ) continue;
							if( tk->role == "pname" ) pname = tk->text;
							else if( tk->role == "pvalue" ) values.push_back( tk->text );
						}
						if( pname == "name" && !values.empty() ) return values[0];
					}
					return std::string();
				}

				//! A chunk's `paramName` value, space-joined across every
				//! pvalue token (e.g. a vec3 "0.9 0.1 0.1").  Returns false
				//! when the chunk carries no such param at all.
				bool CheckerChunkParamValue( const NodeRef& chunk, const std::string& paramName, std::string& outValue )
				{
					if( !chunk ) return false;
					for( const auto& kid : chunk->kids ) {
						if( kid->kind != NodeKind::Param ) continue;
						std::string pname;
						std::vector<std::string> values;
						for( const auto& tk : kid->kids ) {
							if( tk->kind != NodeKind::Token ) continue;
							if( tk->role == "pname" ) pname = tk->text;
							else if( tk->role == "pvalue" ) values.push_back( tk->text );
						}
						if( pname != paramName ) continue;
						std::string joined;
						for( std::size_t i = 0; i < values.size(); ++i ) { if( i ) joined += ' '; joined += values[i]; }
						outValue = joined;
						return true;
					}
					return false;
				}

				//! Tokenize a space-separated string into finite doubles.
				//! Returns false the moment ANY token fails to parse as a
				//! complete number (trailing junk like "1abc" is rejected)
				//! or is non-finite -- so the numeric param_equals refuses
				//! LOUDLY on a non-numeric value rather than silently
				//! treating an unparseable token as 0.
				bool CheckerParseFloatTokens( const std::string& s, std::vector<double>& out )
				{
					out.clear();
					std::istringstream iss( s );
					std::string tok;
					while( iss >> tok ) {
						const char* begin = tok.c_str();
						char* end = nullptr;
						const double v = std::strtod( begin, &end );
						if( end != begin + tok.size() ) return false;   // non-numeric / trailing junk
						if( !std::isfinite( v ) ) return false;
						out.push_back( v );
					}
					return true;
				}

				//! Find a top-level chunk by optional kind (exact keyword
				//! match; "" = no narrowing) + bare name.  Returns 0 (not
				//! found), 1 (found -- fills outChunk), or -1 (AMBIGUOUS:
				//! more than one chunk matches -- a caller must refuse rather
				//! than guess which one the scenario author meant).
				int CheckerFindChunk( const Document& doc, const std::string& kindOrEmpty, const std::string& name,
				                      NodeRef& outChunk )
				{
					const int n = RISE::Cst::DocItemCount( doc );
					int matches = 0;
					for( int i = 0; i < n; ++i ) {
						const RISE::Cst::NodeId id = RISE::Cst::DocNodeIdAt( doc, i );
						if( !id ) continue;
						NodeRef item = RISE::Cst::DocResolveNodeId( doc, id );
						if( !item || item->kind != NodeKind::Chunk ) continue;
						if( !kindOrEmpty.empty() && item->role != kindOrEmpty ) continue;
						if( CheckerChunkName( item ) != name ) continue;
						++matches;
						outChunk = item;
					}
					if( matches == 0 ) return 0;
					if( matches > 1 ) return -1;
					return 1;
				}

				//! Count top-level chunks whose keyword is EXACTLY `kind` (no
				//! narrowing shorthand -- unlike CheckerFindChunk this has no
				//! notion of "no narrowing", since a count over EVERY chunk
				//! regardless of kind is not a meaningful scenario assertion).
				//! Backs the "chunk_count" document op, which exists because
				//! CheckerFindChunk's name-based lookup is unusable for the
				//! common case of asserting on an UNNAMED, non-singleton chunk
				//! kind (e.g. "how many `timeline` chunks are there now" --
				//! every `timeline` chunk shares the empty bare name, so
				//! CheckerFindChunk( doc, "timeline", "", ... ) is AMBIGUOUS
				//! the moment a document carries more than one).
				int CheckerCountChunksOfKind( const Document& doc, const std::string& kind )
				{
					const int n = RISE::Cst::DocItemCount( doc );
					int count = 0;
					for( int i = 0; i < n; ++i ) {
						const RISE::Cst::NodeId id = RISE::Cst::DocNodeIdAt( doc, i );
						if( !id ) continue;
						NodeRef item = RISE::Cst::DocResolveNodeId( doc, id );
						if( !item || item->kind != NodeKind::Chunk ) continue;
						if( item->role != kind ) continue;
						++count;
					}
					return count;
				}

				//! The OPTIONAL chunk-kind NARROWING field of a document/untouched
				//! checkpoint (exact chunk-keyword match; "" = no narrowing).  It is
				//! deliberately NOT named "kind": that name is already the top-level
				//! checkpoint DISCRIMINATOR ("document"/"untouched"/...), and the JSON
				//! codec is last-wins on duplicate keys, so a checkpoint carrying both
				//! a "kind" discriminator and a "kind" narrowing would read the
				//! narrowing value as the discriminator (dispatch then fails with
				//! "unknown checkpoint kind").  The narrowing field is "chunkKind".
				std::string OptKind( const JsonValue& cp )
				{
					return ( cp.has( "chunkKind" ) && cp.get( "chunkKind" ).isString() ) ? cp.get( "chunkKind" ).asString() : std::string();
				}

				//----------------------------------------------------------
				// Per-checkpoint-kind handlers.
				//----------------------------------------------------------

				//! "document": {op:"param_equals",target,param,value,chunkKind?,numeric?} |
				//! {op:"param_range",target,param,min:[...],max:[...],chunkKind?} |
				//! {op:"chunk_exists",chunkKind?,name} | {op:"chunk_absent",chunkKind?,name} |
				//! {op:"chunk_count",chunkKind,min?,max?} |
				//! {op:"param_references_kind",target,param,referencedKind|referencedKinds:[...],chunkKind?} --
				//! the last passes iff target's param value NAMES an existing chunk
				//! of referencedKind (or ANY kind in referencedKinds) (grades a
				//! correctly-typed binding name-agnostically); param_equals' optional
				//! numeric:true compares both values as float tokens within 1e-4;
				//! param_range grades a value COMPONENT-WISE against per-component
				//! [min_i,max_i] bands (a tolerant "any reasonable blue" grade that
				//! is not overfit to one fixture's exact numbers).  All asserted against the
				//! POST-run document (session->ReadDocument(), reparsed via
				//! ParseToCst -- the same idiom AgentChunkCrudTest/
				//! CstFirstSliceTest use to inspect a document's chunks after
				//! an edit).  chunk_count exists for UNNAMED, non-singleton
				//! chunk kinds (e.g. `timeline`) that param_equals/chunk_exists
				//! cannot address individually -- see CheckerCountChunksOfKind's
				//! doc.  At least one of min/max is required (both is a band).
				CheckOutcome CheckDocumentKind( const JsonValue& cp, AgentSession* session )
				{
					if( !session ) return { false, "document checkpoint: no live session (run did not complete)" };
					if( !cp.has( "op" ) || !cp.get( "op" ).isString() )
						return { false, "document checkpoint missing string field \"op\"" };
					const std::string op = cp.get( "op" ).asString();
					const Document doc = RISE::Cst::ParseToCst( session->ReadDocument() );

					if( op == "param_equals" ) {
						if( !cp.has( "target" ) || !cp.get( "target" ).isString() )
							return { false, "param_equals missing string \"target\"" };
						if( !cp.has( "param" ) || !cp.get( "param" ).isString() )
							return { false, "param_equals missing string \"param\"" };
						if( !cp.has( "value" ) || !cp.get( "value" ).isString() )
							return { false, "param_equals missing string \"value\"" };
						const std::string target = cp.get( "target" ).asString();
						const std::string param  = cp.get( "param" ).asString();
						const std::string expected = cp.get( "value" ).asString();
						const std::string kind = OptKind( cp );

						NodeRef chunk;
						const int found = CheckerFindChunk( doc, kind, target, chunk );
						if( found == 0 )
							return { false, "param_equals: no chunk named '" + target + "' found" +
								( kind.empty() ? std::string() : ( " (kind '" + kind + "')" ) ) };
						if( found < 0 )
							return { false, "param_equals: AMBIGUOUS -- more than one chunk named '" + target +
								"' -- narrow with \"kind\"" };
						std::string actual;
						if( !CheckerChunkParamValue( chunk, param, actual ) )
							return { false, "param_equals: chunk '" + target + "' has no param '" + param + "'" };

						// Optional numeric tolerance: tokenize BOTH sides as
						// floats and compare within 1e-4 per component, so a
						// live model's "0.0 0.0 1.0" grades equal to a
						// fixture's "0 0 1".  Refuses LOUDLY (not a silent
						// mismatch) when the token counts differ or a token on
						// either side is non-numeric -- a "numeric" checkpoint
						// authored against a non-numeric param is a rubric bug
						// worth surfacing.
						const bool numeric = cp.has( "numeric" ) && cp.get( "numeric" ).isBool() && cp.get( "numeric" ).asBool();
						if( numeric ) {
							std::vector<double> av, ev;
							if( !CheckerParseFloatTokens( expected, ev ) )
								return { false, "param_equals(numeric): the checkpoint's \"value\" '" + expected +
									"' is not all-numeric" };
							if( !CheckerParseFloatTokens( actual, av ) )
								return { false, "param_equals(numeric): '" + target + "." + param + "' == '" + actual +
									"' is not all-numeric (expected numeric '" + expected + "')" };
							if( av.size() != ev.size() )
								return { false, "param_equals(numeric): '" + target + "." + param + "' has " +
									std::to_string( av.size() ) + " component(s), expected " + std::to_string( ev.size() ) +
									" ('" + actual + "' vs '" + expected + "')" };
							for( std::size_t i = 0; i < av.size(); ++i ) {
								if( std::fabs( av[i] - ev[i] ) > 1e-4 )
									return { false, "param_equals(numeric): '" + target + "." + param + "' == '" + actual +
										"', expected '" + expected + "' (component " + std::to_string( i ) + " differs)" };
							}
							return { true, "param_equals(numeric): '" + target + "." + param + "' == '" + expected +
								"' (within 1e-4)" };
						}

						if( actual != expected )
							return { false, "param_equals: '" + target + "." + param + "' == '" + actual +
								"', expected '" + expected + "'" };
						return { true, "param_equals: '" + target + "." + param + "' == '" + expected + "'" };
					}

					if( op == "chunk_exists" || op == "chunk_absent" ) {
						if( !cp.has( "name" ) || !cp.get( "name" ).isString() )
							return { false, op + " missing string \"name\"" };
						const std::string name = cp.get( "name" ).asString();
						const std::string kind = OptKind( cp );
						NodeRef chunk;
						const int found = CheckerFindChunk( doc, kind, name, chunk );
						if( found < 0 )
							return { false, op + ": AMBIGUOUS -- more than one chunk named '" + name +
								"' -- narrow with \"kind\"" };
						const bool exists = ( found == 1 );
						if( op == "chunk_exists" )
							return { exists, exists ? ( "chunk_exists: '" + name + "' found" )
							                          : ( "chunk_exists: '" + name + "' NOT found" ) };
						return { !exists, !exists ? ( "chunk_absent: '" + name + "' correctly absent" )
						                            : ( "chunk_absent: '" + name + "' unexpectedly PRESENT" ) };
					}

					if( op == "chunk_count" ) {
						if( !cp.has( "chunkKind" ) || !cp.get( "chunkKind" ).isString() || cp.get( "chunkKind" ).asString().empty() )
							return { false, "chunk_count missing non-empty string \"chunkKind\"" };
						if( !cp.has( "min" ) && !cp.has( "max" ) )
							return { false, "chunk_count requires at least one of \"min\"/\"max\"" };
						const std::string kind = cp.get( "chunkKind" ).asString();
						const int count = CheckerCountChunksOfKind( doc, kind );

						std::vector<std::string> failures;
						if( cp.has( "min" ) && cp.get( "min" ).isNumber() ) {
							const double lo = cp.get( "min" ).asNumber();
							if( count < lo )
								failures.push_back( "count " + std::to_string( count ) + " < min " + std::to_string( lo ) );
						}
						if( cp.has( "max" ) && cp.get( "max" ).isNumber() ) {
							const double hi = cp.get( "max" ).asNumber();
							if( count > hi )
								failures.push_back( "count " + std::to_string( count ) + " > max " + std::to_string( hi ) );
						}
						if( !failures.empty() ) {
							std::string detail = "chunk_count '" + kind + "' violated: ";
							for( std::size_t i = 0; i < failures.size(); ++i ) { if( i ) detail += "; "; detail += failures[i]; }
							return { false, detail };
						}
						return { true, "chunk_count: " + std::to_string( count ) + " chunk(s) of kind '" + kind + "' satisfies band" };
					}

					// param_range: pass iff TARGET's PARAM value, tokenized as
					// floats, has EXACTLY as many components as the min/max arrays
					// and each component_i is within [min_i, max_i].  Grades a
					// value-with-tolerance ("any reasonable blue") rather than one
					// fixture's exact numbers -- the fix for a value-level overfit
					// checkpoint.  Loud shape refusals: missing/non-array min or
					// max, a non-numeric array element, a min/max length mismatch,
					// an empty band, a component-count mismatch, or a non-numeric
					// param value.
					if( op == "param_range" ) {
						if( !cp.has( "target" ) || !cp.get( "target" ).isString() )
							return { false, "param_range missing string \"target\"" };
						if( !cp.has( "param" ) || !cp.get( "param" ).isString() )
							return { false, "param_range missing string \"param\"" };
						if( !cp.has( "min" ) || !cp.get( "min" ).isArray() )
							return { false, "param_range missing array \"min\"" };
						if( !cp.has( "max" ) || !cp.get( "max" ).isArray() )
							return { false, "param_range missing array \"max\"" };
						const std::string target = cp.get( "target" ).asString();
						const std::string param  = cp.get( "param" ).asString();
						const std::string kind   = OptKind( cp );

						const JsonValue& minArr = cp.get( "min" );
						const JsonValue& maxArr = cp.get( "max" );
						std::vector<double> mins, maxs;
						for( std::size_t i = 0; i < minArr.size(); ++i ) {
							if( !minArr.at( i ).isNumber() )
								return { false, "param_range: \"min\"[" + std::to_string( i ) + "] is not a number" };
							mins.push_back( minArr.at( i ).asNumber() );
						}
						for( std::size_t i = 0; i < maxArr.size(); ++i ) {
							if( !maxArr.at( i ).isNumber() )
								return { false, "param_range: \"max\"[" + std::to_string( i ) + "] is not a number" };
							maxs.push_back( maxArr.at( i ).asNumber() );
						}
						if( mins.empty() )
							return { false, "param_range: \"min\"/\"max\" must be non-empty arrays" };
						if( mins.size() != maxs.size() )
							return { false, "param_range: \"min\" has " + std::to_string( mins.size() ) +
								" component(s) but \"max\" has " + std::to_string( maxs.size() ) + " -- they must match" };

						NodeRef chunk;
						const int found = CheckerFindChunk( doc, kind, target, chunk );
						if( found == 0 )
							return { false, "param_range: no chunk named '" + target + "' found" +
								( kind.empty() ? std::string() : ( " (kind '" + kind + "')" ) ) };
						if( found < 0 )
							return { false, "param_range: AMBIGUOUS -- more than one chunk named '" + target +
								"' -- narrow with \"chunkKind\"" };
						std::string actual;
						if( !CheckerChunkParamValue( chunk, param, actual ) )
							return { false, "param_range: chunk '" + target + "' has no param '" + param + "'" };

						std::vector<double> av;
						if( !CheckerParseFloatTokens( actual, av ) )
							return { false, "param_range: '" + target + "." + param + "' == '" + actual +
								"' is not all-numeric" };
						if( av.size() != mins.size() )
							return { false, "param_range: '" + target + "." + param + "' has " +
								std::to_string( av.size() ) + " component(s), expected " + std::to_string( mins.size() ) +
								" ('" + actual + "')" };
						for( std::size_t i = 0; i < av.size(); ++i ) {
							if( av[i] < mins[i] || av[i] > maxs[i] )
								return { false, "param_range: '" + target + "." + param + "' == '" + actual +
									"', component " + std::to_string( i ) + " (" + std::to_string( av[i] ) +
									") outside [" + std::to_string( mins[i] ) + ", " + std::to_string( maxs[i] ) + "]" };
						}
						return { true, "param_range: '" + target + "." + param + "' == '" + actual +
							"' within all " + std::to_string( av.size() ) + " band(s)" };
					}

					// param_references_kind: pass iff TARGET's PARAM value
					// NAMES an existing chunk of REFERENCEDKIND (or of ANY kind
					// in REFERENCEDKINDS -- the any-of array form).  Grades the
					// SPACE of correct solutions name-agnostically: any
					// correctly-typed, correctly-named binding passes,
					// regardless of what NAME a live model chose for the new
					// chunk (fixture-overfit fix -- a fixture named a material
					// "mat_metal"; a live model may pick any name).  The array
					// form additionally grades KIND-agnostically across a set of
					// equivalent kinds (e.g. any of RISE's metallic-capable
					// material kinds satisfies "shiny metallic"), so the rubric
					// is not overfit to ONE fixture's chosen material kind.  The
					// optional "chunkKind" narrows the TARGET lookup (as in
					// param_equals); provide EXACTLY ONE of "referencedKind"
					// (a string) or "referencedKinds" (a non-empty string array).
					if( op == "param_references_kind" ) {
						if( !cp.has( "target" ) || !cp.get( "target" ).isString() )
							return { false, "param_references_kind missing string \"target\"" };
						if( !cp.has( "param" ) || !cp.get( "param" ).isString() )
							return { false, "param_references_kind missing string \"param\"" };

						// Collect the accepted kinds from EITHER the singular
						// "referencedKind" (a string) OR the "referencedKinds"
						// any-of array -- exactly one must be supplied.
						std::vector<std::string> acceptedKinds;
						const bool hasSingular = cp.has( "referencedKind" );
						const bool hasArray    = cp.has( "referencedKinds" );
						if( hasSingular == hasArray )
							return { false, "param_references_kind requires EXACTLY ONE of "
								"\"referencedKind\" (string) or \"referencedKinds\" (array)" };
						if( hasSingular ) {
							if( !cp.get( "referencedKind" ).isString() || cp.get( "referencedKind" ).asString().empty() )
								return { false, "param_references_kind: \"referencedKind\" must be a non-empty string" };
							acceptedKinds.push_back( cp.get( "referencedKind" ).asString() );
						} else {
							const JsonValue& arr = cp.get( "referencedKinds" );
							if( !arr.isArray() || arr.size() == 0 )
								return { false, "param_references_kind: \"referencedKinds\" must be a non-empty array" };
							for( std::size_t i = 0; i < arr.size(); ++i ) {
								if( !arr.at( i ).isString() || arr.at( i ).asString().empty() )
									return { false, "param_references_kind: \"referencedKinds\"[" + std::to_string( i ) +
										"] must be a non-empty string" };
								acceptedKinds.push_back( arr.at( i ).asString() );
							}
						}

						const std::string target = cp.get( "target" ).asString();
						const std::string param  = cp.get( "param" ).asString();
						const std::string kind = OptKind( cp );   // narrows the TARGET chunk

						NodeRef chunk;
						const int found = CheckerFindChunk( doc, kind, target, chunk );
						if( found == 0 )
							return { false, "param_references_kind: no chunk named '" + target + "' found" +
								( kind.empty() ? std::string() : ( " (kind '" + kind + "')" ) ) };
						if( found < 0 )
							return { false, "param_references_kind: AMBIGUOUS -- more than one chunk named '" +
								target + "' -- narrow with \"chunkKind\"" };
						std::string referencedName;
						if( !CheckerChunkParamValue( chunk, param, referencedName ) )
							return { false, "param_references_kind: chunk '" + target + "' has no param '" + param + "'" };
						if( referencedName.empty() )
							return { false, "param_references_kind: '" + target + "." + param + "' has an empty value" };

						// ANY-OF: the binding passes as soon as the named chunk
						// resolves under one accepted kind.  An AMBIGUOUS match
						// under ANY single kind is a hard rubric failure (the
						// referenced name is not uniquely a chunk of that kind).
						std::string joinedKinds;
						for( std::size_t i = 0; i < acceptedKinds.size(); ++i ) {
							if( i ) joinedKinds += ", ";
							joinedKinds += "'" + acceptedKinds[i] + "'";
						}
						for( const std::string& refKind : acceptedKinds ) {
							NodeRef referenced;
							const int refFound = CheckerFindChunk( doc, refKind, referencedName, referenced );
							if( refFound < 0 )
								return { false, "param_references_kind: '" + referencedName +
									"' matches MORE THAN ONE chunk of kind '" + refKind + "'" };
							if( refFound == 1 )
								return { true, "param_references_kind: '" + target + "." + param + "' -> '" +
									referencedName + "' (a '" + refKind + "')" };
						}
						return { false, "param_references_kind: '" + target + "." + param + "' names '" +
							referencedName + "', but no chunk of kind(s) " + joinedKinds + " has that name" };
					}

					return { false, "document checkpoint: unknown op '" + op + "'" };
				}

				//! "untouched": {chunks:[{chunkKind?,name},...]} -- the PASS_TO_PASS
				//! guard: every listed chunk must be byte-identical in
				//! handle.initialDocument (the scene AS LOADED, before the
				//! agent's first turn) vs. the POST-run document.
				CheckOutcome CheckUntouchedKind( const JsonValue& cp, const AgentEvalRunHandle& handle )
				{
					if( !handle.dispatcher || !handle.dispatcher->Session() )
						return { false, "untouched checkpoint: no live session (run did not complete: " +
							handle.result.terminalStatus + ")" };
					if( !cp.has( "chunks" ) || !cp.get( "chunks" ).isArray() || cp.get( "chunks" ).size() == 0 )
						return { false, "untouched checkpoint missing non-empty array field \"chunks\"" };

					const Document initialDoc = RISE::Cst::ParseToCst( handle.initialDocument );
					const Document finalDoc = RISE::Cst::ParseToCst( handle.dispatcher->Session()->ReadDocument() );

					const JsonValue& chunksArr = cp.get( "chunks" );
					std::vector<std::string> problems;
					for( std::size_t i = 0; i < chunksArr.size(); ++i ) {
						const JsonValue& c = chunksArr.at( i );
						if( !c.isObject() || !c.has( "name" ) || !c.get( "name" ).isString() ) {
							problems.push_back( "chunks[" + std::to_string( i ) + "] must be an object with a string \"name\"" );
							continue;
						}
						const std::string name = c.get( "name" ).asString();
						const std::string kind = OptKind( c );

						NodeRef initChunk, finalChunk;
						const int foundInit  = CheckerFindChunk( initialDoc, kind, name, initChunk );
						const int foundFinal = CheckerFindChunk( finalDoc, kind, name, finalChunk );
						if( foundInit != 1 ) { problems.push_back( "'" + name + "' not uniquely found in the INITIAL scene" ); continue; }
						if( foundFinal != 1 ) { problems.push_back( "'" + name + "' not uniquely found in the FINAL scene" ); continue; }
						if( CheckerNodeBytes( initChunk ) != CheckerNodeBytes( finalChunk ) )
							problems.push_back( "'" + name + "' CHANGED (not byte-identical to the initial scene)" );
					}
					if( !problems.empty() ) {
						std::string detail = "untouched violated: ";
						for( std::size_t i = 0; i < problems.size(); ++i ) { if( i ) detail += "; "; detail += problems[i]; }
						return { false, detail };
					}
					return { true, "all " + std::to_string( chunksArr.size() ) + " named chunk(s) byte-identical to the initial scene" };
				}

				//! "render": {width?,height?,seed?,meanLumaMin?/Max?,meanRMin?/Max?,
				//! meanGMin?/Max?,meanBMin?/Max?} -- a FRESH render through the
				//! live session (AgentSession::Render, bypassing the JSON-RPC
				//! layer -- the checker is a privileged verifier), asserted
				//! against generous [min,max] bands.  `seed` is accepted (never
				//! rejected -- an author may want to note the seed a fixture
				//! was authored against) but has NO EFFECT: AgentSession::Render
				//! exposes no seed/RNG-pinning control (see AgentRenderParams --
				//! samples/width/height/camera/pinned/quality/mode only), so
				//! per the plan's tolerance-banded rule, bands MUST be wide
				//! enough to absorb ordinary MC noise between runs, not narrow
				//! enough to require a pinned seed.  NEVER an exact-pixel match.
				CheckOutcome CheckRenderKind( const JsonValue& cp, AgentSession* session )
				{
					if( !session ) return { false, "render checkpoint: no live session (run did not complete)" };

					AgentRenderParams rp;
					const bool haveW = cp.has( "width" )  && cp.get( "width" ).isNumber();
					const bool haveH = cp.has( "height" ) && cp.get( "height" ).isNumber();
					if( haveW && haveH ) {
						rp.width  = static_cast<unsigned int>( cp.get( "width" ).asNumber() );
						rp.height = static_cast<unsigned int>( cp.get( "height" ).asNumber() );
					} else if( haveW != haveH ) {
						return { false, "render checkpoint: \"width\"/\"height\" must both be supplied together" };
					}

					const AgentRenderResult rr = session->Render( rp );
					if( !rr.ok ) return { false, "render checkpoint: render failed: " + rr.message };

					// Rec.709 linear luma weights (RISEPel is Rec709RGBPel post
					// the 2026-05-24 colour-space migration) -- a single scalar
					// band that absorbs per-channel MC noise better than
					// checking each channel independently when the author only
					// cares about overall brightness.
					const double meanLuma = 0.2126 * rr.meanR + 0.7152 * rr.meanG + 0.0722 * rr.meanB;

					std::vector<std::string> failures;
					auto checkBand = [&]( const char* minKey, const char* maxKey, const char* label, double actual ) {
						if( cp.has( minKey ) && cp.get( minKey ).isNumber() ) {
							const double lo = cp.get( minKey ).asNumber();
							if( actual < lo )
								failures.push_back( std::string( label ) + "=" + std::to_string( actual ) + " < min " + std::to_string( lo ) );
						}
						if( cp.has( maxKey ) && cp.get( maxKey ).isNumber() ) {
							const double hi = cp.get( maxKey ).asNumber();
							if( actual > hi )
								failures.push_back( std::string( label ) + "=" + std::to_string( actual ) + " > max " + std::to_string( hi ) );
						}
					};
					checkBand( "meanLumaMin", "meanLumaMax", "meanLuma", meanLuma );
					checkBand( "meanRMin", "meanRMax", "meanR", rr.meanR );
					checkBand( "meanGMin", "meanGMax", "meanG", rr.meanG );
					checkBand( "meanBMin", "meanBMax", "meanB", rr.meanB );

					if( !failures.empty() ) {
						std::string detail = "render band(s) violated: ";
						for( std::size_t i = 0; i < failures.size(); ++i ) { if( i ) detail += "; "; detail += failures[i]; }
						return { false, detail };
					}
					char buf[256];
					std::snprintf( buf, sizeof( buf ), "render bands satisfied (meanR=%.4f meanG=%.4f meanB=%.4f meanLuma=%.4f)",
						rr.meanR, rr.meanG, rr.meanB, meanLuma );
					return { true, std::string( buf ) };
				}

				//! "objectmap": {legendContains?,pixelCountFor?,pixelCountMin?,
				//! pixelCountMax?,queryAt?:{x,y,expectName}} -- legendContains/
				//! pixelCountFor run ONE mode:"objectmap" render (only if
				//! needed); queryAt runs AgentSession::QueryObjectAt (which does
				//! its own internal objectmap render regardless).  At least one
				//! of the three assertions must be present.
				CheckOutcome CheckObjectmapKind( const JsonValue& cp, AgentSession* session )
				{
					if( !session ) return { false, "objectmap checkpoint: no live session (run did not complete)" };
					if( !cp.has( "legendContains" ) && !cp.has( "pixelCountFor" ) && !cp.has( "queryAt" ) )
						return { false, "objectmap checkpoint carries none of legendContains/pixelCountFor/queryAt" };

					std::vector<std::string> failures;
					const bool needsLegend = cp.has( "legendContains" ) || cp.has( "pixelCountFor" );
					AgentRenderResult rr;
					if( needsLegend ) {
						AgentRenderParams rp;
						rp.renderTarget = AgentRenderTarget::ObjectMap;
						rr = session->Render( rp );
						if( !rr.ok ) return { false, "objectmap checkpoint: render failed: " + rr.message };
					}

					if( cp.has( "legendContains" ) && cp.get( "legendContains" ).isString() ) {
						const std::string want = cp.get( "legendContains" ).asString();
						bool found = false;
						for( const auto& e : rr.legend ) if( e.name == want ) { found = true; break; }
						if( !found ) failures.push_back( "legend does not contain '" + want + "'" );
					}

					if( cp.has( "pixelCountFor" ) && cp.get( "pixelCountFor" ).isString() ) {
						const std::string name = cp.get( "pixelCountFor" ).asString();
						const LegendEntry* entry = nullptr;
						for( const auto& e : rr.legend ) if( e.name == name ) { entry = &e; break; }
						if( !entry ) {
							failures.push_back( "pixelCountFor: '" + name + "' not in legend" );
						} else {
							const double count = static_cast<double>( entry->pixelCount );
							if( cp.has( "pixelCountMin" ) && cp.get( "pixelCountMin" ).isNumber() && count < cp.get( "pixelCountMin" ).asNumber() )
								failures.push_back( "'" + name + "' pixelCount " + std::to_string( entry->pixelCount ) +
									" < min " + std::to_string( cp.get( "pixelCountMin" ).asNumber() ) );
							if( cp.has( "pixelCountMax" ) && cp.get( "pixelCountMax" ).isNumber() && count > cp.get( "pixelCountMax" ).asNumber() )
								failures.push_back( "'" + name + "' pixelCount " + std::to_string( entry->pixelCount ) +
									" > max " + std::to_string( cp.get( "pixelCountMax" ).asNumber() ) );
						}
					}

					if( cp.has( "queryAt" ) && cp.get( "queryAt" ).isObject() ) {
						const JsonValue& qa = cp.get( "queryAt" );
						if( !qa.has( "x" ) || !qa.get( "x" ).isNumber() || !qa.has( "y" ) || !qa.get( "y" ).isNumber() ) {
							failures.push_back( "queryAt requires numeric \"x\",\"y\"" );
						} else if( !qa.has( "expectName" ) || !qa.get( "expectName" ).isString() ) {
							failures.push_back( "queryAt requires string \"expectName\" (\"\" means expect a miss)" );
						} else {
							const int x = static_cast<int>( qa.get( "x" ).asNumber() );
							const int y = static_cast<int>( qa.get( "y" ).asNumber() );
							const std::string expectName = qa.get( "expectName" ).asString();
							const AgentSession::AgentQueryObjectResult qr = session->QueryObjectAt( x, y );
							if( qr.outOfRange ) {
								failures.push_back( "queryAt(" + std::to_string( x ) + "," + std::to_string( y ) + ") out of range" );
							} else if( expectName.empty() ) {
								if( qr.hit ) failures.push_back( "queryAt expected a miss but hit '" + qr.name + "'" );
							} else if( !qr.hit || qr.name != expectName ) {
								failures.push_back( "queryAt(" + std::to_string( x ) + "," + std::to_string( y ) + ") expected '" +
									expectName + "', got " + ( qr.hit ? ( "'" + qr.name + "'" ) : std::string( "a miss" ) ) );
							}
						}
					}

					if( !failures.empty() ) {
						std::string detail; for( std::size_t i = 0; i < failures.size(); ++i ) { if( i ) detail += "; "; detail += failures[i]; }
						return { false, detail };
					}
					return { true, "objectmap assertion(s) satisfied" };
				}

				//! "diagnostics": {expect:"clean"|"code",code?} -- validate run
				//! against the CURRENT (post-run) document.
				CheckOutcome CheckDiagnosticsKind( const JsonValue& cp, AgentSession* session )
				{
					if( !session ) return { false, "diagnostics checkpoint: no live session (run did not complete)" };
					if( !cp.has( "expect" ) || !cp.get( "expect" ).isString() )
						return { false, "diagnostics checkpoint missing string field \"expect\"" };
					const std::string expect = cp.get( "expect" ).asString();
					const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( session->ReadDocument() );

					if( expect == "clean" ) {
						if( diags.empty() ) return { true, "validate: clean (0 diagnostics)" };
						return { false, "validate: expected clean but found " + std::to_string( diags.size() ) +
							" diagnostic(s); first: " + diags[0].code + " " + diags[0].message };
					}
					if( expect == "code" ) {
						if( !cp.has( "code" ) || !cp.get( "code" ).isString() )
							return { false, "diagnostics checkpoint expect:\"code\" requires string field \"code\"" };
						const std::string wantCode = cp.get( "code" ).asString();
						for( const auto& d : diags )
							if( d.code == wantCode ) return { true, "validate: found expected code '" + wantCode + "'" };
						return { false, "validate: expected code '" + wantCode + "' not found among " +
							std::to_string( diags.size() ) + " diagnostic(s)" };
					}
					return { false, "diagnostics checkpoint: unknown \"expect\" value '" + expect + "' (must be \"clean\" or \"code\")" };
				}

				//! ChatTrajectory's EmbedJsonOrString embeds jsonrpc.response as
				//! a parsed OBJECT when it is valid JSON (every real response
				//! is), else falls back to a raw string -- handle both, and
				//! return the parsed JSON-RPC envelope as an object (Null if
				//! neither form yields one).
				JsonValue ExtractToolResponseEnvelope( const JsonValue& toolRecord )
				{
					const JsonValue& resp = toolRecord.get( "jsonrpc.response" );
					if( resp.isObject() ) return resp;
					if( resp.isString() ) {
						JsonValue parsed; std::string perr;
						if( JsonParse( resp.asString(), parsed, perr ) && parsed.isObject() ) return parsed;
					}
					return JsonValue();
				}

				//! "trajectory": {maxToolCalls?,maxLlmCalls?,terminalStatus?,
				//! noAutonomyRefusal?,requiredToolInOrder?,noMechanicalLoop?,
				//! expectAutonomyRefusal?,toolOutcomes?,toolCallAfterUserTurn?}
				//! -- maxToolCalls/maxLlmCalls/terminalStatus read straight off
				//! handle.result (the SAME counters RunScenario wrote into the
				//! summary line -- no need to re-derive them from the JSONL).
				//! noAutonomyRefusal/requiredToolInOrder/noMechanicalLoop/
				//! expectAutonomyRefusal/toolOutcomes parse handle.trajectoryPath's
				//! "tool" records (name/args/jsonrpc.response), since those need
				//! per-call detail the summary doesn't carry; toolCallAfterUserTurn
				//! additionally walks the full ORDERED record stream (interleaving
				//! "user" and "tool" records) to test a tool call against a running
				//! user-turn count.
				CheckOutcome CheckTrajectoryKind( const JsonValue& cp, const AgentEvalRunHandle& handle )
				{
					std::vector<std::string> failures;

					if( cp.has( "maxToolCalls" ) && cp.get( "maxToolCalls" ).isNumber() ) {
						const double m = cp.get( "maxToolCalls" ).asNumber();
						if( handle.result.toolCalls > m )
							failures.push_back( "toolCalls " + std::to_string( handle.result.toolCalls ) + " > maxToolCalls " + std::to_string( m ) );
					}
					if( cp.has( "maxLlmCalls" ) && cp.get( "maxLlmCalls" ).isNumber() ) {
						const double m = cp.get( "maxLlmCalls" ).asNumber();
						if( handle.result.llmCalls > m )
							failures.push_back( "llmCalls " + std::to_string( handle.result.llmCalls ) + " > maxLlmCalls " + std::to_string( m ) );
					}
					if( cp.has( "terminalStatus" ) && cp.get( "terminalStatus" ).isString() ) {
						const std::string want = cp.get( "terminalStatus" ).asString();
						if( handle.result.terminalStatus != want )
							failures.push_back( "terminalStatus '" + handle.result.terminalStatus + "' != expected '" + want + "'" );
					}

					const bool needsRecords = cp.has( "noAutonomyRefusal" ) || cp.has( "requiredToolInOrder" ) ||
						cp.has( "noMechanicalLoop" ) || cp.has( "expectAutonomyRefusal" ) ||
						cp.has( "toolOutcomes" ) || cp.has( "toolCallAfterUserTurn" );
					if( needsRecords ) {
						if( handle.trajectoryPath.empty() ) {
							failures.push_back( "trajectory file unavailable (run did not complete) -- cannot check "
								"noAutonomyRefusal/requiredToolInOrder/noMechanicalLoop/expectAutonomyRefusal/"
								"toolOutcomes/toolCallAfterUserTurn" );
						} else {
							std::ifstream f( handle.trajectoryPath.c_str(), std::ios::binary );
							if( !f ) {
								failures.push_back( "trajectory file '" + handle.trajectoryPath + "' could not be opened" );
							} else {
								// One ordered pass over every record (session/user/
								// llm/tool/summary, in trajectory order) -- toolRecords
								// is derived from it so the pre-existing per-tool
								// checks below are untouched, while
								// toolCallAfterUserTurn gets the interleaved user/tool
								// order it needs.
								std::vector<JsonValue> allRecords;
								std::string line;
								while( std::getline( f, line ) ) {
									if( line.empty() ) continue;
									JsonValue v; std::string perr;
									if( !JsonParse( line, v, perr ) ) continue;   // tolerate a stray non-JSON line (never emitted by the recorder)
									if( v.isObject() ) allRecords.push_back( v );
								}
								std::vector<JsonValue> toolRecords;
								for( const auto& v : allRecords )
									if( v.get( "run_type" ).asString() == "tool" ) toolRecords.push_back( v );

								// Shared by noAutonomyRefusal (fail if present) and
								// expectAutonomyRefusal (fail if absent) -- one scan so a
								// future change to the -32011 detection can't diverge
								// between the two.  Returns the FIRST refusing tool record,
								// or nullptr when no -32011 occurred.
								auto firstAutonomyRefusal = [&]() -> const JsonValue* {
									for( const auto& t : toolRecords ) {
										const JsonValue env = ExtractToolResponseEnvelope( t );
										if( env.isObject() && env.has( "error" ) && env.get( "error" ).get( "code" ).asNumber( 0.0 ) == -32011.0 )
											return &t;
									}
									return nullptr;
								};

								if( cp.has( "noAutonomyRefusal" ) && cp.get( "noAutonomyRefusal" ).isBool() && cp.get( "noAutonomyRefusal" ).asBool() ) {
									if( const JsonValue* t = firstAutonomyRefusal() )
										failures.push_back( "an autonomy refusal (-32011) occurred on tool '" + t->get( "name" ).asString() + "'" );
								}

								if( cp.has( "expectAutonomyRefusal" ) && cp.get( "expectAutonomyRefusal" ).isBool() && cp.get( "expectAutonomyRefusal" ).asBool() ) {
									if( firstAutonomyRefusal() == nullptr )
										failures.push_back( "expectAutonomyRefusal: no tool call's response carried an autonomy refusal (-32011)" );
								}

								if( cp.has( "requiredToolInOrder" ) && cp.get( "requiredToolInOrder" ).isArray() ) {
									const JsonValue& req = cp.get( "requiredToolInOrder" );
									std::size_t ri = 0;
									for( std::size_t ti = 0; ti < toolRecords.size() && ri < req.size(); ++ti ) {
										if( req.at( ri ).isString() && toolRecords[ti].get( "name" ).asString() == req.at( ri ).asString() ) ++ri;
									}
									if( ri < req.size() )
										failures.push_back( "requiredToolInOrder: not satisfied as a subsequence (matched " +
											std::to_string( ri ) + "/" + std::to_string( req.size() ) + ")" );
								}

								if( cp.has( "noMechanicalLoop" ) && cp.get( "noMechanicalLoop" ).isBool() && cp.get( "noMechanicalLoop" ).asBool() ) {
									for( std::size_t ti = 1; ti < toolRecords.size(); ++ti ) {
										const std::string prevName = toolRecords[ti - 1].get( "name" ).asString();
										const std::string curName  = toolRecords[ti].get( "name" ).asString();
										if( prevName != curName ) continue;
										const std::string prevArgs = JsonSerialize( toolRecords[ti - 1].get( "args" ) );
										const std::string curArgs  = JsonSerialize( toolRecords[ti].get( "args" ) );
										if( prevArgs == curArgs ) {
											failures.push_back( "mechanical loop: identical tool call '" + curName +
												"' repeated consecutively (calls " + std::to_string( ti ) + "," + std::to_string( ti + 1 ) + ")" );
											break;
										}
									}
								}

								if( cp.has( "toolOutcomes" ) && cp.get( "toolOutcomes" ).isArray() ) {
									// Per-spec check: "applied" tests result.applied==true;
									// "staged"/"rejected" test result.status; "error" tests
									// the envelope carries an "error" object.  An unrecognized
									// expect value never matches (fails loudly via the caller's
									// !ok check below, not silently).
									auto satisfiesExpect = [&]( const JsonValue& t, const std::string& expect ) -> bool {
										const JsonValue env = ExtractToolResponseEnvelope( t );
										if( !env.isObject() ) return false;
										if( expect == "error" ) return env.has( "error" );
										const JsonValue& result = env.get( "result" );
										if( expect == "applied" )  return result.isObject() && result.get( "applied" ).asBool( false );
										if( expect == "staged" )   return result.isObject() && result.get( "status" ).asString() == "staged";
										if( expect == "rejected" ) return result.isObject() && result.get( "status" ).asString() == "rejected";
										return false;
									};

									const JsonValue& specs = cp.get( "toolOutcomes" );
									for( std::size_t si = 0; si < specs.size(); ++si ) {
										const JsonValue& spec = specs.at( si );
										if( !spec.isObject() ) continue;   // load-time-validated shape; defensive skip only
										const std::string wantName = spec.get( "name" ).asString();
										const std::string occurrence = spec.has( "occurrence" ) && spec.get( "occurrence" ).isString()
											? spec.get( "occurrence" ).asString() : "any";
										const std::string expect = spec.get( "expect" ).asString();

										std::vector<const JsonValue*> matches;
										for( const auto& t : toolRecords )
											if( t.get( "name" ).asString() == wantName ) matches.push_back( &t );

										if( matches.empty() ) {
											failures.push_back( "toolOutcomes[" + std::to_string( si ) + "]: no tool call named '" + wantName + "' occurred" );
											continue;
										}

										bool ok = false;
										if( occurrence == "first" )      ok = satisfiesExpect( *matches.front(), expect );
										else if( occurrence == "last" )  ok = satisfiesExpect( *matches.back(), expect );
										else if( occurrence == "any" ) {
											for( const JsonValue* m : matches )
												if( satisfiesExpect( *m, expect ) ) { ok = true; break; }
										}
										else {
											// Defense-in-depth: LoadEvalScenario rejects any
											// occurrence outside {first,last,any}, but a hand-built
											// scenario could reach here -- fail LOUDLY rather than
											// silently downgrading to the weakest "any" check.
											failures.push_back( "toolOutcomes[" + std::to_string( si ) + "]: unknown occurrence '" +
												occurrence + "' (must be first|last|any)" );
											continue;
										}

										if( !ok )
											failures.push_back( "toolOutcomes[" + std::to_string( si ) + "]: '" + wantName + "' (" + occurrence +
												") did not satisfy expect:'" + expect + "'" );
									}
								}

								if( cp.has( "toolCallAfterUserTurn" ) && cp.get( "toolCallAfterUserTurn" ).isObject() ) {
									const JsonValue& spec = cp.get( "toolCallAfterUserTurn" );
									const std::string wantName = spec.get( "name" ).asString();
									const long long minUserTurns = static_cast<long long>( spec.get( "minUserTurns" ).asNumber( 0.0 ) );

									long long userCount = 0;
									bool found = false;
									for( const auto& v : allRecords ) {
										const std::string rt = v.get( "run_type" ).asString();
										if( rt == "user" ) { ++userCount; continue; }
										if( rt == "tool" && v.get( "name" ).asString() == wantName && userCount >= minUserTurns ) {
											found = true;
											break;
										}
									}
									if( !found )
										failures.push_back( "toolCallAfterUserTurn: no tool call '" + wantName + "' occurred at userTurns >= " +
											std::to_string( minUserTurns ) );
								}
							}
						}
					}

					if( !failures.empty() ) {
						std::string detail; for( std::size_t i = 0; i < failures.size(); ++i ) { if( i ) detail += "; "; detail += failures[i]; }
						return { false, detail };
					}
					return { true, "trajectory assertion(s) satisfied" };
				}

				//! "finalText": {containsAll?/containsAny?/absent?:string array,
				//! caseSensitive?:bool} -- asserts on handle.result.finalText
				//! (the last FinalText turn's text) so refusal/clarification
				//! scenarios can check the ask-back message actually surfaces
				//! the right content, not just structural state.  At least one
				//! of containsAll/containsAny/absent must be present -- a
				//! checkpoint with none of the three is a FAILED checkpoint
				//! (loud), never a silent vacuous pass.
				CheckOutcome CheckFinalTextKind( const JsonValue& cp, const AgentEvalRunHandle& handle )
				{
					const bool caseSensitive = cp.has( "caseSensitive" ) && cp.get( "caseSensitive" ).isBool() && cp.get( "caseSensitive" ).asBool();

					auto toLowerAscii = []( std::string s ) {
						for( char& c : s ) c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
						return s;
					};

					const std::string haystack = caseSensitive ? handle.result.finalText : toLowerAscii( handle.result.finalText );

					auto contains = [&]( const std::string& needle ) {
						const std::string n = caseSensitive ? needle : toLowerAscii( needle );
						return haystack.find( n ) != std::string::npos;
					};

					auto truncate = []( const std::string& s ) {
						return s.size() > 60 ? s.substr( 0, 60 ) + "..." : s;
					};

					// A "needle" is a non-empty STRING array entry -- empty ("")
					// and non-string entries assert nothing (an empty needle would
					// spuriously always-match on contains / always-trip on absent),
					// so they are skipped everywhere below.  The load-time validator
					// already rejects non-string entries; this is belt-and-suspenders
					// for a hand-built scenario (as the unit tests use).
					auto meaningfulNeedles = [&]( const char* key ) -> std::size_t {
						if( !cp.has( key ) || !cp.get( key ).isArray() ) return 0;
						const JsonValue& a = cp.get( key );
						std::size_t n = 0;
						for( std::size_t i = 0; i < a.size(); ++i )
							if( a.at( i ).isString() && !a.at( i ).asString().empty() ) ++n;
						return n;
					};

					// LOUD, never a silent vacuous pass: a finalText checkpoint must
					// carry at least ONE meaningful needle across the three arrays.
					// This catches both the all-keys-absent case AND the
					// present-but-empty case (containsAll:[] or containsAll:[""]).
					if( meaningfulNeedles( "containsAll" ) + meaningfulNeedles( "containsAny" ) + meaningfulNeedles( "absent" ) == 0 )
						return { false, "finalText checkpoint has no assertion "
							"(needs a non-empty containsAll, containsAny, or absent string)" };

					if( cp.has( "containsAll" ) && cp.get( "containsAll" ).isArray() ) {
						const JsonValue& arr = cp.get( "containsAll" );
						for( std::size_t i = 0; i < arr.size(); ++i ) {
							if( !arr.at( i ).isString() ) continue;
							const std::string needle = arr.at( i ).asString();
							if( needle.empty() ) continue;
							if( !contains( needle ) )
								return { false, "finalText: containsAll failed -- missing \"" + truncate( needle ) + "\"" };
						}
					}
					if( cp.has( "containsAny" ) && cp.get( "containsAny" ).isArray() ) {
						const JsonValue& arr = cp.get( "containsAny" );
						bool sawNeedle = false, any = false;
						for( std::size_t i = 0; i < arr.size(); ++i ) {
							if( !arr.at( i ).isString() ) continue;
							const std::string needle = arr.at( i ).asString();
							if( needle.empty() ) continue;
							sawNeedle = true;
							if( contains( needle ) ) { any = true; break; }
						}
						// Only enforce when this array actually carried needles -- an
						// empty containsAny alongside a real containsAll asserts nothing.
						if( sawNeedle && !any )
							return { false, "finalText: containsAny failed -- none of the candidates were found" };
					}
					if( cp.has( "absent" ) && cp.get( "absent" ).isArray() ) {
						const JsonValue& arr = cp.get( "absent" );
						for( std::size_t i = 0; i < arr.size(); ++i ) {
							if( !arr.at( i ).isString() ) continue;
							const std::string needle = arr.at( i ).asString();
							if( needle.empty() ) continue;
							if( contains( needle ) )
								return { false, "finalText: absent failed -- found forbidden \"" + truncate( needle ) + "\"" };
						}
					}
					return { true, "finalText assertion(s) satisfied" };
				}

				//! Dispatch one checkpoint object by its "kind".  An
				//! unrecognized kind is a FAILED checkpoint (loud), never a
				//! silent skip -- see CheckScenario's doc.
				CheckOutcome CheckOneCheckpoint( const JsonValue& cp, const AgentEvalRunHandle& handle )
				{
					if( !cp.isObject() || !cp.has( "kind" ) || !cp.get( "kind" ).isString() )
						return { false, "checkpoint is not an object with a string \"kind\"" };
					const std::string kind = cp.get( "kind" ).asString();
					AgentSession* session = handle.dispatcher ? handle.dispatcher->Session() : nullptr;

					if( kind == "document" )    return CheckDocumentKind( cp, session );
					if( kind == "untouched" )   return CheckUntouchedKind( cp, handle );
					if( kind == "render" )      return CheckRenderKind( cp, session );
					if( kind == "objectmap" )   return CheckObjectmapKind( cp, session );
					if( kind == "diagnostics" ) return CheckDiagnosticsKind( cp, session );
					if( kind == "trajectory" )  return CheckTrajectoryKind( cp, handle );
					if( kind == "finalText" )   return CheckFinalTextKind( cp, handle );

					return { false, "unknown checkpoint kind '" + kind + "'" };
				}
			}

			AgentEvalCheckResult CheckScenario( const AgentEvalRunHandle& handle, const AgentEvalScenario& scenario )
			{
				AgentEvalCheckResult result;
				result.scenarioId = scenario.id;

				double weightSum = 0.0, passSum = 0.0;
				for( std::size_t i = 0; i < scenario.checkpoints.size(); ++i ) {
					const JsonValue& cp = scenario.checkpoints.at( i );

					AgentEvalCheckpointResult r;
					r.kind = ( cp.isObject() && cp.has( "kind" ) && cp.get( "kind" ).isString() )
						? cp.get( "kind" ).asString() : std::string( "<malformed>" );
					r.weight = ( cp.isObject() && cp.has( "weight" ) && cp.get( "weight" ).isNumber() )
						? cp.get( "weight" ).asNumber() : 1.0;
					if( r.weight < 0.0 ) r.weight = 0.0;   // a negative weight would corrupt the fraction -- clamp defensively

					const CheckOutcome oc = CheckOneCheckpoint( cp, handle );
					r.passed = oc.passed;
					r.detail = oc.detail;

					weightSum += r.weight;
					if( r.passed ) passSum += r.weight;
					result.checkpoints.push_back( r );
				}

				if( weightSum > 0.0 ) {
					result.checkpointFraction = passSum / weightSum;
				} else if( !result.checkpoints.empty() ) {
					// Every checkpoint has zero weight (degenerate authoring):
					// the weighted fraction is undefined, so fall back to the
					// UNWEIGHTED pass fraction.  A failing checkpoint must still
					// drag the score below 1.0 -- returning a flat 1.0 here would
					// report a spurious perfect score for a scenario that failed.
					std::size_t passedCount = 0;
					for( const auto& c : result.checkpoints ) if( c.passed ) ++passedCount;
					result.checkpointFraction =
						static_cast<double>( passedCount ) / static_cast<double>( result.checkpoints.size() );
				} else {
					result.checkpointFraction = 1.0;   // no checkpoints -> vacuously complete
				}
				result.allPassed = true;
				for( const auto& c : result.checkpoints ) if( !c.passed ) { result.allPassed = false; break; }

				// Terminal-success precondition: a run that ended on a non-success terminal
				// status (a budget stop, provider/transport error, load error, replay
				// exhaustion -- anything but "final_text") is NOT a success regardless of
				// checkpoint state, UNLESS the scenario explicitly expects that status (a
				// trajectory checkpoint asserting terminalStatus == the actual status -- an
				// intentional budget/refusal test). Every current scenario asserts
				// terminalStatus:"final_text", so a passing run already ends final_text and
				// this changes nothing for them; it guards a FUTURE scenario that omits the
				// assertion from silently scoring a budget/provider-killed run as a success.
				const std::string ts = handle.result.terminalStatus;
				if( result.allPassed && ts != "final_text" ) {
					bool expected = false;
					if( scenario.checkpoints.isArray() ) {
						for( std::size_t i = 0; i < scenario.checkpoints.size(); ++i ) {
							const JsonValue& cp = scenario.checkpoints.at( i );
							if( !cp.isObject() ) continue;
							if( !cp.has( "kind" ) || !cp.get( "kind" ).isString() || cp.get( "kind" ).asString() != "trajectory" ) continue;
							if( !cp.has( "terminalStatus" ) || !cp.get( "terminalStatus" ).isString() ) continue;
							if( cp.get( "terminalStatus" ).asString() == ts ) { expected = true; break; }
						}
					}
					if( !expected ) {
						result.allPassed = false;
						result.terminalGateNote = "run ended on non-success terminal status '" + ts + "' with no checkpoint asserting it -- scored as failure";
					}
				}

				// Write the results ledger alongside the trajectory.  runDir is
				// recovered from trajectoryPath (or, failing that, resultPath)
				// rather than taken as a parameter -- CheckScenario's signature
				// is (handle, scenario) only, per the plan -- so a "load_error"
				// handle (neither path set) simply skips the write; there is
				// nothing on disk to sit "alongside" in that case anyway.
				std::string runDir;
				if( !handle.trajectoryPath.empty() )      runDir = std::filesystem::path( handle.trajectoryPath ).parent_path().string();
				else if( !handle.resultPath.empty() )     runDir = std::filesystem::path( handle.resultPath ).parent_path().string();

				if( !runDir.empty() ) {
					JsonValue root = JsonValue::MakeObject();
					root.set( "scenarioId", JsonValue::MakeString( result.scenarioId ) );
					root.set( "checkpointFraction", JsonValue::MakeNumber( result.checkpointFraction ) );
					root.set( "allPassed", JsonValue::MakeBool( result.allPassed ) );
					if( !result.terminalGateNote.empty() ) root.set( "terminalGateNote", JsonValue::MakeString( result.terminalGateNote ) );
					// RECORDED either way: terminalStatus lets a reviewer see what the
					// run actually ended on.  Pass/fail stays checkpoint-fraction-based
					// (final-scene-state) UNLESS the terminal-success gate above forced
					// allPassed=false for a non-"final_text" status the scenario never
					// asserted -- in that case terminalGateNote (set above) explains why.
					// handle.result is the same summary line RunScenario/RunScenarioLive
					// already wrote to <id>.result.jsonl.
					root.set( "terminalStatus", JsonValue::MakeString( handle.result.terminalStatus ) );
					JsonValue arr = JsonValue::MakeArray();
					for( const auto& c : result.checkpoints ) {
						JsonValue o = JsonValue::MakeObject();
						o.set( "kind", JsonValue::MakeString( c.kind ) );
						o.set( "passed", JsonValue::MakeBool( c.passed ) );
						o.set( "weight", JsonValue::MakeNumber( c.weight ) );
						o.set( "detail", JsonValue::MakeString( c.detail ) );
						arr.push_back( o );
					}
					root.set( "checkpoints", arr );

					std::error_code ec;
					std::filesystem::create_directories( runDir, ec );
					const std::string resultsPath = ( std::filesystem::path( runDir ) / "results.jsonl" ).string();
					std::ofstream f( resultsPath.c_str(), std::ios::binary | std::ios::app );
					if( f ) f << JsonSerialize( root ) << "\n";
				}

				return result;
			}
	}
}
