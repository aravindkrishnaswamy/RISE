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

#include <type_traits>   // static_assert guarding the RISEPel == Rec709RGBPel decode assumption
#include "pch.h"
#include "AgentEvalRunner.h"

#include "AgentSession.h"
#include "AgentRpc.h"
#include "AgentDiagnostic.h"
#include "Base64.h"   // image-reconstruction Wave 1: Base64Encode for reference-image attachments
#include "../Cst/Cst.h"   // Eval-harness slice E3: the "document"/"untouched" checkpoint kinds walk the CST directly
#include "../Version.h"   // RISE_VER_* -- the run manifest's riseBuild string
#include "../Interfaces/IJobPriv.h"                // propose-mode mock-Owner: RISE_CreateJobPriv + IJobPriv::release/LoadAsciiSceneViaCst
#include "../SceneEditor/SceneEditController.h"    // propose-mode mock-Owner: the unstarted headless controller ProposePatch stages against
#include "../Interfaces/IRasterImageReader.h"      // image-reconstruction Wave 2: render "compareToImage" decodes a reference PNG (brings RISEColor + COLOR_SPACE via Color.h)
#include "../RISE_API.h"                           // image-reconstruction Wave 2: RISE_API_CreatePNGReader / RISE_API_CreateCompatibleMemoryBuffer -- decode a PNG through RISE's OWN reader (png_painter's decoder)

#include <algorithm> // std::max / std::min -- render channelBalanceMax ratio
#include <cctype>
#include <cerrno>    // ERANGE -- string-layer overflow rejection (see CheckerParseFloatTokens)
#include <cmath>
#include <cstdint>   // std::uint64_t -- ScenarioContentHash's FNV-1a accumulator
#include <cstdio>
#include <cstdlib>   // std::strtod -- numeric param_equals token parse
#include <cstring>   // std::strlen -- ParseRateLimit429HintMs phrase-length skip
#include <chrono>    // std::chrono::milliseconds -- the real (non-injected) HTTP 429 backoff sleep
#include "../Utilities/FiniteMath.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>    // std::this_thread::sleep_for -- the real (non-injected) HTTP 429 backoff sleep

namespace RISE
{
	namespace Agent
	{
		namespace
		{

			//! The single filesystem-identity charset predicate: true for the
			//! characters SanitizeForPath PRESERVES.  Used by BOTH the loader's
			//! scenario-id contract check (LoadEvalScenario) and SanitizeForPath
			//! itself so the two can never drift apart -- two textually-identical
			//! copies would silently reopen the raw-vs-sanitized filename
			//! mismatch the id contract exists to prevent.
			inline bool IsSanitizePreservedChar( char ch )
			{
				const unsigned char u = static_cast<unsigned char>( ch );
				return std::isalnum( u ) || ch == '-' || ch == '.' || ch == '_';
			}
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

			//! Finiteness test for JSON numbers that have already been parsed.
			bool IsFiniteEvalNumber( double v )
			{
				return IsFiniteDouble( v );
			}

			//! ASCII-only lowercase copy -- shared by the ask_user scripted
			//! responder's case-INSENSITIVE substring match (below) and,
			//! were a future caller to need it, anything else wanting the
			//! exact same fold CheckFinalTextKind's local lambda already
			//! uses.  Non-ASCII bytes pass through unchanged (scenario
			//! questions/answers are English prompt text in this harness).
			std::string ToLowerAsciiEval( std::string s )
			{
				for( char& c : s ) c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
				return s;
			}

			//! The ask_user scripted responder (stage 2 of the
			//! clarifying-questions feature -- see
			//! AgentEvalAskUserResponses).  Walks `responses.matches` IN
			//! ORDER, case-INSENSITIVE substring match of each entry's
			//! `contains` against `question`; first hit wins.  No match:
			//! falls back to `responses.default` when present.  Returns
			//! true and fills `outAnswer` on either hit; returns false
			//! (leaving `outAnswer` untouched) when neither a match nor a
			//! default applies -- the caller then synthesizes the stage-1
			//! `{available:false}` stub, preserving that behaviour
			//! byte-for-byte for any scenario/question the responder does
			//! not cover.  Deterministic by construction: no regex, no
			//! randomness, no locale dependence (ASCII-only fold).
			bool ResolveScriptedAskUserAnswer( const AgentEvalAskUserResponses& responses,
			                                    const std::string& question, std::string& outAnswer )
			{
				const std::string haystack = ToLowerAsciiEval( question );
				for( const AgentEvalAskUserMatch& m : responses.matches ) {
					if( haystack.find( ToLowerAsciiEval( m.contains ) ) != std::string::npos ) {
						outAnswer = m.answer;
						return true;
					}
				}
				if( responses.hasDefault ) {
					outAnswer = responses.defaultAnswer;
					return true;
				}
				return false;
			}

			bool ParseScenarioWholeNumber( const JsonValue& value, const std::string& label,
			                               long long minValue, long long maxValue,
			                               long long& out, std::string& err )
			{
				if( !value.isNumber() ) {
					err = label + " must be a number";
					return false;
				}
				const double v = value.asNumber();
				if( !IsFiniteEvalNumber( v ) || v != std::floor( v ) ||
				    v < static_cast<double>( minValue ) || v > static_cast<double>( maxValue ) ) {
					char buf[96];
					std::snprintf( buf, sizeof( buf ), "%.10g", v );
					err = label + " must be a finite whole number in [" +
						std::to_string( minValue ) + "," + std::to_string( maxValue ) +
						"] (got " + buf + ")";
					return false;
				}
				out = static_cast<long long>( v );
				return true;
			}

			std::string JoinPath( const std::string& dir, const std::string& leaf )
			{
				std::filesystem::path p( dir );
				p /= leaf;
				return p.string();
			}

			//! Lower-cased file extension (including the leading '.'), or ""
			//! when `path` has no extension -- the DOT past the last path
			//! separator, so a directory component containing '.' (e.g.
			//! "evals/refs.d/foo") never misparses as an extension.  Used by
			//! RunScenarioDriven's reference-image pre-flight to pick a
			//! ChatAttachment mimeType from a prompt's images[] path.
			std::string FileExtensionLower( const std::string& path )
			{
				const std::size_t slash = path.find_last_of( "/\\" );
				const std::size_t dot = path.find_last_of( '.' );
				if( dot == std::string::npos || ( slash != std::string::npos && dot < slash ) )
					return std::string();
				std::string ext = path.substr( dot );
				for( char& c : ext ) c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
				return ext;
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

			//! As RequireFieldType, but a PRESENT numeric field must also be a
			//! FINITE, WHOLE number within [minVal,maxVal].  Guards fields that
			//! a later stage narrows via static_cast to a smaller/unsigned
			//! type (e.g. CheckRenderKind's width/height/samples) -- a
			//! negative, fractional, non-finite, or absurdly large JSON number
			//! would otherwise reach that cast unchecked and silently wrap,
			//! truncate, or invoke undefined behaviour.  Absent is always
			//! legal; RequireFieldType (called first by every caller here) has
			//! already rejected a present-but-wrong-JSON-TYPE field, so this
			//! only re-reads a field already known to be a number.
			bool RequireWholeNumberInRange( const JsonValue& obj, const std::string& key, double minVal, double maxVal,
			                                 const std::string& scenarioId, std::size_t cpIndex, const std::string& label,
			                                 std::string& err )
			{
				if( !obj.has( key ) || !obj.get( key ).isNumber() ) return true;
				const double v = obj.get( key ).asNumber();
				if( IsFiniteEvalNumber( v ) && v == std::floor( v ) && v >= minVal && v <= maxVal ) return true;
				char vb[64];
				std::snprintf( vb, sizeof( vb ), "%.10g", v );
				err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( cpIndex ) + "].\"" + label +
				      "\" must be a whole number in [" + std::to_string( static_cast<long long>( minVal ) ) + "," +
				      std::to_string( static_cast<long long>( maxVal ) ) + "] (got " + std::string( vb ) + ")";
				return false;
			}

			//! "document" (mirrors CheckDocumentKind): "op"/"target"/"param"/
			//! "value"/"name"/"chunkKind"/"referencedKind" are strings;
			//! "numeric" is a bool; "referencedKinds" is a string array.
			//! "min"/"max" are op-DEPENDENT -- chunk_count reads them as
			//! numbers, param_range reads them as number arrays; every other
			//! op ignores them entirely, so an op-less/unknown-op checkpoint
			//! leaves them unchecked here (an unused stray field is out of
			//! this fix's scope; CheckDocumentKind still runs and grades the
			//! actual op normally).  The three TARGET-based ops (param_equals /
			//! param_range / param_references_kind) additionally accept an
			//! ANY-OF-KIND form -- omit "target" (or leave it empty) and supply
			//! a non-empty "chunkKind" to grade "ANY chunk of this kind
			//! satisfies the assertion" existentially.  A target-less op WITHOUT
			//! a chunkKind is a rubric bug (it addresses no chunk at all) and is
			//! rejected LOUDLY here rather than deferred to a runtime failure.
			//! The optional any-of-kind "where" co-binding filter (equality/range
			//! entries) is validated here and rejected on the named-target form;
			//! the "param_series_orbit" op is validated as any-of-kind-only with
			//! bounded thresholds (minKeyframes>=2, minAngularSpreadDeg in
			//! (0,360], maxRadiusRatio>1, centerWithin={x,z,radius}), plus an
			//! optional non-empty "timeParam" (defaults to "time" at check time).
			bool ValidateDocumentCheckpointTypes( const JsonValue& cp, std::size_t idx, const std::string& scenarioId, std::string& err )
			{
				static const char* const kStringFields[] = { "op", "target", "param", "value", "name", "chunkKind", "referencedKind", "timeParam" };
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

				// ANY-OF-KIND guard: for the three TARGET-based ops, a missing/
				// empty "target" is legal ONLY when a non-empty "chunkKind" is
				// present (the existential form).  Reject the target-less,
				// chunkKind-less shape at load rather than at check time.
				if( op == "param_equals" || op == "param_range" || op == "param_references_kind" ) {
					const bool hasTarget = cp.has( "target" ) && cp.get( "target" ).isString() && !cp.get( "target" ).asString().empty();
					const bool hasChunkKind = cp.has( "chunkKind" ) && cp.get( "chunkKind" ).isString() && !cp.get( "chunkKind" ).asString().empty();
					if( !hasTarget && !hasChunkKind ) {
						err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "] op '" + op +
						      "' has no \"target\" -- the any-of-kind form REQUIRES a non-empty \"chunkKind\"";
						return false;
					}
				}

				// param_series_orbit is ANY-OF-KIND ONLY: it requires a
				// non-empty "chunkKind" and a non-empty string "param", takes
				// NO named "target", and its orbit thresholds have hard bounds
				// (minKeyframes >= 2; minAngularSpreadDeg in (0,360];
				// maxRadiusRatio > 1 when present; centerWithin is {x,z,radius}).
				if( op == "param_series_orbit" ) {
					const bool hasTarget = cp.has( "target" ) && cp.get( "target" ).isString() && !cp.get( "target" ).asString().empty();
					const bool hasChunkKind = cp.has( "chunkKind" ) && cp.get( "chunkKind" ).isString() && !cp.get( "chunkKind" ).asString().empty();
					const std::string pfx = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "] op 'param_series_orbit'";
					if( hasTarget ) { err = pfx + " is any-of-kind only -- it takes no \"target\""; return false; }
					if( !hasChunkKind ) { err = pfx + " REQUIRES a non-empty \"chunkKind\""; return false; }
					if( !cp.has( "param" ) || !cp.get( "param" ).isString() || cp.get( "param" ).asString().empty() ) {
						err = pfx + " REQUIRES a non-empty string \"param\""; return false;
					}
					if( !cp.has( "minKeyframes" ) || !cp.get( "minKeyframes" ).isNumber() || cp.get( "minKeyframes" ).asNumber() < 2.0 ) {
						err = pfx + " REQUIRES a numeric \"minKeyframes\" >= 2"; return false;
					}
					if( !cp.has( "minAngularSpreadDeg" ) || !cp.get( "minAngularSpreadDeg" ).isNumber() ) {
						err = pfx + " REQUIRES a numeric \"minAngularSpreadDeg\""; return false;
					}
					const double spreadReq = cp.get( "minAngularSpreadDeg" ).asNumber();
					if( spreadReq <= 0.0 || spreadReq > 360.0 ) {
						err = pfx + " \"minAngularSpreadDeg\" must be in (0, 360]"; return false;
					}
					if( cp.has( "maxRadiusRatio" ) ) {
						if( !cp.get( "maxRadiusRatio" ).isNumber() || cp.get( "maxRadiusRatio" ).asNumber() <= 1.0 ) {
							err = pfx + " \"maxRadiusRatio\" must be a number > 1"; return false;
						}
					}
					if( cp.has( "centerWithin" ) ) {
						const JsonValue& cw = cp.get( "centerWithin" );
						if( !cw.isObject() ) { err = pfx + " \"centerWithin\" must be an object {x,z,radius}"; return false; }
						if( !cw.has( "x" ) || !cw.get( "x" ).isNumber() || !cw.has( "z" ) || !cw.get( "z" ).isNumber() ||
						    !cw.has( "radius" ) || !cw.get( "radius" ).isNumber() ) {
							err = pfx + " \"centerWithin\" requires numeric \"x\",\"z\",\"radius\""; return false;
						}
						if( cw.get( "radius" ).asNumber() < 0.0 ) { err = pfx + " \"centerWithin.radius\" must be >= 0"; return false; }
					}
					if( cp.has( "timeParam" ) && cp.get( "timeParam" ).asString().empty() ) {
						err = pfx + " \"timeParam\" must be a non-empty string when present"; return false;
					}
				}

				// The optional co-binding "where" filter is an ANY-OF-KIND
				// concept -- it narrows the candidate set of an existential op.
				// Accept it only on the four any-of-kind-capable ops, and only
				// in the target-LESS form; every entry is either an equality
				// entry ({param, value}) or a range entry ({param, min, max}).
				if( cp.has( "where" ) ) {
					const bool opTakesWhere = ( op == "param_equals" || op == "param_range" ||
						op == "param_references_kind" || op == "param_series_orbit" );
					const std::string pfx = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "].\"where\"";
					if( !opTakesWhere ) {
						err = pfx + ": op '" + op + "' does not accept a \"where\" filter";
						return false;
					}
					const bool hasTarget = cp.has( "target" ) && cp.get( "target" ).isString() && !cp.get( "target" ).asString().empty();
					if( hasTarget ) {
						err = pfx + " is an any-of-kind co-binding filter -- it is not valid with a named \"target\"";
						return false;
					}
					const JsonValue& w = cp.get( "where" );
					if( !w.isArray() || w.size() == 0 ) {
						err = pfx + " must be a NON-EMPTY array of filter entries";
						return false;
					}
					for( std::size_t wi = 0; wi < w.size(); ++wi ) {
						const JsonValue& e = w.at( wi );
						const std::string el = pfx + "[" + std::to_string( wi ) + "]";
						if( !e.isObject() ) { err = el + " must be an object"; return false; }
						if( !e.has( "param" ) || !e.get( "param" ).isString() || e.get( "param" ).asString().empty() ) {
							err = el + " requires a non-empty string \"param\""; return false;
						}
						const bool isRange = e.has( "min" ) || e.has( "max" );
						if( isRange ) {
							if( !e.has( "min" ) || !e.get( "min" ).isArray() || !e.has( "max" ) || !e.get( "max" ).isArray() ) {
								err = el + " range entry requires array \"min\" AND array \"max\""; return false;
							}
							const JsonValue& mn = e.get( "min" ); const JsonValue& mx = e.get( "max" );
							if( mn.size() == 0 || mn.size() != mx.size() ) {
								err = el + " range entry \"min\"/\"max\" must be non-empty arrays of equal length"; return false;
							}
							for( std::size_t k = 0; k < mn.size(); ++k )
								if( !mn.at( k ).isNumber() ) { err = el + " range \"min\"[" + std::to_string( k ) + "] is not a number"; return false; }
							for( std::size_t k = 0; k < mx.size(); ++k )
								if( !mx.at( k ).isNumber() ) { err = el + " range \"max\"[" + std::to_string( k ) + "] is not a number"; return false; }
						} else {
							if( !e.has( "value" ) || !e.get( "value" ).isString() ) {
								err = el + " equality entry requires a string \"value\""; return false;
							}
						}
					}
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
			//! "channelBalanceMax" is a number that must be strictly > 1.0 (a ratio
			//! of the brightest to the dimmest mean channel -- 1.0 would demand a
			//! perfectly neutral render, which MC noise makes impossible).
			bool ValidateRenderCheckpointTypes( const JsonValue& cp, std::size_t idx, const std::string& scenarioId, std::string& err )
			{
				static const char* const kNumFields[] = {
					"width", "height", "samples",
					"meanLumaMin", "meanLumaMax",
					"meanRMin", "meanRMax", "meanGMin", "meanGMax", "meanBMin", "meanBMax",
					"channelBalanceMax"
				};
				for( const char* f : kNumFields ) {
					if( !RequireFieldType( cp, f, JsonValue::Type::Number, scenarioId, idx, f, err ) ) return false;
					if( cp.has( f ) && !IsFiniteEvalNumber( cp.get( f ).asNumber() ) ) {
						err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "].\"" +
							f + "\" must be a finite number";
						return false;
					}
				}
				if( cp.has( "channelBalanceMax" ) && cp.get( "channelBalanceMax" ).isNumber() &&
				    cp.get( "channelBalanceMax" ).asNumber() <= 1.0 ) {
					err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
					      "].\"channelBalanceMax\" must be > 1.0 (a ratio of 1.0 is unreachable under MC noise)";
					return false;
				}
				// `width`/`height` (when present) are narrowed via
				// static_cast<unsigned int> by CheckRenderKind -- must be
				// finite whole numbers, bounded the same as Film::
				// kMaxFilmWidth/kMaxFilmHeight (32768, Film.h) reject an
				// absurd raster request rather than allocating gigabytes of
				// frame buffer.
				if( !RequireWholeNumberInRange( cp, "width",  1.0, 32768.0, scenarioId, idx, "width",  err ) ) return false;
				if( !RequireWholeNumberInRange( cp, "height", 1.0, 32768.0, scenarioId, idx, "height", err ) ) return false;
				// Image-reconstruction Wave 2: `samples` (when present) is the
				// grading render's SPP override, narrowed via
				// static_cast<int> by CheckRenderKind -- must be a finite
				// whole count, bounded the same as the [1,65536]
				// samples-override clamp documented in AgentRpc.cpp/
				// AgentMcpAdapter.cpp.
				if( !RequireWholeNumberInRange( cp, "samples", 1.0, 65536.0, scenarioId, idx, "samples", err ) ) return false;

				// Image-reconstruction Wave 2: the `camera` pose override.
				// location+lookat are REQUIRED (each an array of exactly 3
				// numbers); up is OPTIONAL (same shape); fov is OPTIONAL (a
				// positive number strictly < 180 degrees).  ObjectMap camera
				// support is explicitly out of scope here -- this validator is
				// the render kind only.
				if( cp.has( "camera" ) ) {
					const JsonValue& cam = cp.get( "camera" );
					if( !cam.isObject() ) {
						err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
						      "].\"camera\" must be an object (got " + JsonTypeName( cam.type() ) + ")";
						return false;
					}
					auto requireVec3 = [&]( const char* key, bool required ) -> bool {
						if( !cam.has( key ) ) {
							if( required ) {
								err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
								      "].\"camera." + key + "\" is required (an array of 3 numbers)";
								return false;
							}
							return true;
						}
						const JsonValue& a = cam.get( key );
						if( !a.isArray() || a.size() != 3 ) {
							err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
							      "].\"camera." + key + "\" must be an array of exactly 3 numbers";
							return false;
						}
						for( std::size_t i = 0; i < 3; ++i ) {
							if( !a.at( i ).isNumber() ) {
								err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
								      "].\"camera." + key + "\"[" + std::to_string( i ) + "] must be a number";
								return false;
							}
							if( !IsFiniteEvalNumber( a.at( i ).asNumber() ) ) {
								err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
									"].\"camera." + key + "\"[" + std::to_string( i ) + "] must be a finite number";
								return false;
							}
						}
						return true;
					};
					if( !requireVec3( "location", true ) ) return false;
					if( !requireVec3( "lookat", true ) ) return false;
					if( !requireVec3( "up", false ) ) return false;
					if( cam.has( "fov" ) ) {
						if( !cam.get( "fov" ).isNumber() ) {
							err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
							      "].\"camera.fov\" must be a number (degrees)";
							return false;
						}
						const double fov = cam.get( "fov" ).asNumber();
					if( !IsFiniteEvalNumber( fov ) || !( fov > 0.0 && fov < 180.0 ) ) {
							err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
							      "].\"camera.fov\" must be > 0 and < 180 degrees";
							return false;
						}
					}
				}

				// Image-reconstruction Wave 2: compareToImage + rmseMax.  Both
				// or neither.  The path is a non-empty repo-relative string that
				// must not contain ".." (same containment rule the prompt
				// reference-images use); rmseMax is a number in (0,1].  The
				// path's existence / PNG-ness / dims are checked at RUNTIME
				// (CheckRenderKind) -- the load validator cannot open the file.
				const bool hasCompare = cp.has( "compareToImage" );
				const bool hasRmse    = cp.has( "rmseMax" );
				if( hasCompare != hasRmse ) {
					err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
					      "].\"compareToImage\" and \"rmseMax\" must be supplied together (both or neither)";
					return false;
				}
				if( hasCompare ) {
					if( !cp.get( "compareToImage" ).isString() || cp.get( "compareToImage" ).asString().empty() ) {
						err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
						      "].\"compareToImage\" must be a non-empty repo-relative .png path string";
						return false;
					}
					if( cp.get( "compareToImage" ).asString().find( ".." ) != std::string::npos ) {
						err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
						      "].\"compareToImage\" must not contain \"..\" (got '" +
						      cp.get( "compareToImage" ).asString() + "')";
						return false;
					}
					if( !cp.get( "rmseMax" ).isNumber() ) {
						err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
						      "].\"rmseMax\" must be a number";
						return false;
					}
					const double rmseMax = cp.get( "rmseMax" ).asNumber();
					if( !IsFiniteEvalNumber( rmseMax ) || !( rmseMax > 0.0 && rmseMax <= 1.0 ) ) {
						err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
						      "].\"rmseMax\" must be in (0,1]";
						return false;
					}
				}
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
					if( !RequireFieldType( qa, "expectGeometryKind", JsonValue::Type::String, scenarioId, idx, "queryAt.expectGeometryKind", err ) ) return false;
					// expectGeometryKind binds a HIT object's geometry to a chunk
					// kind; it is meaningless when the query expects a MISS
					// (expectName ""), so reject that combination at load.
					if( qa.has( "expectGeometryKind" ) ) {
						if( !qa.get( "expectGeometryKind" ).isString() || qa.get( "expectGeometryKind" ).asString().empty() ) {
							err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
							      "].\"queryAt.expectGeometryKind\" must be a non-empty string";
							return false;
						}
						if( qa.has( "expectName" ) && qa.get( "expectName" ).isString() && qa.get( "expectName" ).asString().empty() ) {
							err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
							      "].\"queryAt.expectGeometryKind\" is meaningless with expectName \"\" (expect-miss)";
							return false;
						}
					}
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
			//!  toolOutcomes?:[{name:string, occurrence?:string, expect:string, argsContains?:string|[string]},...],
			//!  toolCallAfterUserTurn?: {name:string, minUserTurns?:number,
			//!    maxUserTurns?:number, argsContains?:string|[string], expect?:string}
			//!    -- OR an ARRAY of such spec objects,
			//!  askUserMin?/askUserMax?:number (whole, >= 0),
			//!  askUserBeforeMutation?:bool}.  toolOutcomes.expect (and
			//!  the optional toolCallAfterUserTurn.expect) is one of
			//!  applied|staged|rejected|error|conflict; argsContains (when
			//!  present) is a non-empty string OR a non-empty array of non-empty
			//!  strings, ALL of which must be substrings of the record's args --
			//!  it FILTERS which same-named records the spec selects among.  A
			//!  toolCallAfterUserTurn spec must carry at least one of min/max (a
			//!  spec with neither asserts nothing).  askUserMin/askUserMax count
			//!  "ask_user" tool records in the trajectory (stage 2 of the
			//!  clarifying-questions feature); askUserBeforeMutation asserts the
			//!  FIRST ask_user record precedes the FIRST document-mutating tool
			//!  record (insert_chunk/insert_chunks/propose_patch/remove_chunk).
			bool ValidateTrajectoryCheckpointTypes( const JsonValue& cp, std::size_t idx, const std::string& scenarioId, std::string& err )
			{
				if( !RequireFieldType( cp, "maxToolCalls", JsonValue::Type::Number, scenarioId, idx, "maxToolCalls", err ) ) return false;
				if( !RequireFieldType( cp, "maxLlmCalls", JsonValue::Type::Number, scenarioId, idx, "maxLlmCalls", err ) ) return false;
				if( !RequireFieldType( cp, "terminalStatus", JsonValue::Type::String, scenarioId, idx, "terminalStatus", err ) ) return false;
				if( !RequireFieldType( cp, "noAutonomyRefusal", JsonValue::Type::Bool, scenarioId, idx, "noAutonomyRefusal", err ) ) return false;
				if( !RequireFieldType( cp, "noMechanicalLoop", JsonValue::Type::Bool, scenarioId, idx, "noMechanicalLoop", err ) ) return false;
				if( !RequireFieldType( cp, "expectAutonomyRefusal", JsonValue::Type::Bool, scenarioId, idx, "expectAutonomyRefusal", err ) ) return false;
				if( !RequireArrayOfType( cp, "requiredToolInOrder", JsonValue::Type::String, scenarioId, idx, "requiredToolInOrder", err ) ) return false;
				// askUserMin/askUserMax: whole numbers >= 0 (a call COUNT can
				// never be negative or fractional).  askUserBeforeMutation: bool.
				if( !RequireFieldType( cp, "askUserMin", JsonValue::Type::Number, scenarioId, idx, "askUserMin", err ) ) return false;
				if( !RequireWholeNumberInRange( cp, "askUserMin", 0.0, static_cast<double>( std::numeric_limits<int>::max() ),
					scenarioId, idx, "askUserMin", err ) ) return false;
				if( !RequireFieldType( cp, "askUserMax", JsonValue::Type::Number, scenarioId, idx, "askUserMax", err ) ) return false;
				if( !RequireWholeNumberInRange( cp, "askUserMax", 0.0, static_cast<double>( std::numeric_limits<int>::max() ),
					scenarioId, idx, "askUserMax", err ) ) return false;
				if( !RequireFieldType( cp, "askUserBeforeMutation", JsonValue::Type::Bool, scenarioId, idx, "askUserBeforeMutation", err ) ) return false;

				// Shared arg/expect validators so toolOutcomes and
				// toolCallAfterUserTurn can never diverge.  argsContains accepts
				// a non-empty string OR a non-empty array of non-empty strings.
				auto validateArgsContains = [&]( const JsonValue& obj, const std::string& label ) -> bool {
					if( !obj.has( "argsContains" ) ) return true;
					const JsonValue& ac = obj.get( "argsContains" );
					if( ac.isString() ) {
						if( ac.asString().empty() ) {
							err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "]." + label +
							      ".argsContains must be a non-empty string";
							return false;
						}
						return true;
					}
					if( ac.isArray() ) {
						if( ac.size() == 0 ) {
							err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "]." + label +
							      ".argsContains array must be non-empty";
							return false;
						}
						for( std::size_t i = 0; i < ac.size(); ++i ) {
							if( !ac.at( i ).isString() || ac.at( i ).asString().empty() ) {
								err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "]." + label +
								      ".argsContains[" + std::to_string( i ) + "] must be a non-empty string";
								return false;
							}
						}
						return true;
					}
					err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "]." + label +
					      ".argsContains must be a string or an array of strings";
					return false;
				};
				auto validExpectValue = []( const std::string& e ) -> bool {
					return e == "applied" || e == "staged" || e == "rejected" || e == "error" || e == "conflict";
				};

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
						if( expect != "applied" && expect != "staged" && expect != "rejected" && expect != "error" && expect != "conflict" ) {
							err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "]." + label +
							      ".expect must be one of applied|staged|rejected|error|conflict (got '" + expect + "')";
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
						// argsContains is OPTIONAL; when present it must be a
						// non-empty string OR a non-empty array of non-empty
						// strings (an empty needle matches every record, asserting
						// nothing -- reject rather than silently no-op).
						if( !validateArgsContains( o, label ) ) return false;
					}
				}

				if( cp.has( "toolCallAfterUserTurn" ) ) {
					const JsonValue& tc = cp.get( "toolCallAfterUserTurn" );
					// Back-compat: a single OBJECT is the original one-spec form; an
					// ARRAY carries several specs (each validated identically).
					if( !tc.isObject() && !tc.isArray() ) {
						err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
						      "].\"toolCallAfterUserTurn\" must be an object or an array of objects (got " +
						      JsonTypeName( tc.type() ) + ")";
						return false;
					}
					// One shared per-spec validator so the object and array forms
					// can never diverge.  `specLabel` names the offending spec.
					auto validateTcSpec = [&]( const JsonValue& sp, const std::string& specLabel ) -> bool {
						if( !sp.isObject() ) {
							err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "]." + specLabel +
							      " must be an object (got " + JsonTypeName( sp.type() ) + ")";
							return false;
						}
						// name is load-bearing -- REQUIRED (a missing name never matches).
						if( !sp.has( "name" ) || !sp.get( "name" ).isString() || sp.get( "name" ).asString().empty() ) {
							err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "]." + specLabel +
							      ".name must be a non-empty string";
							return false;
						}
						// min/max are each OPTIONAL numbers >= 0, but AT LEAST ONE is
						// required -- a spec with neither asserts nothing.
						auto checkTurnBound = [&]( const char* key ) -> bool {
							if( !sp.has( key ) ) return true;
							if( !sp.get( key ).isNumber() ) {
								err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "]." + specLabel +
								      "." + key + " must be a number";
								return false;
							}
							if( sp.get( key ).asNumber() < 0.0 ) {
								err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "]." + specLabel +
								      "." + key + " must be >= 0";
								return false;
							}
							return true;
						};
						if( !checkTurnBound( "minUserTurns" ) ) return false;
						if( !checkTurnBound( "maxUserTurns" ) ) return false;
						if( !sp.has( "minUserTurns" ) && !sp.has( "maxUserTurns" ) ) {
							err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "]." + specLabel +
							      " must carry at least one of \"minUserTurns\"/\"maxUserTurns\" (a spec with neither asserts nothing)";
							return false;
						}
						// argsContains accepts a non-empty string OR a non-empty
						// array of non-empty strings.
						if( !validateArgsContains( sp, specLabel ) ) return false;
						// expect is OPTIONAL; when present the matching call must ALSO
						// satisfy this outcome (same enum as toolOutcomes).
						if( sp.has( "expect" ) ) {
							if( !sp.get( "expect" ).isString() || !validExpectValue( sp.get( "expect" ).asString() ) ) {
								err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "]." + specLabel +
								      ".expect must be one of applied|staged|rejected|error|conflict";
								return false;
							}
						}
						return true;
					};

					if( tc.isObject() ) {
						if( !validateTcSpec( tc, "toolCallAfterUserTurn" ) ) return false;
					} else {
						if( tc.size() == 0 ) {
							err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
							      "].\"toolCallAfterUserTurn\" must be a NON-EMPTY array (an empty array asserts nothing)";
							return false;
						}
						for( std::size_t i = 0; i < tc.size(); ++i )
							if( !validateTcSpec( tc.at( i ), "toolCallAfterUserTurn[" + std::to_string( i ) + "]" ) ) return false;
					}
				}

				return true;
			}

			//! "proposal" (mirrors CheckProposalKind):
			//! {countMin?:number, countMax?:number, match?:{kindIs?:string,
			//!  target?:string, param?:string, status?:string,
			//!  valueMin?:number array, valueMax?:number array}} -- at least ONE
			//! of countMin/countMax/match is required (a checkpoint with none is
			//! vacuous).
			bool ValidateProposalCheckpointTypes( const JsonValue& cp, std::size_t idx, const std::string& scenarioId, std::string& err )
			{
				if( !RequireFieldType( cp, "countMin", JsonValue::Type::Number, scenarioId, idx, "countMin", err ) ) return false;
				if( !RequireFieldType( cp, "countMax", JsonValue::Type::Number, scenarioId, idx, "countMax", err ) ) return false;
				if( !cp.has( "countMin" ) && !cp.has( "countMax" ) && !cp.has( "match" ) ) {
					err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) +
					      "] \"proposal\" checkpoint must carry at least one of countMin/countMax/match (a checkpoint with none asserts nothing)";
					return false;
				}
				if( cp.has( "match" ) ) {
					const JsonValue& m = cp.get( "match" );
					if( !m.isObject() ) {
						err = "scenario '" + scenarioId + "': checkpoints[" + std::to_string( idx ) + "].\"match\" must be an object (got " +
						      JsonTypeName( m.type() ) + ")";
						return false;
					}
					static const char* const kMatchStrings[] = { "kindIs", "target", "param", "status" };
					for( const char* f : kMatchStrings )
						if( !RequireFieldType( m, f, JsonValue::Type::String, scenarioId, idx, std::string( "match." ) + f, err ) ) return false;
					if( !RequireArrayOfType( m, "valueMin", JsonValue::Type::Number, scenarioId, idx, "match.valueMin", err ) ) return false;
					if( !RequireArrayOfType( m, "valueMax", JsonValue::Type::Number, scenarioId, idx, "match.valueMax", err ) ) return false;
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
				if( kind == "proposal" )    return ValidateProposalCheckpointTypes( cp, idx, scenarioId, err );
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
			out.sourcePath = path;

			// Review-round P2 + Finding-1a hardening (report-artifact-discovery
			// reversibility): the id is concatenated into filesystem paths
			// (temp scene, trajectory, result) -- it MUST be a bare token so a
			// scenario can never write outside runDir, AND it must round-trip
			// through SanitizeForPath (Section 3c) bit-for-bit so a reporter
			// reconstructing "<id>.result.jsonl" from a run-dir leaf's parsed
			// scenario-id field can never miss the raw file on disk.  Three
			// loud rules:
			//   (i)   no '/', '\\', or ".." (path traversal / escape) -- mirrors
			//         the read_skill bare-name rule (AgentRpc.h: '/', '\\', ".."
			//         rejected);
			//   (ii)  every character must be one SanitizeForPath itself LEAVES
			//         UNTOUCHED ([A-Za-z0-9._-]) -- so SanitizeForPath(id) == id
			//         ALWAYS (a raw id with, say, a ':' sanitizes to a run-dir
			//         leaf that no longer matches the raw "<id>.result.jsonl"
			//         filename);
			//   (iii) the id must not contain "__" -- RunEvalMatrix's run-dir
			//         leaf is "<id>__<provider>[__<model>]__r<repeat>"
			//         (SanitizeForPath is applied per-field, not to the whole
			//         leaf); an id containing "__" makes that leaf ambiguous to
			//         parse back apart into its fields.
			// The scenario JSON is dev-committed today, but this closes the
			// hole before E4's live runner or any less-trusted scenario source.
			// Every committed scenario id (param_edit, reserved_name_recovery,
			// ... single underscores only) already satisfies all three.
			if( out.id.find( '/' ) != std::string::npos ||
			    out.id.find( '\\' ) != std::string::npos ||
			    out.id.find( ".." ) != std::string::npos ) {
				err = "scenario id '" + out.id + "' must be a BARE token -- no '/', '\\\\', or \"..\" "
				      "(it is used to build filesystem paths under the run dir)";
				return false;
			}
			for( std::size_t i = 0; i < out.id.size(); ++i ) {
				const unsigned char c = static_cast<unsigned char>( out.id[i] );
				if( !IsSanitizePreservedChar( out.id[i] ) ) {
					err = "scenario id '" + out.id + "' must consist only of [A-Za-z0-9._-] -- "
					      "exactly the character set SanitizeForPath leaves untouched, so the raw "
					      "id and its run-dir leaf are always identical (found disallowed character '" +
					      std::string( 1, static_cast<char>( c ) ) + "' at index " + std::to_string( i ) + ")";
					return false;
				}
			}
			if( out.id.find( "__" ) != std::string::npos ) {
				err = "scenario id '" + out.id + "' must not contain \"__\" -- that is the run-dir "
				      "field separator (\"<id>__<provider>[__<model>]__r<repeat>\"); an id containing "
				      "it makes the run-dir leaf ambiguous to parse back apart";
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
					const JsonValue& p = prompts.at( i );
					AgentEvalPrompt prompt;
					if( p.isString() ) {
						// The pre-Wave-1 shape: a bare string, unchanged.
						prompt.text = p.asString();
					} else if( p.isObject() ) {
						// Image-reconstruction Wave 1: {"text"?, "images"?}.
						if( p.has( "text" ) ) {
							if( !p.get( "text" ).isString() ) {
								err = "scenario '" + out.id + "': prompts[" + std::to_string( i ) +
									"].text must be a string";
								return false;
							}
							prompt.text = p.get( "text" ).asString();
						}
						if( p.has( "images" ) ) {
							const JsonValue& images = p.get( "images" );
							if( !images.isArray() || images.size() == 0 ) {
								err = "scenario '" + out.id + "': prompts[" + std::to_string( i ) +
									"].images must be a non-empty array of repo-relative path strings";
								return false;
							}
							for( std::size_t j = 0; j < images.size(); ++j ) {
								if( !images.at( j ).isString() || images.at( j ).asString().empty() ) {
									err = "scenario '" + out.id + "': prompts[" + std::to_string( i ) +
										"].images[" + std::to_string( j ) + "] must be a non-empty string";
									return false;
								}
								const std::string imgPath = images.at( j ).asString();
								if( imgPath.find( ".." ) != std::string::npos ) {
									err = "scenario '" + out.id + "': prompts[" + std::to_string( i ) +
										"].images[" + std::to_string( j ) + "] must not contain \"..\" (got '" +
										imgPath + "')";
									return false;
								}
								prompt.imagePaths.push_back( imgPath );
							}
						}
						if( prompt.text.empty() && prompt.imagePaths.empty() ) {
							err = "scenario '" + out.id + "': prompts[" + std::to_string( i ) +
								"] object must carry a non-empty \"text\", a non-empty \"images\", or both";
							return false;
						}
					} else {
						err = "scenario '" + out.id + "': prompts[" + std::to_string( i ) +
							"] must be a string or an object {text?, images?}";
						return false;
					}
					out.prompts.push_back( prompt );
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
					long long value = 0;
					if( !ParseScenarioWholeNumber( b.get( "maxToolCalls" ),
						"scenario '" + out.id + "': budgets.maxToolCalls", -1,
						std::numeric_limits<int>::max(), value, err ) ) return false;
					out.budgets.maxToolCalls = static_cast<int>( value );
				}
				if( b.has( "maxLlmCalls" ) ) {
					long long value = 0;
					if( !ParseScenarioWholeNumber( b.get( "maxLlmCalls" ),
						"scenario '" + out.id + "': budgets.maxLlmCalls", -1,
						std::numeric_limits<int>::max(), value, err ) ) return false;
					out.budgets.maxLlmCalls = static_cast<int>( value );
				}
				if( b.has( "maxWallMs" ) ) {
					long long value = 0;
					// Keep a margin below LLONG_MAX because a double at that
					// magnitude rounds to 2^63, which is outside long long.
					if( !ParseScenarioWholeNumber( b.get( "maxWallMs" ),
						"scenario '" + out.id + "': budgets.maxWallMs", -1,
						9000000000000000000LL, value, err ) ) return false;
					out.budgets.maxWallMs = value;
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
					if( !e.has( "afterToolCalls" ) ) {
						err = "scenario '" + out.id + "': interventions[" + idx +
							"] missing required number field \"afterToolCalls\"";
						return false;
					}
					long long afterToolCalls = 0;
					if( !ParseScenarioWholeNumber( e.get( "afterToolCalls" ),
						"scenario '" + out.id + "': interventions[" + idx + "].afterToolCalls",
						1, std::numeric_limits<int>::max(), afterToolCalls, err ) ) return false;
					it.afterToolCalls = static_cast<int>( afterToolCalls );
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

			// askUserResponses: OPTIONAL scripted responder for the
			// ask_user chat tool (stage 2 of the clarifying-questions
			// feature -- see AgentEvalAskUserResponses).  Validated LOUDLY
			// here so a malformed responder is a load_error, never a
			// silently-skipped no-op that leaves the stage-1 stub in
			// place unbeknownst to the scenario author.
			out.askUserResponses = AgentEvalAskUserResponses();
			if( root.has( "askUserResponses" ) ) {
				if( !root.get( "askUserResponses" ).isObject() ) {
					err = "scenario '" + out.id + "': \"askUserResponses\" must be an object";
					return false;
				}
				const JsonValue& aur = root.get( "askUserResponses" );
				if( aur.has( "matches" ) ) {
					if( !aur.get( "matches" ).isArray() ) {
						err = "scenario '" + out.id + "': askUserResponses.matches must be an array";
						return false;
					}
					const JsonValue& matches = aur.get( "matches" );
					for( std::size_t i = 0; i < matches.size(); ++i ) {
						const std::string idx = std::to_string( i );
						const JsonValue& m = matches.at( i );
						if( !m.isObject() ) {
							err = "scenario '" + out.id + "': askUserResponses.matches[" + idx + "] must be an object";
							return false;
						}
						if( !m.has( "contains" ) || !m.get( "contains" ).isString() ||
						    m.get( "contains" ).asString().empty() ) {
							err = "scenario '" + out.id + "': askUserResponses.matches[" + idx +
								"].contains must be a non-empty string";
							return false;
						}
						if( !m.has( "answer" ) || !m.get( "answer" ).isString() ||
						    m.get( "answer" ).asString().empty() ) {
							err = "scenario '" + out.id + "': askUserResponses.matches[" + idx +
								"].answer must be a non-empty string";
							return false;
						}
						AgentEvalAskUserMatch am;
						am.contains = m.get( "contains" ).asString();
						am.answer   = m.get( "answer" ).asString();
						out.askUserResponses.matches.push_back( am );
					}
				}
				if( aur.has( "default" ) ) {
					if( !aur.get( "default" ).isString() || aur.get( "default" ).asString().empty() ) {
						err = "scenario '" + out.id + "': askUserResponses.default must be a non-empty string";
						return false;
					}
					out.askUserResponses.hasDefault    = true;
					out.askUserResponses.defaultAnswer = aur.get( "default" ).asString();
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

			//! Inverse of ParseReplayProviderName -- the canonical provider NAME
			//! string for a ChatProvider enum value, as written into a
			//! result.jsonl's "provider" field (see RunScenarioDriven's
			//! result-summary write) and consumed by tools/eval_report.py, which
			//! PREFERS this true, un-sanitized field over its lossy dir-name
			//! parse (a SanitizeForPath leaf can collapse two distinct model ids
			//! to the same fragment -- see the P2 collision finding).
			std::string ChatProviderName( ChatProvider p )
			{
				switch( p ) {
					case ChatProvider::Anthropic: return "anthropic";
					case ChatProvider::Gemini:    return "gemini";
					case ChatProvider::OpenAI:    return "openai";
					case ChatProvider::XAI:       return "xai";
					case ChatProvider::Local:     return "local";
				}
				return "";   // unreachable for a valid enum value; keeps -Wreturn-type quiet on all compilers
			}

			//! Shared FNV-1a 64-bit hex digest over an arbitrary byte string (16
			//! lowercase hex chars, zero-padded).  Factored out of
			//! ScenarioContentHash so the SAME implementation also backs
			//! FnvHexOfFile below -- the raw-file-byte hashes stamped into
			//! result.jsonl ("scenarioFileFnv" / "sceneFileFnv") that
			//! tools/eval_report.py recomputes independently in Python
			//! (fnv1a64_hex) to close the equal-count-edit blind spot a bare
			//! semantic ScenarioContentHash can't: the two implementations are
			//! pinned bit-identical against the FNV-1a 64 of "hello".
			std::string FnvHex( const std::string& bytes )
			{
				std::uint64_t hash = 14695981039346656037ull;
				const std::uint64_t prime = 1099511628211ull;
				for( char ch : bytes ) {
					hash ^= static_cast<unsigned char>( ch );
					hash *= prime;
				}
				char buf[17];
				std::snprintf( buf, sizeof( buf ), "%016llx", static_cast<unsigned long long>( hash ) );
				return std::string( buf );
			}

			//! FnvHex of a file's raw bytes (binary read).  Returns "" when the
			//! file cannot be opened -- callers treat that as "not applicable /
			//! unknown" and OMIT the stamped key entirely rather than writing a
			//! misleading empty hash (see RunScenarioDriven's result-summary
			//! write, and eval_report.py's check_staleness, which skips the
			//! comparison when the key is absent).
			std::string FnvHexOfFile( const std::string& path )
			{
				std::ifstream f( path.c_str(), std::ios::binary );
				if( !f ) return std::string();
				std::ostringstream ss;
				ss << f.rdbuf();
				return FnvHex( ss.str() );
			}

			//! Methodology epoch, folded into ScenarioContentHash. BUMP THIS by one whenever
			//! a harness-side change alters how runs are DRIVEN or GRADED without touching
			//! any scenario file -- a system-prompt change, a tool-definition change, a
			//! checker-semantics change, a drive-loop behavior change. Bumping invalidates
			//! every cached cell in every runDir on the next matrix invocation (they re-run
			//! under the new methodology), preventing one runDir from silently mixing
			//! results produced under two methodologies. Scenario-file changes do NOT need
			//! this -- the hash already covers them.
			//! Epoch history:
			//!   1 -> initial.
			//!   2 -> (2026-07-19) compare_to_reference gained the `split`
			//!        parameter (a TOOL-DEFINITION change) and
			//!        skills/agent/modeling-from-image-captures.md turned its
			//!        plateau guidance into a directive rule with a mechanical
			//!        trigger (a DRIVE-LOOP change).  Note the skill file's
			//!        bytes are NOT covered by ScenarioContentHash -- only
			//!        scenario files are -- so a skill edit is precisely the
			//!        "alters how runs are DRIVEN without touching any
			//!        scenario file" case this epoch exists for.  Without the
			//!        bump the pre-split vision cells stay hash-current and
			//!        get silently reused, so a re-run would measure nothing
			//!        and the board would mix two methodologies.
			//!   3 -> (2026-07-19) compare_to_reference gained `splitObjects`
			//!        (tool-definition change) and the skill now directs the
			//!        model to SCOPE the split to its hero object
			//!        (drive-loop change).  Motivated by the epoch-2 gpt run:
			//!        unscoped, a candidate's ground plane and backdrop are
			//!        registered objects, so they landed in the OBJECT bucket
			//!        and objectPixelFraction averaged 0.86 -- the split was
			//!        reporting geometry-vs-environment, not
			//!        object-vs-staging, making the skill's own read of it
			//!        wrong.  Epoch-2 cells were driven under that broken
			//!        reading and must not be compared against scoped runs.
			//!   4 -> (2026-07-19) the skill's split guidance was SOFTENED from
			//!        a mandate ("from your THIRD compare onward") back to an
			//!        occasional, opt-in diagnostic (drive-loop change).  A
			//!        controlled A/B (epoch 3, gpt, 2 scenarios x 3 repeats x
			//!        2 arms, arms differing ONLY in the skill file) measured
			//!        the mandate's effect on final RMSE at -0.0004, 95% CI
			//!        [-0.050, +0.050] -- nothing -- while it pushed 5 of 6
			//!        runs into budget exhaustion vs 1 of 6 without it, since
			//!        each split costs an extra render and calls are the
			//!        binding constraint.  Epoch-3 cells were driven under the
			//!        mandate and are not comparable to softened-guidance runs.
			//!   5 -> (2026-07-20) the blind-edit nudge (AgentChatLoop appends a
			//!        one-shot "you have edited N times without rendering"
			//!        system-prompt reminder, default on) is a DRIVE-LOOP
			//!        change, and the general modeling skill gained a
			//!        "don't build blind / place the hero at the fixed
			//!        camera's focal point" lesson.  Both alter how a build
			//!        turn is driven; cells recorded before them are not
			//!        comparable.  (The skill file's bytes are not covered by
			//!        ScenarioContentHash, so the epoch is the only thing that
			//!        invalidates prior cells on a skill edit -- see epoch 2.)
			//!   6 -> (2026-07-22) lighting arc.  The scene-build render-band
			//!        channelBalanceMax was recalibrated 3.0 -> 4.0 (a
			//!        scenario-file change, so already hash-invalidating) after
			//!        rendered calibration showed 3.0 unfairly failed
			//!        legitimately-warm rooms the prompt ASKS for (a good warm
			//!        scene renders at ratio ~1.4-2.9; only no-fill crush
			//!        exceeds ~5), and the lighting skill gained a "warm key
			//!        alone crushes to orange -- always add a fill" lesson (a
			//!        drive change whose skill bytes are NOT in the hash, hence
			//!        this bump).  Prior cells were graded/driven under the old
			//!        threshold + skill and are not comparable.
			//!   7 -> (2026-07-22) the batch `insert_chunks` verb shipped (a
			//!        tool-definition change -- models can now add a coherent
			//!        group of chunks in ONE call instead of N), and the
			//!        modeling skill's observe-loop section was rewritten from
			//!        a prescriptive "render every iteration" cadence to a
			//!        scenario-based "render to answer a question" framing (a
			//!        drive change; skill bytes are not in the hash).  Both
			//!        change how a build turn is driven; prior cells are not
			//!        comparable.
			//!   8 -> (2026-07-22) the modeling skill's observe-loop section
			//!        elevated the LIGHTING render to a "must not skip" case
			//!        (epoch-7's scenario-based cadence cut renders 6.9->2.8 on
			//!        gpt and the sole quality cost was the lighting checkpoint
			//!        -- fewer renders, less lighting correction).  A drive
			//!        change; prior cells are not comparable.
			//!   9 -> (2026-07-22) RESTORED the regular render cadence in the
			//!        modeling skill while KEEPING batch insert_chunks.  The
			//!        epoch-7/8 data showed chattiness was dominated by INSERTS
			//!        (fixed by the batch verb), not renders; the render
			//!        softening had cut renders 6.9->2.3 on gpt and cost the
			//!        lighting checkpoint for a lever that was never the cost
			//!        problem.  Skill now says "build in coherent groups (batch),
			//!        then render after each group and always after lighting."
			//!        A drive change; prior cells are not comparable.
			//!   10 -> (2026-07-23) the `ask_user` chat tool shipped (stage 1a of
			//!        the clarifying-questions feature): models now see a
			//!        THIRTEENTH tool in every provider request and may spend a
			//!        round calling it instead of building.  The eval runner
			//!        intercepts ask_user before dispatch and always answers
			//!        available:false (no interactive user in a headless run),
			//!        so a model that asks pays a real tool-call round-trip for
			//!        no scene progress -- a tool-DEFINITION change, not a drive
			//!        or grading change, but it alters how a run is driven all
			//!        the same (a model may now choose to ask rather than act).
			//!        Prior cells were driven against a twelve-tool table and
			//!        are not comparable.
			//!   11 -> (2026-07-23) stage 2 of the clarifying-questions feature:
			//!        the eval runner's ask_user interception now consults an
			//!        OPTIONAL scripted responder (askUserResponses) before
			//!        falling back to the stage-1 available:false stub, and the
			//!        "trajectory" checkpoint kind gained three new assertion
			//!        fields (askUserMin/askUserMax/askUserBeforeMutation) --
			//!        both a DRIVE change (a scripted answer changes what the
			//!        model does next, unlike the stage-1 stub which always told
			//!        it to proceed unassisted) and a CHECKER-SEMANTICS change
			//!        (a scenario grading on ask_user counts/ordering did not
			//!        exist before this epoch).  Prior cells were driven and
			//!        graded without a scripted responder or the new trajectory
			//!        fields and are not comparable.
			static const int kEvalMethodologyEpoch = 11;

			//! A deterministic content hash of the parts of a scenario that
			//! determine how a run is DRIVEN and GRADED: autonomy, prompts,
			//! budgets, the scene definition (including a path-backed scene's
			//! file BYTES, not just its path), the checkpoints[], the
			//! interventions[], and the harness-side kEvalMethodologyEpoch.
			//! Two scenario definitions with the same hash produce the same
			//! run + the same grading, so a cached matrix cell is safe to
			//! reuse; a different hash means the oracle (or the run, or the
			//! harness methodology) changed and the cell is STALE (the
			//! cross-invocation resume guard wipes + re-runs it).  Stamped
			//! into each result.jsonl (see RunScenarioDriven) so a later
			//! RunEvalMatrix invocation into the SAME runDir -- and the Python
			//! reporter -- can tell "graded under the current oracle" from
			//! "stale cell, re-run me".
			//!
			//! v2 (this canonicalization) deliberately changes the hash for
			//! EVERY scenario relative to v1 (interventions + methodology
			//! epoch are new inputs; path-backed scenes now fold in file
			//! bytes) -- every v1-stamped cell in an existing runDir is
			//! therefore correctly treated as stale and re-run under v2. v1
			//! was computed under an incomplete canonicalization (it missed
			//! interventions entirely and hashed a path-backed scene's
			//! PATH STRING rather than its bytes, so editing the scene file
			//! in place or the intervention list left a cell wrongly marked
			//! "current"); that was the bug this version fixes.
			//!
			//! P1-1 (still v2): the checkpoints= line serializes the raw
			//! checkpoint JSON, and for a "render" checkpoint that carries
			//! "compareToImage" that JSON is only the reference PNG's PATH
			//! STRING -- editing evals/references/*.png in place left a cached
			//! cell wrongly "current", the same bug class v1 had for
			//! path-backed scenes.  A conditional "compareImages=" section
			//! (below, mirroring the prompts= image-hashing) folds in each
			//! distinct compareToImage path's FnvHexOfFile.  It is appended
			//! ONLY when at least one render checkpoint carries compareToImage,
			//! so every scenario without one hashes byte-identically to before
			//! this fix -- no version bump, no blanket cache invalidation; only
			//! compareToImage-bearing scenarios get a new hash.
			std::string ScenarioContentHash( const AgentEvalScenario& s )
			{
				std::string canon = "v2\n";
				canon += "epoch=" + std::to_string( kEvalMethodologyEpoch ) + "\n";
				canon += "autonomy=" + s.autonomy + "\n";
				// Prompts serialized as a JSON array (like checkpoints below) so the
				// canonicalization is INJECTIVE: JSON escaping keeps a prompt that
				// itself contains a delimiter or newline from straddling the field
				// boundary or colliding ["a\x1fb"] with ["a","b"].
				//
				// Image-reconstruction Wave 1: a TEXT-ONLY prompt (imagePaths
				// empty) still serializes as a bare JSON string -- BYTE-IDENTICAL
				// to the pre-Wave-1 canonicalization, so no existing scenario's
				// hash (and therefore no cached matrix cell) changes.  An
				// image-bearing prompt serializes as an object instead, folding
				// each referenced image's BYTES (via FnvHexOfFile, "unreadable"
				// on an unreadable path -- deterministic either way) into the
				// hash, so editing a reference image in place invalidates cached
				// cells exactly like the path-backed scene-bytes rule below.
				JsonValue promptsArr = JsonValue::MakeArray();
				for( std::size_t i = 0; i < s.prompts.size(); ++i ) {
					const AgentEvalPrompt& prompt = s.prompts[i];
					if( prompt.imagePaths.empty() ) {
						promptsArr.push_back( JsonValue::MakeString( prompt.text ) );
					} else {
						JsonValue o = JsonValue::MakeObject();
						o.set( "text", JsonValue::MakeString( prompt.text ) );
						JsonValue imgs = JsonValue::MakeArray();
						for( std::size_t j = 0; j < prompt.imagePaths.size(); ++j ) {
							JsonValue io = JsonValue::MakeObject();
							io.set( "path", JsonValue::MakeString( prompt.imagePaths[j] ) );
							const std::string h = FnvHexOfFile( prompt.imagePaths[j] );
							io.set( "fnv", JsonValue::MakeString( h.empty() ? std::string( "unreadable" ) : h ) );
							imgs.push_back( io );
						}
						o.set( "images", imgs );
						promptsArr.push_back( o );
					}
				}
				canon += "prompts=" + JsonSerialize( promptsArr ) + "\n";
				canon += "budgets=" + std::to_string( s.budgets.maxToolCalls ) + "|" +
				         std::to_string( s.budgets.maxLlmCalls ) + "|" +
				         std::to_string( s.budgets.maxWallMs ) + "\n";
				if( s.scenePath.empty() ) {
					canon += "scene=inline:" + s.sceneInline + "\n";
				} else {
					// Hash the file's CONTENTS as well as the path -- either a moved
					// path or edited-in-place contents must invalidate the cell.  On
					// an unreadable path we fall back to a fixed marker string: the
					// run itself will fail with a load_error against an unreadable
					// scene, so this fallback only needs to be DETERMINISTIC, not
					// meaningful.
					std::ifstream sceneFile( s.scenePath, std::ios::binary );
					if( sceneFile ) {
						std::ostringstream ss;
						ss << sceneFile.rdbuf();
						canon += "scene=path:" + s.scenePath + ":bytes:" + ss.str() + "\n";
					} else {
						canon += "scene=path:" + s.scenePath + ":unreadable\n";
					}
				}
				canon += "checkpoints=" + JsonSerialize( s.checkpoints ) + "\n";

				// P1-1: the checkpoints= line above hashes the checkpoint JSON
				// itself, which for a "render" checkpoint's "compareToImage"
				// field only pins the reference image's PATH STRING, not its
				// bytes -- editing evals/references/*.png in place (the same
				// failure mode prompt reference images had pre-Wave-1) left a
				// cached matrix cell wrongly "current".  Mirror the prompts=
				// image-hashing above: walk the render checkpoints in order,
				// fold in FnvHexOfFile of each DISTINCT compareToImage path
				// (first-seen order; a path reused across checkpoints is
				// hashed once), "unreadable" on an unreadable path (matches
				// the promptsArr fallback -- deterministic either way).
				if( s.checkpoints.isArray() ) {
					JsonValue compareImages = JsonValue::MakeArray();
					std::vector<std::string> seenPaths;
					for( std::size_t ci = 0; ci < s.checkpoints.size(); ++ci ) {
						const JsonValue& cp = s.checkpoints.at( ci );
						if( !cp.isObject() ) continue;
						if( !cp.has( "kind" ) || !cp.get( "kind" ).isString() || cp.get( "kind" ).asString() != "render" ) continue;
						if( !cp.has( "compareToImage" ) || !cp.get( "compareToImage" ).isString() ) continue;
						const std::string& path = cp.get( "compareToImage" ).asString();
						if( std::find( seenPaths.begin(), seenPaths.end(), path ) != seenPaths.end() ) continue;
						seenPaths.push_back( path );
						JsonValue o = JsonValue::MakeObject();
						o.set( "path", JsonValue::MakeString( path ) );
						const std::string h = FnvHexOfFile( path );
						o.set( "fnv", JsonValue::MakeString( h.empty() ? std::string( "unreadable" ) : h ) );
						compareImages.push_back( o );
					}
					if( compareImages.size() > 0 )
						canon += "compareImages=" + JsonSerialize( compareImages ) + "\n";
				}

				// Interventions serialized as a JSON array (like prompts/checkpoints
				// above) for the same injectivity reason.
				JsonValue ivArr = JsonValue::MakeArray();
				for( const auto& iv : s.interventions ) {
					JsonValue o = JsonValue::MakeObject();
					o.set( "afterToolCalls", JsonValue::MakeNumber( static_cast<double>( iv.afterToolCalls ) ) );
					o.set( "op", JsonValue::MakeString( iv.op ) );
					o.set( "target", JsonValue::MakeString( iv.target ) );
					o.set( "param", JsonValue::MakeString( iv.param ) );
					o.set( "value", JsonValue::MakeString( iv.value ) );
					ivArr.push_back( o );
				}
				canon += "interventions=" + JsonSerialize( ivArr ) + "\n";

				// askUserResponses (stage 2 of the clarifying-questions
				// feature): folded in unconditionally, mirroring the
				// interventions= line above (never gated on emptiness) --
				// it changes how a run is DRIVEN, since a scenario whose
				// responder answers (or answers DIFFERENTLY) drives a
				// genuinely different session than the stage-1
				// {available:false} stub, so a cached matrix cell graded
				// under a different responder must be treated as stale.
				// "default" is set on the canonical object only when the
				// scenario actually carries one, so present-vs-absent
				// default is distinguishable from a present-but-equal-to-
				// sentinel value.
				{
					JsonValue aurArr = JsonValue::MakeArray();
					for( const auto& m : s.askUserResponses.matches ) {
						JsonValue o = JsonValue::MakeObject();
						o.set( "contains", JsonValue::MakeString( m.contains ) );
						o.set( "answer",   JsonValue::MakeString( m.answer ) );
						aurArr.push_back( o );
					}
					JsonValue aurObj = JsonValue::MakeObject();
					aurObj.set( "matches", aurArr );
					if( s.askUserResponses.hasDefault )
						aurObj.set( "default", JsonValue::MakeString( s.askUserResponses.defaultAnswer ) );
					canon += "askUserResponses=" + JsonSerialize( aurObj ) + "\n";
				}

				return FnvHex( canon );
			}

			//====================================================================
			// HTTP 429 (rate-limit) backoff helpers -- LIVE path only (see the
			// RunScenarioLive doc comment in AgentEvalRunner.h for the full
			// behavioural contract this backs).
			//====================================================================

			//! The hard cap on any single computed backoff wait, per the E4
			//! design: never sleep longer than this for one attempt, no
			//! matter what a provider hints or the exponential table says.
			const int64_t kRateLimitMaxWaitMs = 90000;

			//! The total number of HTTP attempts a round will make while it
			//! keeps seeing 429 before giving up (the original attempt PLUS
			//! same-round retries).
			const int kRateLimitMaxAttempts = 5;

			//! The exponential fallback table (seconds, converted to ms
			//! below), indexed by attempt number (1-based) when the
			//! response body carries no parseable retry hint: 10s, 20s,
			//! 40s, 60s, 60s.
			const int64_t kRateLimitFallbackSecTable[kRateLimitMaxAttempts] = { 10, 20, 40, 60, 60 };

			//! Parse a provider's rate-limit RETRY HINT out of a 429
			//! response body: a "try again in <N>s" or "retry after <N>
			//! seconds" phrase (case-insensitive; N a float).  Returns the
			//! hinted wait in milliseconds (rounded), or -1 if no such
			//! phrase/number is found -- the caller then falls back to the
			//! exponential table.  Deliberately NOT a response-HEADER
			//! parse: ChatHttpResponse (ChatHttpTransport.h) does not
			//! surface response headers to the runner today, so a
			//! `Retry-After` header hint is out of scope for this pass
			//! (plumbing headers through the transport seam would be new
			//! surface, which the design explicitly says to skip rather
			//! than add speculatively).
			int64_t ParseRateLimit429HintMs( const std::string& body )
			{
				std::string lower( body );
				for( char& c : lower ) c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );

				static const char* const kPhrases[] = { "try again in", "retry after" };
				for( std::size_t p = 0; p < sizeof( kPhrases ) / sizeof( kPhrases[0] ); ++p ) {
					std::size_t pos = lower.find( kPhrases[p] );
					if( pos == std::string::npos ) continue;

					std::size_t i = pos + std::strlen( kPhrases[p] );
					while( i < lower.size() && std::isspace( static_cast<unsigned char>( lower[i] ) ) ) ++i;

					const std::size_t numStart = i;
					bool sawDigit = false;
					while( i < lower.size() &&
					       ( std::isdigit( static_cast<unsigned char>( lower[i] ) ) || lower[i] == '.' ) ) {
						if( std::isdigit( static_cast<unsigned char>( lower[i] ) ) ) sawDigit = true;
						++i;
					}
					if( !sawDigit ) continue;   // phrase matched but no number followed -- try the other phrase

					const std::string numTok = body.substr( numStart, i - numStart );
					char* endp = nullptr;
					const double seconds = std::strtod( numTok.c_str(), &endp );
					if( endp == numTok.c_str() || seconds < 0.0 ) continue;

					// Require a trailing "s"/"sec"/"second(s)" marker so an
					// unrelated number that happens to follow the phrase
					// (e.g. an id) is never mistaken for a duration.
					std::size_t j = i;
					while( j < lower.size() && std::isspace( static_cast<unsigned char>( lower[j] ) ) ) ++j;
					if( j < lower.size() && lower[j] == 's' )
						return static_cast<int64_t>( seconds * 1000.0 + 0.5 );
				}
				return -1;
			}

			//! The wait to use for the `attemptNumber`-th (1-based) 429
			//! response of a round: the body hint if present, else the
			//! exponential fallback table (clamped to the table's last
			//! entry past kRateLimitMaxAttempts), capped at
			//! kRateLimitMaxWaitMs either way.
			int64_t ComputeRateLimit429WaitMs( const std::string& body, int attemptNumber )
			{
				int64_t ms = ParseRateLimit429HintMs( body );
				if( ms < 0 ) {
					int idx = attemptNumber - 1;
					if( idx < 0 ) idx = 0;
					if( idx >= kRateLimitMaxAttempts ) idx = kRateLimitMaxAttempts - 1;
					ms = kRateLimitFallbackSecTable[idx] * 1000;
				}
				if( ms > kRateLimitMaxWaitMs ) ms = kRateLimitMaxWaitMs;
				if( ms < 0 ) ms = 0;
				return ms;
			}

			//! The default (non-injected) sleeper: a real wall-clock sleep.
			//! Every AgentEvalLiveRunOptions/AgentEvalMatrixOptions caller
			//! that leaves `sleeper` unset gets this -- only tests inject a
			//! recorder in its place.
			void RealSleepMs( int64_t ms )
			{
				if( ms > 0 ) std::this_thread::sleep_for( std::chrono::milliseconds( ms ) );
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
				const AgentEvalScenario&            scenario,
				const std::string&                  runDir,
				const std::function<int64_t()>&     optClock,
				ChatProvider                        provider,
				const std::string&                  modelId,
				const std::string&                  apiKey,
				const FetchFn&                      fetch,
				const std::function<void(int64_t)>& sleeper )
			{
				AgentEvalRunHandle handle;
				handle.result.scenarioId = scenario.id;

				// The HTTP 429 backoff sleep hook: the caller's injected
				// sleeper (a test recorder), or a real sleep by default.
				// Never invoked by the replay path (RunScenario always
				// synthesizes status 200 -- see replayFetch below).
				const std::function<void(int64_t)> effectiveSleeper = sleeper ? sleeper : &RealSleepMs;

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

				// Image-reconstruction Wave 1: pre-flight-load EVERY prompt's
				// reference images UP FRONT, right after the scene loads and
				// BEFORE any LLM round runs -- an unreadable path or an
				// unsupported image type must never burn a partial run (some
				// prompts' turns already driven, tool calls already dispatched)
				// before the run discovers the broken path.  One entry per
				// scenario.prompts[i], parallel-indexed; empty for a text-only
				// prompt.
				std::vector<std::vector<ChatAttachment>> promptAttachments( scenario.prompts.size() );
				// compare_to_reference naming contract: every prompt-attachment
				// image, in prompt-then-attachment order (the SAME order the
				// promptAttachments loop below walks), is ALSO registered on
				// the session as an AgentSession::AgentReferenceImage named
				// "view1", "view2", ... -- the Nth image attached across ALL
				// prompts is viewN.  Raw file bytes (not base64) -- compare_to_
				// reference decodes them itself.  Scenarios/skills reference
				// these names; keep this contract in sync with AgentSession.h's
				// SetReferenceImages doc if it ever changes.
				std::vector<AgentReferenceImage> referenceImages;
				for( std::size_t pi = 0; pi < scenario.prompts.size(); ++pi ) {
					const AgentEvalPrompt& prompt = scenario.prompts[pi];
					for( std::size_t ii = 0; ii < prompt.imagePaths.size(); ++ii ) {
						const std::string& imgPath = prompt.imagePaths[ii];
						std::string mimeType;
						const std::string ext = FileExtensionLower( imgPath );
						if( ext == ".png" ) mimeType = "image/png";
						else if( ext == ".jpg" || ext == ".jpeg" ) mimeType = "image/jpeg";
						else {
							handle.result.terminalStatus = "load_error";
							handle.result.errorMessage =
								"scenario '" + scenario.id + "' prompts[" + std::to_string( pi ) +
								"].images[" + std::to_string( ii ) + "] '" + imgPath +
								"': unsupported reference-image type";
							return handle;
						}
						std::string bytes, readErr;
						if( !ReadWholeFile( imgPath, bytes, readErr ) ) {
							handle.result.terminalStatus = "load_error";
							handle.result.errorMessage =
								"scenario '" + scenario.id + "' prompts[" + std::to_string( pi ) +
								"].images[" + std::to_string( ii ) + "]: could not read reference image '" +
								imgPath + "'";
							return handle;
						}
						ChatAttachment att;
						att.mimeType = mimeType;
						att.base64Data = Base64Encode( std::vector<unsigned char>( bytes.begin(), bytes.end() ) );
						promptAttachments[pi].push_back( att );

						AgentReferenceImage refImg;
						refImg.name     = "view" + std::to_string( referenceImages.size() + 1 );
						refImg.pngBytes = bytes;   // raw file bytes verbatim (compare_to_reference decodes non-PNG types honestly, per its own PNG-only decode contract)
						referenceImages.push_back( std::move( refImg ) );
					}
				}
				session->SetReferenceImages( std::move( referenceImages ) );

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
				// The loop's per-turn anti-spin cap (a provider-dependent default:
				// bounded for hosted, far higher for local) is an interactive-chat
				// posture; THIS host enforces the scenario's own budgets each
				// round (maxToolCalls/maxLlmCalls -- the honest, accounted
				// stops), so raise the instance cap to the budget ceiling and
				// let the budgets govern.  A legitimately iterative single-turn
				// scenario (image->scene reconstruction runs ~12-15
				// render-inspect-adjust rounds) would otherwise die at the cap
				// with provider_error("iteration cap") long before its budgets
				// -- the first live vision baseline failed 24/24 exactly this
				// way.  Scenarios with no budgets keep the default cap.
				{
					int cap = AgentChatLoop::kMaxToolRoundsPerTurn;
					if( scenario.budgets.maxLlmCalls  > cap ) cap = scenario.budgets.maxLlmCalls;
					if( scenario.budgets.maxToolCalls > cap ) cap = scenario.budgets.maxToolCalls;
					loop.SetMaxToolRoundsPerTurn( cap );
				}
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
					// Image-reconstruction Wave 1: the attachments overload only
					// when this turn actually carries reference images --
					// byte-identical behavior for every text-only scenario.
					if( promptAttachments[pi].empty() )
						loop.AddUserMessage( scenario.prompts[pi].text );
					else
						loop.AddUserMessage( scenario.prompts[pi].text, promptAttachments[pi] );

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
						// HTTP 429 (rate-limit) backoff: counts 429 RESPONSES
						// seen for THIS round (reset per round, unlike
						// `attempt` which also counts the 5xx/image/
						// reasoning-effort retries above).  See the 429
						// branch below and the RunScenarioLive doc comment.
						int rateLimitAttempts = 0;
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
							// failed attempts -- the honest cost measure.  EXCEPTION:
							// an HTTP 429 (rate-limit) response is NOT counted here --
							// see the 429 branch below, which is the one case where a
							// received response still does not consume the budget
							// (the call never succeeded; RISE waits it out rather
							// than billing the scenario for it).
							if( !( fo.proceed && fo.status == 429 ) ) ++llmCalls;
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

							// HTTP 429 (rate-limit) backoff: an EXPECTED condition
							// for image-heavy scenarios re-sending a full
							// multi-image payload every round, not a terminal
							// failure.  Retry the SAME round up to
							// kRateLimitMaxAttempts times total, waiting between
							// attempts.  The wait is checked against the
							// scenario's REMAINING maxWallMs budget BEFORE it is
							// dispatched -- a wait that would blow the budget
							// stops the run honestly (budget_wall_ms) rather than
							// pretending the round could still finish in time.
							if( fo.status == 429 ) {
								++rateLimitAttempts;
								// The FINAL attempt (rateLimitAttempts reaches
								// kRateLimitMaxAttempts): no further retry will
								// follow, so there is nothing left to back off
								// FOR.  Report exhaustion immediately rather
								// than computing/checking/waiting out a dead
								// backoff -- that wasted wait burns wall-clock
								// for no benefit and can push a legitimate
								// rate_limited-style exhaustion past the
								// scenario's wall budget, misreporting it as
								// budget_wall_ms instead of the honest
								// provider_error below.
								if( rateLimitAttempts >= kRateLimitMaxAttempts ) {
									terminalStatus = "provider_error";
									errorMessage = "rate limit: exhausted " + std::to_string( rateLimitAttempts ) +
										" attempt(s) waiting out HTTP 429 (Too Many Requests) responses";
									roundStopped = true;
									break;
								}
								const int64_t waitMs = ComputeRateLimit429WaitMs( fo.body, rateLimitAttempts );
								if( scenario.budgets.maxWallMs >= 0 ) {
									const int64_t remaining =
										scenario.budgets.maxWallMs - ( clock() - startMs );
									if( remaining <= 0 || waitMs > remaining ) {
										terminalStatus = "budget_wall_ms";
										budgetHit = true;
										roundStopped = true;
										break;
									}
								}
								effectiveSleeper( waitMs );
								continue;   // retry the SAME round after the backoff wait
							}

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
								// rpcId captured (not just incremented inline) so the
								// ask_user interception below can stamp the SAME id
								// into its synthesized response -- ToolCallToJsonRpcLine
								// already embeds it into `line`'s "id" field either way.
								const int rpcId = nextRpcId++;
								const std::string line = loop.ToolCallToJsonRpcLine( call, rpcId );
								std::string resp;
								if( call.name == "ask_user" ) {
									// PINNED DESIGN: ask_user is a chat-loop-only tool
									// (see AgentChatCodecs.cpp) -- it has NO AgentRpc verb
									// and is never sent to dispatcher->HandleLine.  The
									// HOST (here: the eval runner) intercepts it BEFORE
									// dispatch and synthesizes the result itself, exactly
									// as the Mac/Windows GUI drive loop will (stage 1b).
									//
									// Stage 2: consult the scenario's OPTIONAL scripted
									// responder (askUserResponses) first -- parse the
									// call's "question" argument out of argsJson and try
									// ResolveScriptedAskUserAnswer against it.  A scenario
									// that never sets askUserResponses gets
									// AgentEvalAskUserResponses's default-constructed
									// (empty matches, hasDefault false) value, for which
									// ResolveScriptedAskUserAnswer always returns false --
									// so the fallback branch below is BYTE-IDENTICAL to
									// stage 1's unconditional stub on every scenario that
									// does not opt in.
									std::string question;
									{
										JsonValue argsVal; std::string argsErr;
										if( JsonParse( call.argsJson, argsVal, argsErr ) && argsVal.isObject() &&
										    argsVal.get( "question" ).isString() )
											question = argsVal.get( "question" ).asString();
									}
									std::string scriptedAnswer;
									JsonValue resultObj = JsonValue::MakeObject();
									if( ResolveScriptedAskUserAnswer( scenario.askUserResponses, question, scriptedAnswer ) ) {
										resultObj.set( "answer", JsonValue::MakeString( scriptedAnswer ) );
									} else {
										// The stage-1 stub, unchanged: an eval run has no
										// interactive user and no scripted answer covers
										// this question, so synthesize a SUCCESS (never an
										// error -- an error result would pollute the
										// trajectory with noise the model did not cause)
										// telling the model no one is available and it
										// should proceed on its own judgment, matching the
										// tool description's contract.
										resultObj.set( "available", JsonValue::MakeBool( false ) );
										resultObj.set( "note", JsonValue::MakeString(
											"ask_user: no interactive user is available in this run; "
											"use your best judgment and proceed" ) );
									}
									JsonValue respObj = JsonValue::MakeObject();
									respObj.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
									respObj.set( "id", JsonValue::MakeNumber( static_cast<double>( rpcId ) ) );
									respObj.set( "result", resultObj );
									resp = JsonSerialize( respObj );
								}
								else {
									resp = dispatcher->HandleLine( line );
								}
								loop.AddToolResult( call, resp );
								// Counts the SAME as every other tool -- ask_user is a
								// real round-trip against the scenario's tool-call
								// budget (maxToolCalls), just like a dispatched verb;
								// it is not free just because it never reaches the
								// dispatcher.
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

				// Content-aware-resume + report-side staleness stamp (P1): the
				// scenario's oracle/drive hash + its raw checkpoint count let a
				// later RunEvalMatrix into the SAME runDir tell "still valid,
				// skip" from "oracle changed, STALE -- wipe + re-run", and let
				// tools/eval_report.py flag results graded under a since-changed
				// scenario version.  The TRUE (un-sanitized) provider/model are
				// stamped too (P2): the reporter prefers these over its lossy
				// dir-name parse, where two distinct model ids can sanitize to
				// one leaf.  On the replay path (modelId=="") these carry the
				// replay-fixture's provider + an empty model -- harmless; only
				// matrix cells (RunScenarioLive) carry the values P2 needs.
				r.set( "scenarioContentHash", JsonValue::MakeString( ScenarioContentHash( scenario ) ) );
				r.set( "scenarioCheckpointCount",
					JsonValue::MakeNumber( static_cast<double>( scenario.checkpoints.isArray() ? scenario.checkpoints.size() : 0 ) ) );
				r.set( "provider", JsonValue::MakeString( ChatProviderName( provider ) ) );
				r.set( "model", JsonValue::MakeString( modelId ) );

				// Raw file-byte FNV-1a 64 hashes (Finding 2): unlike
				// ScenarioContentHash (a semantic canonicalization only this C++
				// binary can recompute), these are trivially recomputable
				// byte-for-byte by a standalone Python reporter -- closing the
				// "equal checkpoint count but different content" blind spot for
				// FILE-backed scenarios/scenes.  Keys are OMITTED entirely (not
				// stamped as "") when not applicable/unreadable -- absent means
				// "no check", never "stale".
				if( !scenario.sourcePath.empty() ) {
					const std::string h = FnvHexOfFile( scenario.sourcePath );
					if( !h.empty() ) r.set( "scenarioFileFnv", JsonValue::MakeString( h ) );
				}
				if( !scenario.scenePath.empty() ) {
					const std::string h = FnvHexOfFile( scenario.scenePath );
					if( !h.empty() ) r.set( "sceneFileFnv", JsonValue::MakeString( h ) );
				}

				// Image-reconstruction Wave 1 (+ P1-1): report-side staleness
				// parity for reference images, mirroring
				// scenarioFileFnv/sceneFileFnv above.  "refImageFnvs" maps each
				// DISTINCT referenced image path -- a prompt attachment OR a
				// render checkpoint's "compareToImage" reference -- to the
				// FNV-1a 64 of its current bytes (recomputable by
				// tools/eval_report.py's fnv1a64_hex, which just walks the map
				// generically; it does not care which source populated a given
				// key).  Stamped whenever prompts carry images OR any
				// checkpoint carries compareToImage; omitted entirely (not an
				// empty object) when neither applies, so absence still means
				// "no check" rather than "zero images verified".  By this
				// point every path was already proven readable by the
				// pre-flight above (prompts) / by CheckRenderKind (compare
				// images, at grading time), so FnvHexOfFile is expected
				// non-empty here; the same omit-on-empty guard as
				// scenarioFileFnv/sceneFileFnv is kept anyway (defense-in-depth
				// against a file removed mid-run).
				{
					JsonValue refImageFnvs = JsonValue::MakeObject();
					for( std::size_t pi = 0; pi < scenario.prompts.size(); ++pi ) {
						for( std::size_t ii = 0; ii < scenario.prompts[pi].imagePaths.size(); ++ii ) {
							const std::string& imgPath = scenario.prompts[pi].imagePaths[ii];
							if( refImageFnvs.has( imgPath ) ) continue;   // dedupe a path reused across prompts
							const std::string h = FnvHexOfFile( imgPath );
							if( !h.empty() ) refImageFnvs.set( imgPath, JsonValue::MakeString( h ) );
						}
					}
					if( scenario.checkpoints.isArray() ) {
						for( std::size_t ci = 0; ci < scenario.checkpoints.size(); ++ci ) {
							const JsonValue& cp = scenario.checkpoints.at( ci );
							if( !cp.isObject() ) continue;
							if( !cp.has( "kind" ) || !cp.get( "kind" ).isString() || cp.get( "kind" ).asString() != "render" ) continue;
							if( !cp.has( "compareToImage" ) || !cp.get( "compareToImage" ).isString() ) continue;
							const std::string& imgPath = cp.get( "compareToImage" ).asString();
							if( refImageFnvs.has( imgPath ) ) continue;   // dedupe a path reused across checkpoints (or with a prompt image)
							const std::string h = FnvHexOfFile( imgPath );
							if( !h.empty() ) refImageFnvs.set( imgPath, JsonValue::MakeString( h ) );
						}
					}
					if( !refImageFnvs.members().empty() )
						r.set( "refImageFnvs", refImageFnvs );
				}

				const std::string resultPath = JoinPath( runDir, scenario.id + ".result.jsonl" );
				{
					std::error_code ec;
					std::filesystem::create_directories( std::filesystem::path( runDir ), ec );
					std::ofstream rf( resultPath.c_str(), std::ios::binary );
					if( rf ) rf << JsonSerialize( r ) << "\n";
				}
				handle.resultPath = resultPath;

				// Persist the FINAL assembled scene alongside the result, so a
				// run can be re-rendered or inspected later.  The renders
				// themselves are computed in-memory and discarded -- only their
				// numeric grades survive in the checkpoints -- so without this
				// the scene the model actually built is only reconstructible by
				// replaying the trajectory's insert_chunk calls.  ReadDocument()
				// returns the canonical `.RISEscene` text of the head (secrets
				// length-masked; a no-op for eval scenes), re-loadable via
				// LoadAsciiSceneViaCst.  Best-effort: a run with no session or an
				// empty head writes nothing (a load_error / never-built run).
				if( dispatcher && dispatcher->Session() ) {
					const std::string finalScene = dispatcher->Session()->ReadDocument();
					if( !finalScene.empty() ) {
						const std::string scenePath = JoinPath( runDir, scenario.id + ".final.RISEscene" );
						std::ofstream sf( scenePath.c_str(), std::ios::binary );
						if( sf ) sf << finalScene;
					}
				}

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
			                          /*apiKey=*/std::string(), replayFetch,
			                          /*sleeper=*/std::function<void(int64_t)>() );
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
			                          options.apiKey, liveFetch, options.sleeper );
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
					if( IsSanitizePreservedChar( s[i] ) ) o += s[i];
					else o += '_';
				}
				if( o.empty() ) o = "x";
				return o;
			}
		}

		//====================================================================
		// PRE-FLIGHT AUTH PROBE (RunEvalMatrix) -- see the doc comment on
		// RunEvalMatrix in AgentEvalRunner.h for the full behavioural
		// contract.  Kept as free functions in this file-local namespace so
		// RunEvalMatrix's own body stays a straight-line read of "probe, then
		// run the column".
		//====================================================================
		namespace
		{
			//! Lower-cased ASCII copy of `s`.  The ONLY thing this file needs a
			//! case fold for -- IsAuthProbeFailure's keyword match below.
			std::string ToLowerAscii( const std::string& s )
			{
				std::string o( s );
				for( char& c : o ) c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
				return o;
			}

			//! The auth-probe DEAD-CREDENTIAL body phrases, matched case-
			//! insensitively as a raw substring.  Deliberately NARROW and
			//! drawn from real provider error text (Anthropic "Your credit
			//! balance is too low...", OpenAI/xAI "Incorrect API key
			//! provided...", generic "authentication"/"unauthorized" wording)
			//! -- never a bare "error"/"invalid" match, which would also catch
			//! an ordinary malformed-request 400 and wrongly skip a column
			//! whose credential is actually fine.
			const char* const kAuthProbeFailurePhrases[] = {
				"incorrect api key", "invalid api key", "credit balance",
				"authentication", "unauthorized"
			};

			//! True iff (`status`, `body`) is a DEAD-CREDENTIAL response to the
			//! pre-flight auth probe: a bare 401/403 (any body), or a 4xx body
			//! containing one of kAuthProbeFailurePhrases (case-insensitive).
			//! Always false for a 2xx/3xx and for a 5xx (a transient server
			//! error must never skip a whole provider column) -- and false for
			//! a 4xx that matches none of the phrases (an ordinary bad-request/
			//! tool-shaped 400, e.g. the reasoning_effort quirk AgentChatLoop
			//! already retries around).  On true, `matchedReason` is filled
			//! with a SHORT, key-free description for the log line/manifest
			//! (the status code, plus the matched phrase for a keyword hit).
			bool IsAuthProbeFailure( long status, const std::string& body, std::string& matchedReason )
			{
				if( status == 401 ) { matchedReason = "401 Unauthorized"; return true; }
				if( status == 403 ) { matchedReason = "403 Forbidden"; return true; }
				if( status < 400 || status >= 500 ) return false;
				const std::string lowerBody = ToLowerAscii( body );
				for( std::size_t i = 0; i < sizeof( kAuthProbeFailurePhrases ) / sizeof( kAuthProbeFailurePhrases[0] ); ++i ) {
					if( lowerBody.find( kAuthProbeFailurePhrases[i] ) != std::string::npos ) {
						matchedReason = std::to_string( status ) + " (\"" + kAuthProbeFailurePhrases[i] + "\")";
						return true;
					}
				}
				return false;
			}

			//! Perform the ONE-ROUND pre-flight auth probe for a resolved,
			//! non-keyless provider column: build the SAME minimal request
			//! shape a real run's first LLM round would (a fresh
			//! AgentChatLoop, one "ping" user turn, BuildRequest(key) -- no
			//! tool registration or trajectory sink needed; the codecs bake
			//! their own tool defs in and BuildRequest works off whatever the
			//! transcript holds) and POST it through the SAME transport a real
			//! run uses.  Returns true (DEAD credential -- the caller SKIPS
			//! the whole column) only for IsAuthProbeFailure; a transport-
			//! level failure (status <= 0: DNS/TLS/timeout) is explicitly NOT
			//! an auth verdict -- the real runs will hit the same failure
			//! honestly, per-cell, if it persists.  `reason` is ALWAYS filled
			//! with a short, KEY-FREE diagnostic (never the raw request/
			//! response body, never the key -- this function never retains
			//! `apiKey` past the BuildRequest call, matching every other
			//! apiKey use in this file).
			bool RunAuthProbe( IChatHttpTransport* transport, ChatProvider provider,
			                   const std::string& modelId, const std::string& apiKey,
			                   std::string& reason )
			{
				AgentChatLoop probe;
				probe.SetProvider( provider, modelId );
				probe.AddUserMessage( "ping" );
				const ChatHttpRequest req = probe.BuildRequest( apiKey );
				if( req.url.empty() ) {
					// Defensive only -- a real provider codec always builds a
					// non-empty request for a one-user-turn transcript; never
					// block a column on an internal builder bug.
					reason = "probe request build was empty (not attempted)";
					return false;
				}
				const ChatHttpResponse resp = transport->Post( req );
				if( resp.status <= 0 ) {
					reason = "transport error (not auth-fatal): " +
						( resp.error.empty() ? std::string( "no detail" ) : resp.error );
					return false;
				}
				std::string matched;
				if( IsAuthProbeFailure( resp.status, resp.body, matched ) ) {
					reason = "HTTP " + matched;
					return true;
				}
				reason = "HTTP " + std::to_string( resp.status );
				return false;
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

			// REFUSE LOUDLY on a provider/model leaf collision.  A cell's
			// subdir leaf embeds SanitizeForPath(provider)[__SanitizeForPath(model)]
			// -- any char outside [A-Za-z0-9._-] maps to '_'.  Two DIFFERENT
			// (provider, model) pairs can therefore sanitize to the SAME leaf
			// fragment (e.g. models "a:b" and "a b" both -> "a_b"), silently
			// colliding into one cell + one report row -- the exact
			// trajectory-append / result-overwrite corruption the duplicate-id
			// guard above prevents, but on the provider axis.  Catch it here,
			// before any run executes (needs only config.providers, no key
			// resolution); all counts stay 0, same style as the id guard.  Two
			// entries with the IDENTICAL original (provider, model) are just
			// redundant config, NOT a collision -- only flag when the ORIGINAL
			// pairs differ yet the SANITIZED fragment matches.
			{
				auto leafFragment = []( const AgentEvalProviderConfig& p ) {
					std::string frag = SanitizeForPath( p.provider );
					if( !p.model.empty() ) frag += "__" + SanitizeForPath( p.model );
					return frag;
				};
				for( std::size_t a = 0; a < config.providers.size(); ++a ) {
					for( std::size_t b = a + 1; b < config.providers.size(); ++b ) {
						const AgentEvalProviderConfig& pa = config.providers[a];
						const AgentEvalProviderConfig& pb = config.providers[b];
						const bool sameOriginal = ( pa.provider == pb.provider && pa.model == pb.model );
						if( !sameOriginal && leafFragment( pa ) == leafFragment( pb ) ) {
							result.errorMessage =
								"RunEvalMatrix: provider/model leaf collision -- (provider '" +
								pa.provider + "', model '" + pa.model + "') and (provider '" +
								pb.provider + "', model '" + pb.model + "') both sanitize to the "
								"same per-run subdir fragment '" + leafFragment( pa ) +
								"' (refusing to run: two distinct providers/models would collide "
								"into one cell + one report row, silently appending trajectories "
								"and overwriting results)";
							logLine( result.errorMessage );
							return result;
						}
					}
				}
			}

			const int reps = config.repeats > 0 ? config.repeats : 0;

			// PRE-FLIGHT AUTH PROBE outcomes, keyed by the provider's declared
			// NAME ("anthropic" / "gemini" / ...) -- one entry per provider
			// column that reaches the probe-eligibility point (i.e. past the
			// missing-key/empty-keyEnvVar skips below).  Stamped into the run
			// manifest's "authProbes" object at the end of this function.  See
			// RunEvalMatrix's doc comment for the full contract.
			std::map<std::string, std::string> authProbeResults;

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

				// PRE-FLIGHT AUTH PROBE (see RunEvalMatrix's doc comment): a
				// keyless "local" column has no credential to probe -- skip the
				// probe entirely so an unreachable local server still surfaces
				// per-cell, as before this feature.  Every other column's
				// (now-resolved) key is probed ONCE before its scenario x
				// repeat loop runs, so a DEAD credential is caught loudly here
				// instead of burning every cell on the same auth failure.
				if( prov.provider == "local" ) {
					authProbeResults[prov.provider] = "skipped-keyless";
				} else {
					std::string probeReason;
					if( RunAuthProbe( options.transport, providerEnum, prov.model, key, probeReason ) ) {
						++result.providersSkipped;
						++result.providersAuthFailed;
						result.runsSkipped += static_cast<int>( scenarios.size() ) * reps;
						logLine( "RunEvalMatrix: SKIP provider '" + prov.provider +
							"' -- credential rejected by the provider (auth probe failed: " + probeReason + ")" );
						authProbeResults[prov.provider] = "auth-failed";
						continue;
					}
					authProbeResults[prov.provider] = "ok";
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
						// invocation into this same runDir.  But "completed"
						// is not enough to reuse it: the cell was graded under
						// whatever oracle (checkpoints[]) / drive spec the
						// scenario had THEN, and if the scenario changed since,
						// the cached score is STALE.  So we stamp each result
						// with a ScenarioContentHash and compare:
						//   * result present, non-empty, parses, and its
						//     scenarioContentHash EQUALS the current scenario's
						//     hash -> genuinely still valid: SKIP (rather than
						//     re-executing, which would reopen trajectory.jsonl
						//     in APPEND mode -- concatenating two sessions into
						//     one file -- while truncate-overwriting result.jsonl).
						//   * result present + non-empty but the stamped hash is
						//     MISSING (an old pre-content-hash result) or DIFFERS
						//     (the scenario's oracle/prompts/budgets/scene
						//     changed) -> STALE: wipe the subdir and fall through
						//     to re-run, exactly like a crashed run below.
						//   * result missing/empty -> crashed/interrupted run:
						//     wipe its contents first so the trajectory sink's
						//     append can't concatenate onto a partial file, then
						//     fall through and re-run it normally.
						const std::string resultPath =
							( std::filesystem::path( runDir ) / ( scenario.id + ".result.jsonl" ) ).string();
						{
							std::error_code ec;
							const bool resultExists = std::filesystem::exists( resultPath, ec ) && !ec;
							const std::uintmax_t resultSize =
								resultExists ? std::filesystem::file_size( resultPath, ec ) : 0;
							bool completeAndCurrent = false;
							if( resultExists && !ec && resultSize > 0 ) {
								// Read the one-line summary and compare its
								// stamped hash against the CURRENT scenario's.
								std::ifstream rf( resultPath.c_str(), std::ios::binary );
								std::string firstLine;
								std::getline( rf, firstLine );
								JsonValue parsed;
								std::string perr;
								if( JsonParse( firstLine, parsed, perr ) && parsed.isObject() ) {
									const JsonValue* stamped = parsed.find( "scenarioContentHash" );
									if( stamped && stamped->isString() &&
									    stamped->asString() == ScenarioContentHash( scenario ) ) {
										completeAndCurrent = true;
									}
								}
							}
							if( completeAndCurrent ) {
								++result.runsAlreadyComplete;
								logLine( "RunEvalMatrix: SKIP " + leaf +
									" -- already completed -- skipping (delete the subdir to re-run)" );
								continue;
							}
							if( resultExists && !ec && resultSize > 0 ) {
								// Present but stale (hash missing/different): the
								// scenario changed since this cell was graded.
								logLine( "RunEvalMatrix: STALE " + leaf +
									" -- scenario changed since it was recorded (hash mismatch) -- re-running" );
							}
							ec.clear();
							if( std::filesystem::exists( runDir, ec ) && !ec ) {
								std::filesystem::remove_all( runDir, ec );   // wipe a stale OR crashed/partial run's leftovers
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
						lo.sleeper  = options.sleeper;   // HTTP 429 backoff hook (see AgentEvalLiveRunOptions.sleeper)

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

				// PRE-FLIGHT AUTH PROBE outcomes, keyed by provider name -- "ok"
				// (probed, credential live), "auth-failed" (probed, credential
				// dead -- the column was SKIPPED), or "skipped-keyless" (a
				// "local" column, never probed).  A provider column skipped
				// BEFORE the probe point (missing/empty keyEnvVar) has no entry
				// here -- its own SKIP log line already explains why.
				JsonValue authProbesObj = JsonValue::MakeObject();
				for( std::map<std::string, std::string>::const_iterator it = authProbeResults.begin();
				     it != authProbeResults.end(); ++it ) {
					authProbesObj.set( it->first, JsonValue::MakeString( it->second ) );
				}
				root.set( "authProbes", authProbesObj );

				JsonValue resultObj = JsonValue::MakeObject();
				resultObj.set( "runsExecuted",        JsonValue::MakeNumber( result.runsExecuted ) );
				resultObj.set( "runsSkipped",         JsonValue::MakeNumber( result.runsSkipped ) );
				resultObj.set( "runsAlreadyComplete", JsonValue::MakeNumber( result.runsAlreadyComplete ) );
				resultObj.set( "providersUsed",       JsonValue::MakeNumber( result.providersUsed ) );
				resultObj.set( "providersSkipped",    JsonValue::MakeNumber( result.providersSkipped ) );
				resultObj.set( "providersAuthFailed", JsonValue::MakeNumber( result.providersAuthFailed ) );
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

				//! EVERY value of `paramName` in a chunk, in document order.  A
				//! repeated same-role param (e.g. a `timeline`'s repeated `value`
				//! keyframes) is each its OWN NodeKind::Param child, so this
				//! returns one space-joined string per occurrence -- the sibling
				//! of CheckerChunkParamValue (which returns only the FIRST) that
				//! series ops like param_series_orbit need to read a full keyframe
				//! track.  An occurrence with no pvalue tokens yields an empty
				//! string entry (kept, so positional callers see the gap).
				std::vector<std::string> CheckerChunkParamValues( const NodeRef& chunk, const std::string& paramName )
				{
					std::vector<std::string> out;
					if( !chunk ) return out;
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
						out.push_back( joined );
					}
					return out;
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
						// REJECT AT THE STRING LAYER.  This avoids a value-level
						// classification after -ffast-math has attached a finite
						// assumption to the parsed double.  Text and errno are
						// integer-domain, so they are immune to that assumption:
						//   * no valid decimal OR hex float token contains 'n' or
						//     'i' ('n'/'i' are not hex digits, and the exponent
						//     markers are e/E/p/P), so those letters can only come
						//     from nan / inf / infinity;
						//   * strtod sets ERANGE for overflow ("1e999" -> HUGE_VAL)
						//     and for underflow; both are refused as comparison
						//     values.
						if( tok.find_first_of( "nNiI" ) != std::string::npos ) return false;
						errno = 0;
						const double v = std::strtod( begin, &end );
						if( end != begin + tok.size() ) return false;   // non-numeric / trailing junk
						if( errno == ERANGE ) return false;             // overflow / underflow
						out.push_back( v );
					}
					return true;
				}

				//! Evaluate the optional any-of-kind `where` co-binding filter
				//! against a single candidate chunk.  Each entry is EITHER an
				//! EQUALITY entry ({param, value}: the chunk's param value must
				//! equal `value` -- 1e-4-per-component tolerance when BOTH sides
				//! tokenize all-numeric, else exact string) OR a RANGE entry
				//! ({param, min:[...], max:[...]}: the chunk's param float tokens
				//! must each fall in the matching [min_i, max_i] band, mirroring
				//! param_range).  A chunk passes ONLY when EVERY entry is
				//! satisfied.  A missing param, a component-count mismatch, or a
				//! non-numeric value where numbers are required all fail the entry
				//! (the chunk is filtered OUT).  Load validation has already
				//! enforced entry shape; the defensive shape checks here keep a
				//! hand-built checkpoint from crashing.
				bool CheckerChunkSatisfiesWhere( const NodeRef& chunk, const JsonValue& whereArr )
				{
					if( !whereArr.isArray() ) return true;   // no filter -> every chunk passes
					for( std::size_t i = 0; i < whereArr.size(); ++i ) {
						const JsonValue& e = whereArr.at( i );
						if( !e.isObject() || !e.has( "param" ) || !e.get( "param" ).isString() ) return false;
						const std::string param = e.get( "param" ).asString();
						std::string actual;
						if( !CheckerChunkParamValue( chunk, param, actual ) ) return false;
						const bool isRange = e.has( "min" ) || e.has( "max" );
						if( isRange ) {
							if( !e.has( "min" ) || !e.get( "min" ).isArray() || !e.has( "max" ) || !e.get( "max" ).isArray() )
								return false;
							const JsonValue& mn = e.get( "min" );
							const JsonValue& mx = e.get( "max" );
							std::vector<double> mins, maxs, av;
							for( std::size_t k = 0; k < mn.size(); ++k ) { if( !mn.at( k ).isNumber() ) return false; mins.push_back( mn.at( k ).asNumber() ); }
							for( std::size_t k = 0; k < mx.size(); ++k ) { if( !mx.at( k ).isNumber() ) return false; maxs.push_back( mx.at( k ).asNumber() ); }
							if( mins.empty() || mins.size() != maxs.size() ) return false;
							if( !CheckerParseFloatTokens( actual, av ) || av.size() != mins.size() ) return false;
							for( std::size_t k = 0; k < av.size(); ++k )
								if( av[k] < mins[k] || av[k] > maxs[k] ) return false;
						} else {
							if( !e.has( "value" ) || !e.get( "value" ).isString() ) return false;
							const std::string expected = e.get( "value" ).asString();
							std::vector<double> av, ev;
							if( CheckerParseFloatTokens( expected, ev ) && CheckerParseFloatTokens( actual, av ) &&
							    av.size() == ev.size() && !ev.empty() ) {
								for( std::size_t k = 0; k < av.size(); ++k )
									if( std::fabs( av[k] - ev[k] ) > 1e-4 ) return false;
							} else if( actual != expected ) {
								return false;
							}
						}
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

				//! Collect EVERY top-level chunk whose keyword is EXACTLY `kind`
				//! (document order).  Backs the ANY-OF-KIND (target-less) form of
				//! the param_equals / param_range / param_references_kind ops: the
				//! op passes iff ANY of the returned chunks satisfies the
				//! per-chunk assertion.
				std::vector<NodeRef> CheckerCollectChunksOfKind( const Document& doc, const std::string& kind )
				{
					std::vector<NodeRef> out;
					const int n = RISE::Cst::DocItemCount( doc );
					for( int i = 0; i < n; ++i ) {
						const RISE::Cst::NodeId id = RISE::Cst::DocNodeIdAt( doc, i );
						if( !id ) continue;
						NodeRef item = RISE::Cst::DocResolveNodeId( doc, id );
						if( !item || item->kind != NodeKind::Chunk ) continue;
						if( item->role != kind ) continue;
						out.push_back( item );
					}
					return out;
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
				//! ANY-OF-KIND: param_equals / param_range / param_references_kind
				//! also accept a TARGET-LESS existential form -- omit "target"
				//! (or leave it empty) and supply a non-empty "chunkKind"; the op
				//! then passes iff ANY chunk of that kind satisfies the per-chunk
				//! assertion (the per-chunk logic is shared with the named form).
				//! An optional any-of-kind "where":[{param,value}|{param,min,max}]
				//! CO-BINDING filter narrows the candidate set BEFORE the per-chunk
				//! assertion (only chunks satisfying every entry are considered) --
				//! collapsing two independent existentials into one AND'd-on-the-
				//! same-chunk grade.  {op:"param_series_orbit",chunkKind,where?,
				//! param,minKeyframes,minAngularSpreadDeg,centerWithin?,
				//! maxRadiusRatio?} (any-of-kind only) proves a single timeline's
				//! keyframe track is a camera ORBIT rather than accepting two
				//! independent existentials.
				CheckOutcome CheckDocumentKind( const JsonValue& cp, AgentSession* session )
				{
					if( !session ) return { false, "document checkpoint: no live session (run did not complete)" };
					if( !cp.has( "op" ) || !cp.get( "op" ).isString() )
						return { false, "document checkpoint missing string field \"op\"" };
					const std::string op = cp.get( "op" ).asString();
					const Document doc = RISE::Cst::ParseToCst( session->ReadDocument() );

					// Shared TARGET resolution for the three target-based ops.
					// `chunks` is either the single named target (name mode) or
					// EVERY chunk of chunkKind (any-of-kind mode, when target is
					// absent/empty).  `ok=false` carries a hard shape/lookup
					// failure detail the op returns verbatim.
					struct TargetResolution { bool ok; std::string detail; std::vector<NodeRef> chunks; bool anyOfKind; std::string kind; };
					auto resolveTargets = [&]( const std::string& opName ) -> TargetResolution {
						const std::string kind = OptKind( cp );
						const bool hasTarget = cp.has( "target" ) && cp.get( "target" ).isString() && !cp.get( "target" ).asString().empty();
						if( hasTarget ) {
							const std::string target = cp.get( "target" ).asString();
							NodeRef chunk;
							const int found = CheckerFindChunk( doc, kind, target, chunk );
							if( found == 0 )
								return { false, opName + ": no chunk named '" + target + "' found" +
									( kind.empty() ? std::string() : ( " (kind '" + kind + "')" ) ), {}, false, kind };
							if( found < 0 )
								return { false, opName + ": AMBIGUOUS -- more than one chunk named '" + target +
									"' -- narrow with \"chunkKind\"", {}, false, kind };
							return { true, std::string(), { chunk }, false, kind };
						}
						// any-of-kind: target absent/empty -> chunkKind REQUIRED
						// (load-validated; this is a defensive re-check).
						if( kind.empty() )
							return { false, opName + ": no \"target\" and no \"chunkKind\" -- the any-of-kind form requires a chunkKind",
								{}, true, kind };
						std::vector<NodeRef> chunks = CheckerCollectChunksOfKind( doc, kind );
						if( chunks.empty() )
							return { false, opName + " (any-of-kind): no chunk of kind '" + kind + "' found", {}, true, kind };
						// Optional co-binding "where" filter (any-of-kind ONLY --
						// load-validated absent on the named-target form): a
						// candidate survives only when it satisfies EVERY where
						// entry, so two independent existentials collapse into one
						// AND'd-on-the-same-chunk assertion.
						if( cp.has( "where" ) && cp.get( "where" ).isArray() ) {
							std::vector<NodeRef> filtered;
							for( const NodeRef& c : chunks )
								if( CheckerChunkSatisfiesWhere( c, cp.get( "where" ) ) ) filtered.push_back( c );
							if( filtered.empty() )
								return { false, opName + " (any-of-kind): " + std::to_string( chunks.size() ) +
									" chunk(s) of kind '" + kind + "' found, but NONE satisfied the \"where\" co-binding filter",
									{}, true, kind };
							chunks.swap( filtered );
						}
						return { true, std::string(), chunks, true, kind };
					};

					// Run a per-chunk assertion across the resolution: name mode
					// grades the single target; any-of-kind passes iff ANY chunk
					// satisfies, and reports how many were examined.  `evalChunk`
					// takes (chunk, humanDescription) -> CheckOutcome.
					auto runAssertion = [&]( const std::string& opName, const TargetResolution& res, auto&& evalChunk ) -> CheckOutcome {
						if( !res.anyOfKind )
							return evalChunk( res.chunks[0], "'" + cp.get( "target" ).asString() + "'" );
						CheckOutcome lastFail{ false, std::string() };
						for( std::size_t i = 0; i < res.chunks.size(); ++i ) {
							const std::string nm = CheckerChunkName( res.chunks[i] );
							const std::string desc = "chunk '" + ( nm.empty() ? std::string( "<unnamed>" ) : nm ) +
								"' (kind '" + res.kind + "')";
							CheckOutcome oc = evalChunk( res.chunks[i], desc );
							if( oc.passed )
								return { true, opName + " (any-of-kind): " + std::to_string( res.chunks.size() ) +
									" chunk(s) of kind '" + res.kind + "' examined; " + desc + " satisfies -- " + oc.detail };
							lastFail = oc;
						}
						return { false, opName + " (any-of-kind): none of " + std::to_string( res.chunks.size() ) +
							" chunk(s) of kind '" + res.kind + "' satisfied the assertion (last: " + lastFail.detail + ")" };
					};

					if( op == "param_equals" ) {
						if( !cp.has( "param" ) || !cp.get( "param" ).isString() )
							return { false, "param_equals missing string \"param\"" };
						if( !cp.has( "value" ) || !cp.get( "value" ).isString() )
							return { false, "param_equals missing string \"value\"" };
						const std::string param  = cp.get( "param" ).asString();
						const std::string expected = cp.get( "value" ).asString();
						const bool numeric = cp.has( "numeric" ) && cp.get( "numeric" ).isBool() && cp.get( "numeric" ).asBool();

						const TargetResolution res = resolveTargets( "param_equals" );
						if( !res.ok ) return { false, res.detail };

						// Per-chunk assertion, shared between the named form and
						// the any-of-kind form.  Optional numeric tolerance:
						// tokenize BOTH sides as floats and compare within 1e-4
						// per component, so a live model's "0.0 0.0 1.0" grades
						// equal to a fixture's "0 0 1".  Refuses LOUDLY (not a
						// silent mismatch) when the token counts differ or a token
						// on either side is non-numeric.
						auto evalChunk = [&]( const NodeRef& chunk, const std::string& desc ) -> CheckOutcome {
							std::string actual;
							if( !CheckerChunkParamValue( chunk, param, actual ) )
								return { false, "param_equals: " + desc + " has no param '" + param + "'" };
							if( numeric ) {
								std::vector<double> av, ev;
								if( !CheckerParseFloatTokens( expected, ev ) )
									return { false, "param_equals(numeric): the checkpoint's \"value\" '" + expected +
										"' is not all-numeric" };
								if( !CheckerParseFloatTokens( actual, av ) )
									return { false, "param_equals(numeric): " + desc + "." + param + " == '" + actual +
										"' is not all-numeric (expected numeric '" + expected + "')" };
								if( av.size() != ev.size() )
									return { false, "param_equals(numeric): " + desc + "." + param + " has " +
										std::to_string( av.size() ) + " component(s), expected " + std::to_string( ev.size() ) +
										" ('" + actual + "' vs '" + expected + "')" };
								for( std::size_t i = 0; i < av.size(); ++i ) {
									if( std::fabs( av[i] - ev[i] ) > 1e-4 )
										return { false, "param_equals(numeric): " + desc + "." + param + " == '" + actual +
											"', expected '" + expected + "' (component " + std::to_string( i ) + " differs)" };
								}
								return { true, "param_equals(numeric): " + desc + "." + param + " == '" + expected +
									"' (within 1e-4)" };
							}
							if( actual != expected )
								return { false, "param_equals: " + desc + "." + param + " == '" + actual +
									"', expected '" + expected + "'" };
							return { true, "param_equals: " + desc + "." + param + " == '" + expected + "'" };
						};
						return runAssertion( "param_equals", res, evalChunk );
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
						if( !cp.has( "param" ) || !cp.get( "param" ).isString() )
							return { false, "param_range missing string \"param\"" };
						if( !cp.has( "min" ) || !cp.get( "min" ).isArray() )
							return { false, "param_range missing array \"min\"" };
						if( !cp.has( "max" ) || !cp.get( "max" ).isArray() )
							return { false, "param_range missing array \"max\"" };
						const std::string param  = cp.get( "param" ).asString();

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

						const TargetResolution res = resolveTargets( "param_range" );
						if( !res.ok ) return { false, res.detail };

						// Per-chunk assertion, shared between the named form and
						// the any-of-kind form.
						auto evalChunk = [&]( const NodeRef& chunk, const std::string& desc ) -> CheckOutcome {
							std::string actual;
							if( !CheckerChunkParamValue( chunk, param, actual ) )
								return { false, "param_range: " + desc + " has no param '" + param + "'" };
							std::vector<double> av;
							if( !CheckerParseFloatTokens( actual, av ) )
								return { false, "param_range: " + desc + "." + param + " == '" + actual +
									"' is not all-numeric" };
							if( av.size() != mins.size() )
								return { false, "param_range: " + desc + "." + param + " has " +
									std::to_string( av.size() ) + " component(s), expected " + std::to_string( mins.size() ) +
									" ('" + actual + "')" };
							for( std::size_t i = 0; i < av.size(); ++i ) {
								if( av[i] < mins[i] || av[i] > maxs[i] )
									return { false, "param_range: " + desc + "." + param + " == '" + actual +
										"', component " + std::to_string( i ) + " (" + std::to_string( av[i] ) +
										") outside [" + std::to_string( mins[i] ) + ", " + std::to_string( maxs[i] ) + "]" };
							}
							return { true, "param_range: " + desc + "." + param + " == '" + actual +
								"' within all " + std::to_string( av.size() ) + " band(s)" };
						};
						return runAssertion( "param_range", res, evalChunk );
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
						if( !cp.has( "param" ) || !cp.get( "param" ).isString() )
							return { false, "param_references_kind missing string \"param\"" };

						// Collect the accepted kinds from EITHER the singular
						// "referencedKind" (a string) OR the "referencedKinds"
						// any-of array -- exactly one must be supplied.  (This is
						// the REFERENCED chunk's kind set, distinct from the
						// TARGET-side any-of-kind form driven by "chunkKind".)
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

						const std::string param  = cp.get( "param" ).asString();
						std::string joinedKinds;
						for( std::size_t i = 0; i < acceptedKinds.size(); ++i ) {
							if( i ) joinedKinds += ", ";
							joinedKinds += "'" + acceptedKinds[i] + "'";
						}

						const TargetResolution res = resolveTargets( "param_references_kind" );
						if( !res.ok ) return { false, res.detail };

						// Per-chunk assertion, shared between the named form and
						// the any-of-kind form.  ANY-OF (referenced kinds): the
						// binding passes as soon as the named chunk resolves under
						// one accepted kind.  An AMBIGUOUS match under ANY single
						// kind is a hard rubric failure.
						auto evalChunk = [&]( const NodeRef& chunk, const std::string& desc ) -> CheckOutcome {
							std::string referencedName;
							if( !CheckerChunkParamValue( chunk, param, referencedName ) )
								return { false, "param_references_kind: " + desc + " has no param '" + param + "'" };
							if( referencedName.empty() )
								return { false, "param_references_kind: " + desc + "." + param + " has an empty value" };
							for( const std::string& refKind : acceptedKinds ) {
								NodeRef referenced;
								const int refFound = CheckerFindChunk( doc, refKind, referencedName, referenced );
								if( refFound < 0 )
									return { false, "param_references_kind: '" + referencedName +
										"' matches MORE THAN ONE chunk of kind '" + refKind + "'" };
								if( refFound == 1 )
									return { true, "param_references_kind: " + desc + "." + param + " -> '" +
										referencedName + "' (a '" + refKind + "')" };
							}
							return { false, "param_references_kind: " + desc + "." + param + " names '" +
								referencedName + "', but no chunk of kind(s) " + joinedKinds + " has that name" };
						};
						return runAssertion( "param_references_kind", res, evalChunk );
					}

					// param_series_orbit: ANY-OF-KIND only.  Per candidate chunk
					// (post-`where`), FIRST prove time progression is intrinsic to
					// the op (always enforced, not opt-in): collect every `param`
					// occurrence AND every `timeParam` occurrence (default "time")
					// in document order, require the RAW counts to match 1:1, every
					// time token to parse as a single float, and the time sequence
					// to be STRICTLY increasing (subsumes an all-equal/degenerate
					// range -- no epsilon needed).  THEN collect every `param`
					// occurrence in order, parse each as 3 floats (skip malformed),
					// dedupe consecutive duplicates, and grade the resulting XZ
					// point track as an ORBIT: >= minKeyframes points, an angular
					// spread (about the centerWithin point when given, else the
					// points' XZ centroid) of >= minAngularSpreadDeg, the centroid
					// within centerWithin's radius (when given), and a max/min
					// radius ratio <= maxRadiusRatio (when given).  This binds a
					// single timeline to PROVE a camera orbit -- both in SHAPE and
					// in TIME -- rather than accepting two independent existentials
					// or a geometry-only track that never actually animates.
					// Angular spread = 360 minus the largest gap between
					// circularly-sorted per-point angles (a full sweep of N
					// evenly-spaced points measures 360 - 360/N).
					if( op == "param_series_orbit" ) {
						if( !cp.has( "param" ) || !cp.get( "param" ).isString() )
							return { false, "param_series_orbit missing string \"param\"" };
						// All numeric knobs are load-validated (minKeyframes >= 2,
						// minAngularSpreadDeg in (0,360], maxRadiusRatio > 1); these
						// reads are defensive against a hand-built checkpoint.
						const std::string param = cp.get( "param" ).asString();
						const std::string timeParam = ( cp.has( "timeParam" ) && cp.get( "timeParam" ).isString() &&
							!cp.get( "timeParam" ).asString().empty() ) ? cp.get( "timeParam" ).asString() : std::string( "time" );
						const long minKeyframes = ( cp.has( "minKeyframes" ) && cp.get( "minKeyframes" ).isNumber() )
							? static_cast<long>( cp.get( "minKeyframes" ).asNumber() ) : 2;
						const double minSpread = ( cp.has( "minAngularSpreadDeg" ) && cp.get( "minAngularSpreadDeg" ).isNumber() )
							? cp.get( "minAngularSpreadDeg" ).asNumber() : 0.0;
						const bool hasCenter = cp.has( "centerWithin" ) && cp.get( "centerWithin" ).isObject();
						double cwx = 0.0, cwz = 0.0, cwr = 0.0;
						if( hasCenter ) {
							const JsonValue& cw = cp.get( "centerWithin" );
							cwx = cw.get( "x" ).asNumber( 0.0 );
							cwz = cw.get( "z" ).asNumber( 0.0 );
							cwr = cw.get( "radius" ).asNumber( 0.0 );
						}
						const bool hasRatio = cp.has( "maxRadiusRatio" ) && cp.get( "maxRadiusRatio" ).isNumber();
						const double maxRatio = hasRatio ? cp.get( "maxRadiusRatio" ).asNumber() : 0.0;

						const TargetResolution res = resolveTargets( "param_series_orbit" );
						if( !res.ok ) return { false, res.detail };

						auto evalChunk = [&]( const NodeRef& chunk, const std::string& desc ) -> CheckOutcome {
							// Time progression is INTRINSIC to this op -- always
							// enforced, never opt-in.  Collect the RAW `param` and
							// `timeParam` occurrence lists (before any parsing or
							// dedupe) and require them to pair up 1:1: a timeline
							// that omits `time` lines (parser defaults a missing
							// time to 0.0) or carries extras fails right here.
							const std::vector<std::string> rawValues = CheckerChunkParamValues( chunk, param );
							const std::vector<std::string> rawTimes  = CheckerChunkParamValues( chunk, timeParam );
							if( rawTimes.size() != rawValues.size() )
								return { false, "param_series_orbit: " + desc + " has " + std::to_string( rawValues.size() ) +
									" '" + param + "' value(s) but " + std::to_string( rawTimes.size() ) + " '" + timeParam +
									"' entrie(s) -- every keyframe value needs a paired time" };

							// Every time token must parse as a single float.
							std::vector<double> times;
							times.reserve( rawTimes.size() );
							for( std::size_t i = 0; i < rawTimes.size(); ++i ) {
								std::vector<double> tf;
								if( !CheckerParseFloatTokens( rawTimes[i], tf ) || tf.size() != 1 )
									return { false, "param_series_orbit: " + desc + " '" + timeParam + "'[" + std::to_string( i ) +
										"] = \"" + rawTimes[i] + "\" does not parse as a single float" };
								times.push_back( tf[0] );
							}

							// The time sequence must be STRICTLY increasing.  This
							// subsumes the degenerate all-equal-timestamp case (no
							// epsilon needed) and catches duplicate/decreasing/
							// non-progressing sequences alike.
							for( std::size_t i = 1; i < times.size(); ++i ) {
								if( !( times[i - 1] < times[i] ) )
									return { false, "param_series_orbit: " + desc + " '" + timeParam + "' is not strictly "
										"increasing at index " + std::to_string( i - 1 ) + "->" + std::to_string( i ) + " (" +
										std::to_string( times[i - 1] ) + " -> " + std::to_string( times[i] ) + ")" };
							}

							// Collect every `param` value; parse each as an XYZ
							// triple (skip malformed); we grade in the XZ plane.
							struct Pt { double x, y, z; };
							std::vector<Pt> pts;
							for( const std::string& s : rawValues ) {
								std::vector<double> f;
								if( !CheckerParseFloatTokens( s, f ) || f.size() != 3 ) continue;
								pts.push_back( { f[0], f[1], f[2] } );
							}
							// Dedupe CONSECUTIVE duplicates (a closing keyframe that
							// repeats the opening one is NOT consecutive, so it is
							// kept -- matching a closed loop's genuine keyframe count).
							std::vector<Pt> dp;
							for( const Pt& p : pts ) {
								if( dp.empty() || dp.back().x != p.x || dp.back().y != p.y || dp.back().z != p.z )
									dp.push_back( p );
							}
							if( static_cast<long>( dp.size() ) < minKeyframes )
								return { false, "param_series_orbit: " + desc + " has " + std::to_string( dp.size() ) +
									" keyframe(s) after dedupe, need >= " + std::to_string( minKeyframes ) };

							// Center for the angle sweep: centerWithin when given,
							// else the points' XZ centroid.
							double sx = 0.0, sz = 0.0;
							for( const Pt& p : dp ) { sx += p.x; sz += p.z; }
							const double centroidX = sx / static_cast<double>( dp.size() );
							const double centroidZ = sz / static_cast<double>( dp.size() );
							const double cx = hasCenter ? cwx : centroidX;
							const double cz = hasCenter ? cwz : centroidZ;

							// Per-point angle (deg in [0,360)) + radius from center.
							std::vector<double> deg;
							double minDist = -1.0, maxDist = 0.0;
							for( const Pt& p : dp ) {
								const double dx = p.x - cx, dz = p.z - cz;
								double a = std::atan2( dz, dx ) * ( 180.0 / 3.14159265358979323846 );
								if( a < 0.0 ) a += 360.0;
								deg.push_back( a );
								const double d = std::sqrt( dx * dx + dz * dz );
								if( minDist < 0.0 || d < minDist ) minDist = d;
								if( d > maxDist ) maxDist = d;
							}
							std::sort( deg.begin(), deg.end() );
							double largestGap = 0.0;
							for( std::size_t i = 1; i < deg.size(); ++i )
								largestGap = std::max( largestGap, deg[i] - deg[i - 1] );
							largestGap = std::max( largestGap, ( deg.front() + 360.0 ) - deg.back() );   // wrap gap
							const double spread = 360.0 - largestGap;
							if( spread < minSpread )
								return { false, "param_series_orbit: " + desc + " angular spread " + std::to_string( spread ) +
									" deg < required " + std::to_string( minSpread ) + " (" + std::to_string( dp.size() ) + " keyframes)" };

							if( hasCenter ) {
								const double cd = std::sqrt( ( centroidX - cwx ) * ( centroidX - cwx ) +
									( centroidZ - cwz ) * ( centroidZ - cwz ) );
								if( cd > cwr )
									return { false, "param_series_orbit: " + desc + " XZ centroid distance " + std::to_string( cd ) +
										" from center exceeds radius " + std::to_string( cwr ) };
							}
							if( hasRatio ) {
								if( minDist <= 1e-6 )
									return { false, "param_series_orbit: " + desc + " degenerate -- a keyframe coincides with the "
										"center (minDist ~0), radius ratio undefined" };
								const double ratio = maxDist / minDist;
								if( ratio > maxRatio )
									return { false, "param_series_orbit: " + desc + " radius ratio " + std::to_string( ratio ) +
										" exceeds max " + std::to_string( maxRatio ) };
							}
							return { true, "param_series_orbit: " + desc + " -- " + std::to_string( dp.size() ) +
								" keyframes, angular spread " + std::to_string( spread ) + " deg" +
								( hasRatio ? ( ", radius ratio " + std::to_string( maxDist / minDist ) ) : std::string() ) +
								", time span " + std::to_string( times.front() ) + ".." + std::to_string( times.back() ) };
						};
						return runAssertion( "param_series_orbit", res, evalChunk );
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

				//! Shared total-pixel budget for every render/decode allocation
				//! CheckRenderKind drives (its own width*height product AND the
				//! compareToImage reference PNG's decoded w*h -- see
				//! DecodePngToRgb8's guard below).  The per-axis caps CheckRenderKind
				//! already enforces (32768, mirroring Film::kMaxFilmWidth/
				//! kMaxFilmHeight) bound only ONE axis at a time: a 32768x32768
				//! request passes BOTH individually yet demands ~1.07e9 pixels --
				//! tens of GiB across the rasterizer's frame + accumulation
				//! buffers.  64,000,000 (64 "decimal" megapixels, the photography
				//! convention) sits comfortably above any legitimate eval
				//! checkpoint render -- the qHD default is 960x540 (~0.5MP) and
				//! even an 8K UHD frame (7680x4320) is ~33.2MP -- while keeping
				//! the worst-case allocation in the low hundreds of MB rather
				//! than GiB.
				static constexpr double kMaxEvalRenderPixels = 64000000.0;

				//! Finiteness test for JSON numbers that have already been parsed.
				bool IsFiniteRenderNumber( double v )
				{
					return RISE::IsFiniteDouble( v );
				}

				//! Image-reconstruction Wave 2: format an author-supplied
				//! [x,y,z] JSON number array (VALIDATED by ValidateCameraVec3Field
				//! below to be exactly 3 finite numbers) into the "x y z" string
				//! form AgentCameraOverride's Vec3 fields accept (the same form
				//! CameraIntrospection::SetProperty parses).  %.10g keeps full
				//! double precision without a trailing exponent for ordinary
				//! scene coordinates.
				std::string CameraVec3String( const JsonValue& arr )
				{
					char b[128];
					std::snprintf( b, sizeof( b ), "%.10g %.10g %.10g",
						arr.at( 0 ).asNumber(), arr.at( 1 ).asNumber(), arr.at( 2 ).asNumber() );
					return std::string( b );
				}

				//! P2 fix (2026-07-19 re-review): validate a camera vector
				//! FIELD's shape (array of exactly 3 numbers, each finite)
				//! BEFORE CameraVec3String ever touches it.  Mirrors the loader's
				//! requireVec3 (ValidateRenderCheckpointTypes: array-of-3-numbers)
				//! PLUS a finiteness check the loader itself doesn't perform.
				//! This matters here specifically because CheckRenderKind is
				//! reachable DIRECTLY -- CheckScenario/CheckOneCheckpoint can be
				//! driven with a hand-built checkpoint that never passed through
				//! LoadEvalScenario -- so a malformed/non-finite value must be
				//! REFUSED here rather than silently defaulted to 0 by
				//! CameraVec3String's out-of-range .at()/asNumber() fallbacks, or
				//! turned into a literal "inf" token (strtod/sscanf accept the
				//! C99 "inf" spelling) that reaches the renderer's camera-pose
				//! parser.  Returns true on a valid shape; false + `err` (naming
				//! the field) otherwise.
				bool ValidateCameraVec3Field( const JsonValue& arr, const char* fieldName, std::string& err )
				{
					if( !arr.isArray() || arr.size() != 3 ) {
						err = std::string( "render checkpoint: \"camera." ) + fieldName +
							"\" must be an array of exactly 3 numbers (got " +
							( arr.isArray() ? ( std::to_string( arr.size() ) + " element(s)" ) : std::string( JsonTypeName( arr.type() ) ) ) + ")";
						return false;
					}
					for( std::size_t i = 0; i < 3; ++i ) {
						if( !arr.at( i ).isNumber() ) {
							err = std::string( "render checkpoint: \"camera." ) + fieldName + "\"[" +
								std::to_string( i ) + "] must be a number";
							return false;
						}
						const double v = arr.at( i ).asNumber();
						if( !IsFiniteRenderNumber( v ) ) {
							char nb[64];
							std::snprintf( nb, sizeof( nb ), "%.10g", v );
							err = std::string( "render checkpoint: \"camera." ) + fieldName + "\"[" +
								std::to_string( i ) + "] must be a finite number (got " + nb + ")";
							return false;
						}
					}
					return true;
				}

				//! P1 fix (2026-07-19 mutation review): parse width/height
				//! directly from the PNG signature + IHDR chunk header
				//! BEFORE any reader is constructed.  PNGReader::BeginRead
				//! (PNGReader.cpp) does NOT stop at parsing IHDR -- it goes
				//! on to allocate the FULL pixel buffer (`new unsigned
				//! char[nBytes]`), memset it, and decode EVERY scanline via
				//! png_read_row, all before returning w/h to the caller.  A
				//! kMaxEvalRenderPixels check placed after BeginRead (as the
				//! prior version of this function did) therefore lets an
				//! oversized-but-under-libpng's-own-2GiB-cap reference PNG
				//! fully decode -- allocation and all -- before being
				//! refused.  The PNG container's first 24 bytes are
				//! fixed-layout and don't require libpng at all:
				//!   [0..8)   8-byte PNG signature
				//!   [8..12)  IHDR chunk length (big-endian uint32; == 13)
				//!   [12..16) chunk type, must be "IHDR"
				//!   [16..20) width  (big-endian uint32)
				//!   [20..24) height (big-endian uint32)
				//! Returns false (leaving outW/outH at 0) when the buffer is
				//! too short or the signature/tag don't match -- callers
				//! fall through to the existing reader-based path so
				//! non-PNG/malformed/truncated input still produces the
				//! SAME honest decode-failure diagnostic as before, never a
				//! new crash or a false refusal.
				bool ProbePngDimensions( const char* bytes, unsigned int byteCount, unsigned int& outW, unsigned int& outH )
				{
					outW = 0;
					outH = 0;
					static const unsigned char kPngSig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
					if( !bytes || byteCount < 24 )
						return false;
					const unsigned char* u = reinterpret_cast<const unsigned char*>( bytes );
					if( std::memcmp( u, kPngSig, 8 ) != 0 )
						return false;
					if( u[12] != 'I' || u[13] != 'H' || u[14] != 'D' || u[15] != 'R' )
						return false;
					outW = ( static_cast<unsigned int>( u[16] ) << 24 ) | ( static_cast<unsigned int>( u[17] ) << 16 ) |
					       ( static_cast<unsigned int>( u[18] ) << 8 )  |   static_cast<unsigned int>( u[19] );
					outH = ( static_cast<unsigned int>( u[20] ) << 24 ) | ( static_cast<unsigned int>( u[21] ) << 16 ) |
					       ( static_cast<unsigned int>( u[22] ) << 8 )  |   static_cast<unsigned int>( u[23] );
					return true;
				}

				//! Bound a compareToImage file before buffering it.  PNG's IHDR is
				//! also inspected from a fixed 24-byte read so an oversized image
				//! is refused before any compressed payload enters process memory.
				bool ReadReferencePngWithinEvalBudget( const std::string& path,
				                                          std::string& out, std::string& err )
				{
					static constexpr std::uintmax_t kMaxReferenceBytes = 256u * 1024u * 1024u;
					std::error_code ec;
					const std::uintmax_t size = std::filesystem::file_size( path, ec );
					if( ec ) {
						err = "cannot stat file: " + path;
						return false;
					}
					if( size > kMaxReferenceBytes ) {
						err = "reference PNG is " + std::to_string( size ) +
							" bytes, exceeds the " + std::to_string( kMaxReferenceBytes ) + "-byte eval input budget";
						return false;
					}

					std::ifstream f( path.c_str(), std::ios::binary );
					if( !f ) {
						err = "cannot open file: " + path;
						return false;
					}
					char header[24] = {};
					f.read( header, sizeof( header ) );
					const std::streamsize headerBytes = f.gcount();
					unsigned int width = 0, height = 0;
					if( headerBytes == static_cast<std::streamsize>( sizeof( header ) ) &&
						ProbePngDimensions( header, sizeof( header ), width, height ) ) {
						const double pixels = static_cast<double>( width ) * static_cast<double>( height );
						if( pixels > kMaxEvalRenderPixels ) {
							char buf[192];
							std::snprintf( buf, sizeof( buf ),
								"reference PNG is %ux%u = %.0f pixels, exceeds the %.0f-pixel eval render budget",
								width, height, pixels, kMaxEvalRenderPixels );
							err = buf;
							return false;
						}
					}

					f.clear();
					f.seekg( 0, std::ios::beg );
					out.resize( static_cast<std::size_t>( size ) );
					if( !out.empty() ) {
						f.read( &out[0], static_cast<std::streamsize>( out.size() ) );
						if( f.gcount() != static_cast<std::streamsize>( out.size() ) ) {
							err = "could not read complete file: " + path;
							out.clear();
							return false;
						}
					}
					return true;
				}

				//! Image-reconstruction Wave 2: decode PNG bytes already in
				//! memory into a tightly-packed 8-bit RGB pixel buffer (row-
				//! major, 3 bytes/pixel, alpha dropped) through RISE's OWN
				//! The two decoders below read with eColorSpace_Rec709RGB_Linear and
				//! treat the resulting pel channel as the stored byte/255 VERBATIM.
				//! That is only a no-op while RISEPel IS Rec709RGBPel: PNGReader
				//! hardcodes SetFromIntegerized<Rec709RGBPel,...>, whose ColorBase
				//! conversion is a plain copy in that case.  If RISEPel is ever
				//! retyped -- the documented ACEScg migration in
				//! docs/COLOR_SPACE_MIGRATION.md would do exactly that -- the read
				//! silently becomes a real primaries conversion and EVERY decoded
				//! byte shifts, with no compile error and no failing test.  Fail
				//! loudly at compile time instead.
				static_assert( std::is_same<RISEPel, Rec709RGBPel>::value,
					"PNG decode assumes RISEPel == Rec709RGBPel (verbatim byte passthrough). "
					"RISEPel was retyped -- re-derive the decode path in "
					"DecodeReferencePngToRgb8_ / DecodePngToRgb8 before flipping the typedef." );

				//! Scale one decoded [0,1] channel back to its stored 8-bit byte,
				//! ROUNDING to nearest and clamping to [0,255].  Lockstep twin of
				//! AgentSession.cpp's QuantizeDecodedChannel_, whose doc carries
				//! the full rationale (the read side's precomputed-reciprocal
				//! scale makes 24 of the 256 byte values land a hair low, which a
				//! truncating cast then drops an LSB).  `!( d > 0.0 )` rather than
				//! `d <= 0.0` so NaN takes the 0 branch instead of reaching the
				//! cast (casting NaN to an integral type is UB).
				inline unsigned char QuantizeDecodedChannel_( Scalar v )
				{
					const double d = static_cast<double>( v ) * 255.0 + 0.5;
					if( !( d > 0.0 ) ) return 0;     // also catches NaN
					if( d >= 255.0 )   return 255;
					return static_cast<unsigned char>( d );
				}

				//! PNGReader -- the SAME decoder png_painter uses.  We decode
				//! with eColorSpace_Rec709RGB_Linear (the reader's "do nothing"
				//! branch: byte/255 in) and scale back out by ·255 so the EXACT
				//! stored 8-bit byte the writer emitted comes back verbatim --
				//! a pure linear round-trip with NO gamma step, so no
				//! quantization drift is introduced by the read side.  That
				//! last property requires ROUNDING the scale-back (see
				//! QuantizeDecodedChannel_): Integerize's truncating cast
				//! silently biased 24 of the 256 possible byte values one LSB
				//! low, which largely cancels in a candidate-vs-reference
				//! difference but is still wrong.
				//! This is why "compareToImage" compares like-with-like: the
				//! candidate is decode(rr.png) where rr.png is byte-identical to
				//! what read_image returns (InMemoryRasterizerOutput::ToPng),
				//! and the reference was itself written by that same ToPng path.
				//! Returns false (populating `err`) on any decode failure --
				//! empty buffer (missing/unreadable file), non-PNG bytes, or
				//! degenerate dims -- never throws or crashes.  `bytes` must
				//! outlive the call (the MemoryBuffer does NOT take ownership).
				bool DecodePngToRgb8( char* bytes, unsigned int byteCount,
					std::vector<unsigned char>& outRgb, unsigned int& outW, unsigned int& outH,
					std::string& err )
				{
					outRgb.clear();
					outW = 0;
					outH = 0;
					if( !bytes || byteCount == 0 ) {
						err = "empty PNG buffer (missing or unreadable reference)";
						return false;
					}

					// P1 fix (2026-07-19 mutation review): refuse an
					// over-budget reference BEFORE constructing a reader at
					// all, using ProbePngDimensions' header-only parse (see
					// its doc above for why this must happen ahead of
					// BeginRead, not after).  A buffer that doesn't parse as
					// a recognizable PNG header falls through silently --
					// the reader path below still runs and produces the
					// existing "not a valid PNG" diagnostic.
					{
						unsigned int probeW = 0, probeH = 0;
						if( ProbePngDimensions( bytes, byteCount, probeW, probeH ) ) {
							const double totalPixels = static_cast<double>( probeW ) * static_cast<double>( probeH );
							if( totalPixels > kMaxEvalRenderPixels ) {
								char pb[192];
								std::snprintf( pb, sizeof( pb ),
									"reference PNG is %ux%u = %.0f pixels, exceeds the %.0f-pixel eval render budget",
									probeW, probeH, totalPixels, kMaxEvalRenderPixels );
								err = pb;
								return false;
							}
						}
					}

					IMemoryBuffer* buffer = nullptr;
					if( !RISE_API_CreateCompatibleMemoryBuffer( &buffer, bytes, byteCount, /*bTakeOwnership=*/false ) || !buffer ) {
						err = "could not wrap PNG bytes in a read buffer";
						return false;
					}

					IRasterImageReader* reader = nullptr;
					if( !RISE_API_CreatePNGReader( &reader, *buffer, eColorSpace_Rec709RGB_Linear ) || !reader ) {
						buffer->release();
						err = "could not create a PNG reader";
						return false;
					}

					unsigned int w = 0, h = 0;
					if( !reader->BeginRead( w, h ) || w == 0 || h == 0 ) {
						reader->EndRead();
						reader->release();
						buffer->release();
						err = "PNG decode failed (not a valid PNG, or zero dimensions)";
						return false;
					}

					// Belt-and-braces SECOND gate (2026-07-19 mutation review):
					// the PRIMARY guard is the ProbePngDimensions header-only
					// probe above, which runs before the reader is even
					// constructed -- by the time control reaches here,
					// BeginRead has ALREADY allocated the full pixel buffer
					// and decoded every scanline (see ProbePngDimensions'
					// doc; this comment previously and incorrectly claimed
					// BeginRead "has already parsed IHDR without touching
					// pixel data", which is false).  This check stays as a
					// harmless redundant catch for any path that bypasses
					// the header probe (e.g. a PNG whose signature/IHDR the
					// probe couldn't parse but libpng still accepts).
					{
						const double totalPixels = static_cast<double>( w ) * static_cast<double>( h );
						if( totalPixels > kMaxEvalRenderPixels ) {
							reader->EndRead();
							reader->release();
							buffer->release();
							char pb[192];
							std::snprintf( pb, sizeof( pb ),
								"reference PNG is %ux%u = %.0f pixels, exceeds the %.0f-pixel eval render budget",
								w, h, totalPixels, kMaxEvalRenderPixels );
							err = pb;
							return false;
						}
					}

					outRgb.resize( static_cast<std::size_t>( w ) * h * 3 );
					for( unsigned int y = 0; y < h; ++y ) {
						for( unsigned int x = 0; x < w; ++x ) {
							RISEColor c;
							reader->ReadColor( c, x, y );
							// eColorSpace_Rec709RGB_Linear leaves c.base holding
							// the stored byte/255 verbatim.  Scale back by *255
							// with ROUNDING, not Integerize's truncating cast:
							// the read side scales by a precomputed reciprocal
							// (1.0/255.0), so b*OVMax*255.0 lands a hair below b
							// for 24 of the 256 byte values (e.g. 180 ->
							// 179.99999999999997) and a truncating cast then drops
							// those an LSB.  Kept in lockstep with
							// AgentSession.cpp's DecodeReferencePngToRgb8_, whose
							// doc carries the full rationale and the bad-byte list.
							const std::size_t idx = ( static_cast<std::size_t>( y ) * w + x ) * 3;
							outRgb[idx + 0] = QuantizeDecodedChannel_( c.base.r );
							outRgb[idx + 1] = QuantizeDecodedChannel_( c.base.g );
							outRgb[idx + 2] = QuantizeDecodedChannel_( c.base.b );
						}
					}

					reader->EndRead();
					reader->release();
					buffer->release();
					outW = w;
					outH = h;
					return true;
				}

				//! "render": {width?,height?,samples?,seed?,camera?,
				//! meanLumaMin?/Max?,meanRMin?/Max?,meanGMin?/Max?,meanBMin?/Max?,
				//! channelBalanceMax?,compareToImage?,rmseMax?} -- a FRESH render
				//! through the live session (AgentSession::Render, bypassing the
				//! JSON-RPC layer -- the checker is a privileged verifier),
				//! asserted against generous [min,max] bands.
				//!
				//! Image-reconstruction Wave 2 additions:
				//!  * `camera` {location:[x,y,z], lookat:[x,y,z], up?:[x,y,z]
				//!    (default [0,1,0]), fov?:degrees} overrides the ACTIVE
				//!    camera's pose for THIS grading render only (via
				//!    AgentRenderParams::camera) so the render is taken from a
				//!    KNOWN pose regardless of the scene's authored camera.
				//!    location+lookat are required when `camera` is present; fov
				//!    absent = keep the camera's current fov.  Composes with
				//!    width/height/samples and every band above.
				//!  * `samples` (>=1, load-validated) is wired to
				//!    AgentRenderParams::samples so a grading render can use
				//!    enough SPP to stabilise a compareToImage RMSE (honoured by
				//!    the pixel-based rasterizer family; see AgentRenderParams).
				//!  * `compareToImage` (repo-relative .png path) + `rmseMax`
				//!    (in (0,1], both-or-neither) assert an IMAGE similarity:
				//!    RMSE = sqrt(mean over all pixels·RGB of ((a-b)/255)^2) of
				//!    THIS render vs the committed reference PNG, failing iff it
				//!    exceeds rmseMax.  The candidate is decode(rr.png) where
				//!    rr.png is byte-identical to what read_image returns, and
				//!    the reference was written by that SAME render->PNG path, so
				//!    tonemap/clamp/quantization are IDENTICAL on both sides (see
				//!    DecodePngToRgb8's doc).  The render is forced to the
				//!    reference's EXACT dimensions: if width/height are also
				//!    given they MUST equal the reference's dims (a RUNTIME check
				//!    -- the load validator cannot open the file), else the
				//!    reference's dims are adopted.  The measured RMSE is ALWAYS
				//!    reported in the detail (pass or fail) so a calibration run
				//!    can read the actual noise floor.  A missing / non-PNG
				//!    reference is a FAILED checkpoint with a clear detail, never
				//!    a crash.
				//!
				//! `seed` is accepted (never rejected -- an author may want to
				//! note the seed a fixture was authored against) but has NO
				//! EFFECT: AgentSession::Render exposes no seed/RNG-pinning
				//! control, so per the plan's tolerance-banded rule, bands (and
				//! rmseMax) MUST be wide enough to absorb ordinary MC noise
				//! between runs, not narrow enough to require a pinned seed.
				//! NEVER an exact-pixel match.
				//! render-checkpoint dimension/sample-count sanity bound: a JSON
				//! number destined for a NARROWING cast (static_cast<unsigned
				//! int> for width/height, static_cast<int> for samples, just
				//! below) must be a FINITE, non-negative, WHOLE number inside
				//! this range, or the cast is unsafe -- a negative value wraps
				//! to a huge unsigned width/height, a fractional value silently
				//! truncates, and an absurdly large value (e.g. a JSON
				//! double that overflows the target int type) is undefined
				//! behaviour on the cast itself.  Bounds mirror existing
				//! conventions elsewhere in the agent surface: Film::
				//! kMaxFilmWidth/kMaxFilmHeight (32768, Film.h) for width/
				//! height, and the [1,65536] samples-override clamp documented
				//! in AgentRpc.cpp/AgentMcpAdapter.cpp for samples.  Checked
				//! HERE (at the checker, not only at scenario-load time) because
				//! CheckScenario/CheckOneCheckpoint can be driven directly with
				//! a hand-built checkpoint that never passed through
				//! LoadEvalScenario's ValidateRenderCheckpointTypes.
				bool IsWholeNumberInRange( double v, double minVal, double maxVal )
				{
					return IsFiniteRenderNumber( v ) && v == std::floor( v ) && v >= minVal && v <= maxVal;
				}

				//! Format a double for a checkpoint refusal diagnostic (matches
				//! the "%.10g" convention CameraVec3String/camera.fov use
				//! elsewhere in this file for JSON-number values).
				std::string FormatCheckpointNumber( double v )
				{
					char b[64];
					std::snprintf( b, sizeof( b ), "%.10g", v );
					return std::string( b );
				}

				CheckOutcome CheckRenderKind( const JsonValue& cp, AgentSession* session )
				{
					if( !session ) return { false, "render checkpoint: no live session (run did not complete)" };

					AgentRenderParams rp;
					rp.maxPixelCount = static_cast<std::uint64_t>( kMaxEvalRenderPixels );

					static const char* const kThresholdFields[] = {
						"meanLumaMin", "meanLumaMax", "meanRMin", "meanRMax",
						"meanGMin", "meanGMax", "meanBMin", "meanBMax",
						"channelBalanceMax"
					};
					for( const char* field : kThresholdFields ) {
						if( !cp.has( field ) ) continue;
						if( !cp.get( field ).isNumber() ) {
							return { false, "render checkpoint: \"" + std::string( field ) + "\" must be a number (got " +
								std::string( JsonTypeName( cp.get( field ).type() ) ) + ")" };
						}
						if( !IsFiniteRenderNumber( cp.get( field ).asNumber() ) ) {
							return { false, "render checkpoint: \"" + std::string( field ) + "\" must be a finite number (got " +
								FormatCheckpointNumber( cp.get( field ).asNumber() ) + ")" };
						}
					}
					if( cp.has( "channelBalanceMax" ) && !( cp.get( "channelBalanceMax" ).asNumber() > 1.0 ) ) {
						return { false, "render checkpoint: \"channelBalanceMax\" must be > 1.0" };
					}

					// P2 fix (2026-07-19 re-review): width/height MUST be numbers
					// when present -- a present-but-wrong-typed field (e.g.
					// "width":"big") must REFUSE with an honest diagnostic, not
					// silently fall through to the scene's default render size.
					// Previously `haveW`/`haveH` only distinguished "present AND
					// numeric" from "everything else", so a MATCHED pair of
					// wrong-typed width+height (both non-numeric) slipped past the
					// haveW!=haveH mismatch guard below entirely and silently used
					// the default dims.  CheckRenderKind is reachable DIRECTLY --
					// CheckScenario/CheckOneCheckpoint can be driven with a
					// hand-built checkpoint that never passed through
					// LoadEvalScenario's ValidateRenderCheckpointTypes -- so this
					// type check can't be skipped upstream.
					if( cp.has( "width" ) && !cp.get( "width" ).isNumber() )
						return { false, "render checkpoint: \"width\" must be a number (got " +
							std::string( JsonTypeName( cp.get( "width" ).type() ) ) + ")" };
					if( cp.has( "height" ) && !cp.get( "height" ).isNumber() )
						return { false, "render checkpoint: \"height\" must be a number (got " +
							std::string( JsonTypeName( cp.get( "height" ).type() ) ) + ")" };
					const bool haveW = cp.has( "width" );
					const bool haveH = cp.has( "height" );
					if( haveW && haveH ) {
						const double wVal = cp.get( "width" ).asNumber();
						const double hVal = cp.get( "height" ).asNumber();
						if( !IsWholeNumberInRange( wVal, 1.0, 32768.0 ) )
							return { false, "render checkpoint: \"width\" must be a whole number in [1,32768] (got " +
								FormatCheckpointNumber( wVal ) + ")" };
						if( !IsWholeNumberInRange( hVal, 1.0, 32768.0 ) )
							return { false, "render checkpoint: \"height\" must be a whole number in [1,32768] (got " +
								FormatCheckpointNumber( hVal ) + ")" };
						// P1 fix (2026-07-19 re-review): TOTAL-PIXEL budget -- the
						// per-axis caps just above bound only ONE axis at a time;
						// a 32768x32768 request passes BOTH individually yet
						// demands ~1.07e9 pixels (kMaxEvalRenderPixels's doc has
						// the full rationale).  Checked here (the explicit-dims
						// path) BEFORE any allocation; the compareToImage-adopted-
						// dims path below is separately bounded by
						// DecodePngToRgb8's own guard on the reference's header
						// dims.
						const double totalPixels = wVal * hVal;
						if( totalPixels > kMaxEvalRenderPixels ) {
							char pb[224];
							std::snprintf( pb, sizeof( pb ),
								"render checkpoint: \"width\"x\"height\" %.0fx%.0f = %.0f pixels exceeds the %.0f-pixel eval render budget",
								wVal, hVal, totalPixels, kMaxEvalRenderPixels );
							return { false, std::string( pb ) };
						}
						rp.width  = static_cast<unsigned int>( wVal );
						rp.height = static_cast<unsigned int>( hVal );
					} else if( haveW != haveH ) {
						return { false, "render checkpoint: \"width\"/\"height\" must both be supplied together" };
					}

					// Image-reconstruction Wave 2: samples override (>=1, and
					// bounded/whole -- see IsWholeNumberInRange's doc).  P2 fix:
					// a present-but-wrong-typed "samples" must REFUSE (see the
					// width/height note above), not silently skip the override.
					if( cp.has( "samples" ) ) {
						if( !cp.get( "samples" ).isNumber() )
							return { false, "render checkpoint: \"samples\" must be a number (got " +
								std::string( JsonTypeName( cp.get( "samples" ).type() ) ) + ")" };
						const double sVal = cp.get( "samples" ).asNumber();
						if( !IsWholeNumberInRange( sVal, 1.0, 65536.0 ) )
							return { false, "render checkpoint: \"samples\" must be a whole number in [1,65536] (got " +
								FormatCheckpointNumber( sVal ) + ")" };
						rp.samples = static_cast<int>( sVal );
					}

					// Image-reconstruction Wave 2: camera-pose override.  `up`
					// defaults to [0,1,0] (the +Y-up convention) when omitted;
					// `fov` omitted keeps the camera's current fov (hasFov stays
					// false).  P2 fix (2026-07-19 re-review): CheckRenderKind is
					// reachable DIRECTLY (see the width/height note above), so
					// the shape (location+lookat required, each an array of 3
					// numbers), TYPE, and FINITENESS of every vector component
					// and of fov are validated HERE rather than assumed --
					// ValidateRenderCheckpointTypes's requireVec3 mirrors the same
					// finite-number rule on the loader path (ValidateCameraVec3Field's
					// doc has the full rationale for
					// why a non-finite component, e.g. {"location":[0,0,1e999]},
					// must never reach the renderer's camera-pose parser).
					if( cp.has( "camera" ) ) {
						if( !cp.get( "camera" ).isObject() )
							return { false, "render checkpoint: \"camera\" must be an object (got " +
								std::string( JsonTypeName( cp.get( "camera" ).type() ) ) + ")" };
						const JsonValue& cam = cp.get( "camera" );
						if( !cam.has( "location" ) )
							return { false, "render checkpoint: \"camera.location\" is required (an array of 3 numbers)" };
						if( !cam.has( "lookat" ) )
							return { false, "render checkpoint: \"camera.lookat\" is required (an array of 3 numbers)" };
						std::string camErr;
						if( !ValidateCameraVec3Field( cam.get( "location" ), "location", camErr ) ) return { false, camErr };
						if( !ValidateCameraVec3Field( cam.get( "lookat" ),   "lookat",   camErr ) ) return { false, camErr };
						rp.camera.hasLocation = true;
						rp.camera.location    = CameraVec3String( cam.get( "location" ) );
						rp.camera.hasLookAt   = true;
						rp.camera.lookAt      = CameraVec3String( cam.get( "lookat" ) );
						rp.camera.hasUp       = true;
						if( cam.has( "up" ) ) {
							if( !ValidateCameraVec3Field( cam.get( "up" ), "up", camErr ) ) return { false, camErr };
							rp.camera.up = CameraVec3String( cam.get( "up" ) );
						} else {
							rp.camera.up = "0 1 0";
						}
						if( cam.has( "fov" ) ) {
							if( !cam.get( "fov" ).isNumber() )
								return { false, "render checkpoint: \"camera.fov\" must be a number (degrees)" };
							const double fov = cam.get( "fov" ).asNumber();
							if( !IsFiniteRenderNumber( fov ) || !( fov > 0.0 && fov < 180.0 ) )
								return { false, "render checkpoint: \"camera.fov\" must be > 0 and < 180 degrees (got " +
									FormatCheckpointNumber( fov ) + ")" };
							char fbuf[64];
							std::snprintf( fbuf, sizeof( fbuf ), "%.10g", fov );
							rp.camera.hasFov = true;
							rp.camera.fov    = fbuf;
						}
					}

					// Image-reconstruction Wave 2: decode the compareToImage
					// reference FIRST -- its dims drive the grading render (the
					// candidate must be produced at EXACTLY the reference's
					// dimensions).  P2 fix (2026-07-19 re-review): TYPE-check
					// each field when present (refuse, don't silently treat a
					// wrong-typed field as absent) and validate rmseMax's (0,1]
					// range directly here too -- an unbounded rmseMax (e.g.
					// 1e999) would make `rmse > rmseMax` always false below,
					// silently turning compareToImage into a no-op that always
					// "passes" regardless of the actual image difference.
					if( cp.has( "compareToImage" ) && !cp.get( "compareToImage" ).isString() )
						return { false, "render checkpoint: \"compareToImage\" must be a string (got " +
							std::string( JsonTypeName( cp.get( "compareToImage" ).type() ) ) + ")" };
					if( cp.has( "rmseMax" ) && !cp.get( "rmseMax" ).isNumber() )
						return { false, "render checkpoint: \"rmseMax\" must be a number (got " +
							std::string( JsonTypeName( cp.get( "rmseMax" ).type() ) ) + ")" };
					const bool haveCompare = cp.has( "compareToImage" );
					const bool haveRmse    = cp.has( "rmseMax" );
					if( haveCompare != haveRmse )
						return { false, "render checkpoint: \"compareToImage\" and \"rmseMax\" must both be supplied together" };
					std::vector<unsigned char> refRgb;
					unsigned int refW = 0, refH = 0;
					std::string  refPath;
					double       rmseMax = 0.0;
					if( haveCompare ) {
						rmseMax = cp.get( "rmseMax" ).asNumber();
						if( !IsFiniteRenderNumber( rmseMax ) || !( rmseMax > 0.0 && rmseMax <= 1.0 ) )
							return { false, "render checkpoint: \"rmseMax\" must be in (0,1] (got " +
								FormatCheckpointNumber( rmseMax ) + ")" };
						refPath = cp.get( "compareToImage" ).asString();
						if( refPath.empty() )
							return { false, "render checkpoint: \"compareToImage\" must be a non-empty path" };
						std::string refBytes, readErr;
						if( !ReadReferencePngWithinEvalBudget( refPath, refBytes, readErr ) ) {
							return { false, "render checkpoint: compareToImage reference '" + refPath +
								"' could not be read (" + readErr + ")" };
						}
						std::string decErr;
						if( !DecodePngToRgb8( refBytes.empty() ? nullptr : &refBytes[0],
						                      static_cast<unsigned int>( refBytes.size() ),
						                      refRgb, refW, refH, decErr ) ) {
							return { false, "render checkpoint: compareToImage reference '" + refPath +
								"' could not be decoded (" + decErr + ")" };
						}
						// Dimension rule (RUNTIME -- the load validator cannot
						// open the file): an explicit width/height must MATCH the
						// reference; otherwise adopt the reference's dims.
						if( haveW && haveH ) {
							if( rp.width != refW || rp.height != refH ) {
								char db[320];
								std::snprintf( db, sizeof( db ),
									"render checkpoint: compareToImage requested %ux%u but reference '%s' is %ux%u -- dims must match",
									rp.width, rp.height, refPath.c_str(), refW, refH );
								return { false, std::string( db ) };
							}
						} else {
							rp.width  = refW;
							rp.height = refH;
						}
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

					// channelBalanceMax: a name-agnostic "the render is roughly
					// neutral/grey" assertion.  The ratio of the brightest to the
					// dimmest mean channel must not exceed the given cap (> 1.0,
					// load-validated).  The 1e-6 floor guards a near-black channel
					// from producing a divide-by-~0 blow-up.
					if( cp.has( "channelBalanceMax" ) && cp.get( "channelBalanceMax" ).isNumber() ) {
						const double cap = cp.get( "channelBalanceMax" ).asNumber();
						const double hi = std::max( rr.meanR, std::max( rr.meanG, rr.meanB ) );
						const double lo = std::min( rr.meanR, std::min( rr.meanG, rr.meanB ) );
						const double ratio = hi / std::max( lo, 1e-6 );
						if( ratio > cap ) {
							char rbuf[256];
							std::snprintf( rbuf, sizeof( rbuf ),
								"channelBalance ratio %.4f > max %.4f (meanR=%.4f meanG=%.4f meanB=%.4f)",
								ratio, cap, rr.meanR, rr.meanG, rr.meanB );
							failures.push_back( std::string( rbuf ) );
						}
					}

					// Image-reconstruction Wave 2: compareToImage RMSE.  The
					// candidate is decode(rr.png) -- rr.png is byte-identical to
					// what read_image returns, so decoding it back guarantees
					// the same tonemap/clamp/quantization the reference carries.
					// `rmseNote` is built whenever the RMSE is actually computed
					// and is appended to the final detail on BOTH pass and fail
					// (the calibration workflow reads the measured value either
					// way).
					std::string rmseNote;
					if( haveCompare ) {
						std::vector<unsigned char> candBytes = rr.png;   // mutable copy (rr is const; MemoryBuffer wants char*)
						std::vector<unsigned char> candRgb;
						unsigned int candW = 0, candH = 0;
						std::string  decErr;
						if( !DecodePngToRgb8( candBytes.empty() ? nullptr : reinterpret_cast<char*>( &candBytes[0] ),
						                      static_cast<unsigned int>( candBytes.size() ),
						                      candRgb, candW, candH, decErr ) ) {
							failures.push_back( "compareToImage: candidate render PNG could not be decoded (" + decErr + ")" );
						} else if( candW != refW || candH != refH ) {
							char db[320];
							std::snprintf( db, sizeof( db ),
								"compareToImage dimension mismatch: render is %ux%u but reference '%s' is %ux%u",
								candW, candH, refPath.c_str(), refW, refH );
							failures.push_back( std::string( db ) );
						} else {
							const std::size_t n = static_cast<std::size_t>( candW ) * candH * 3;
							double sumSq = 0.0;
							for( std::size_t i = 0; i < n; ++i ) {
								const double diff = ( static_cast<double>( candRgb[i] ) - static_cast<double>( refRgb[i] ) ) / 255.0;
								sumSq += diff * diff;
							}
							const double rmse = ( n > 0 ) ? std::sqrt( sumSq / static_cast<double>( n ) ) : 0.0;
							char nb[256];
							std::snprintf( nb, sizeof( nb ), "compareToImage RMSE=%.6f (max %.6f, ref '%s')",
								rmse, rmseMax, refPath.c_str() );
							rmseNote = nb;
							if( rmse > rmseMax ) {
								char fb[256];
								std::snprintf( fb, sizeof( fb ), "compareToImage RMSE %.6f > max %.6f vs reference '%s'",
									rmse, rmseMax, refPath.c_str() );
								failures.push_back( std::string( fb ) );
							}
						}
					}

					if( !failures.empty() ) {
						std::string detail = "render band(s) violated: ";
						for( std::size_t i = 0; i < failures.size(); ++i ) { if( i ) detail += "; "; detail += failures[i]; }
						if( !rmseNote.empty() ) detail += " [" + rmseNote + "]";
						return { false, detail };
					}
					char buf[256];
					std::snprintf( buf, sizeof( buf ), "render bands satisfied (meanR=%.4f meanG=%.4f meanB=%.4f meanLuma=%.4f)",
						rr.meanR, rr.meanG, rr.meanB, meanLuma );
					std::string detail( buf );
					if( !rmseNote.empty() ) detail += "; " + rmseNote;
					return { true, detail };
				}

				//! "objectmap": {legendContains?,pixelCountFor?,pixelCountMin?,
				//! pixelCountMax?,queryAt?:{x,y,expectName,expectGeometryKind?}} --
				//! legendContains/pixelCountFor run ONE mode:"objectmap" render
				//! (only if needed); queryAt runs AgentSession::QueryObjectAt
				//! (which does its own internal objectmap render regardless).  At
				//! least one of the three assertions must be present.  Optional
				//! queryAt.expectGeometryKind binds the HIT object's `geometry`
				//! param to a chunk of the named kind (e.g. "the centered visible
				//! object is a sphere_geometry"); load-rejected with expectName ""
				//! (a miss has no geometry).
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
							bool identityOk = false;
							if( qr.outOfRange ) {
								failures.push_back( "queryAt(" + std::to_string( x ) + "," + std::to_string( y ) + ") out of range" );
							} else if( expectName == "*" ) {
								// Name-AGNOSTIC hit: expect ANY non-background object at
								// (x,y), whatever it is named.  For open-ended "build a
								// scene" scenarios where the model names entities freely,
								// this proves an object is actually bound + positioned +
								// rendered there without pinning a model-chosen name.
								if( !qr.hit )
									failures.push_back( "queryAt(" + std::to_string( x ) + "," + std::to_string( y ) +
										") expected ANY object (\"*\") but got a miss" );
								else identityOk = true;
							} else if( expectName.empty() ) {
								if( qr.hit ) failures.push_back( "queryAt expected a miss but hit '" + qr.name + "'" );
								else identityOk = true;   // a miss cannot carry a geometry kind (load-rejected combo)
							} else if( !qr.hit || qr.name != expectName ) {
								failures.push_back( "queryAt(" + std::to_string( x ) + "," + std::to_string( y ) + ") expected '" +
									expectName + "', got " + ( qr.hit ? ( "'" + qr.name + "'" ) : std::string( "a miss" ) ) );
							} else {
								identityOk = true;
							}

							// expectGeometryKind: bind the HIT object's `geometry`
							// param to a chunk of the named kind -- proves the visible
							// object is actually (e.g.) a sphere, killing a
							// detached-geometry + wrong-centered-object dodge.  Only
							// meaningful on a real hit; load validation rejects it
							// paired with expectName "" (expect-miss).
							if( identityOk && qr.hit && qa.has( "expectGeometryKind" ) &&
							    qa.get( "expectGeometryKind" ).isString() ) {
								const std::string wantGeomKind = qa.get( "expectGeometryKind" ).asString();
								const Document odoc = RISE::Cst::ParseToCst( session->ReadDocument() );
								NodeRef objChunk;
								const int of = CheckerFindChunk( odoc, std::string(), qr.name, objChunk );
								if( of != 1 ) {
									failures.push_back( "queryAt expectGeometryKind: hit object '" + qr.name +
										"' is not a uniquely-named chunk in the document" );
								} else {
									std::string geomName;
									if( !CheckerChunkParamValue( objChunk, "geometry", geomName ) || geomName.empty() ) {
										failures.push_back( "queryAt expectGeometryKind: hit object '" + qr.name +
											"' has no 'geometry' binding" );
									} else {
										NodeRef geomChunk;
										const int gf = CheckerFindChunk( odoc, wantGeomKind, geomName, geomChunk );
										if( gf < 0 ) {
											failures.push_back( "queryAt expectGeometryKind: geometry '" + geomName +
												"' matches more than one '" + wantGeomKind + "' chunk" );
										} else if( gf == 0 ) {
											NodeRef anyGeom;
											const int af = CheckerFindChunk( odoc, std::string(), geomName, anyGeom );
											const std::string actualKind = ( af == 1 && anyGeom ) ? anyGeom->role : std::string( "<no such chunk>" );
											failures.push_back( "queryAt expectGeometryKind: hit object '" + qr.name + "' geometry '" +
												geomName + "' is a '" + actualKind + "', expected a '" + wantGeomKind + "'" );
										}
									}
								}
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
				//! expectAutonomyRefusal?,toolOutcomes?,toolCallAfterUserTurn?,
				//! askUserMin?,askUserMax?,askUserBeforeMutation?}
				//! -- maxToolCalls/maxLlmCalls/terminalStatus read straight off
				//! handle.result (the SAME counters RunScenario wrote into the
				//! summary line -- no need to re-derive them from the JSONL).
				//! noAutonomyRefusal/requiredToolInOrder/noMechanicalLoop/
				//! expectAutonomyRefusal/toolOutcomes parse handle.trajectoryPath's
				//! "tool" records (name/args/jsonrpc.response), since those need
				//! per-call detail the summary doesn't carry; toolCallAfterUserTurn
				//! additionally walks the full ORDERED record stream (interleaving
				//! "user" and "tool" records) to test a tool call against a running
				//! user-turn count.  askUserMin/askUserMax (stage 2 of the
				//! clarifying-questions feature) count "ask_user" tool records;
				//! askUserBeforeMutation asserts the FIRST "ask_user" record
				//! precedes the FIRST document-mutating tool record (insert_chunk/
				//! insert_chunks/propose_patch/remove_chunk -- the same mutation
				//! set AgentChatLoop::AddToolResult's blind-edit nudge tracks).
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
						cp.has( "toolOutcomes" ) || cp.has( "toolCallAfterUserTurn" ) ||
						cp.has( "askUserMin" ) || cp.has( "askUserMax" ) || cp.has( "askUserBeforeMutation" );
					if( needsRecords ) {
						if( handle.trajectoryPath.empty() ) {
							failures.push_back( "trajectory file unavailable (run did not complete) -- cannot check "
								"noAutonomyRefusal/requiredToolInOrder/noMechanicalLoop/expectAutonomyRefusal/"
								"toolOutcomes/toolCallAfterUserTurn/askUserMin/askUserMax/askUserBeforeMutation" );
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

								// Shared between toolOutcomes and
								// toolCallAfterUserTurn so the two can never diverge on
								// outcome semantics: "applied" tests result.applied==true;
								// "staged"/"rejected"/"conflict" test result.status;
								// "error" tests the envelope carries an "error" object.  An
								// unrecognized expect never matches.
								auto satisfiesExpect = [&]( const JsonValue& t, const std::string& expect ) -> bool {
									const JsonValue env = ExtractToolResponseEnvelope( t );
									if( !env.isObject() ) return false;
									if( expect == "error" ) return env.has( "error" );
									const JsonValue& result = env.get( "result" );
									if( expect == "applied" )  return result.isObject() && result.get( "applied" ).asBool( false );
									if( expect == "staged" )   return result.isObject() && result.get( "status" ).asString() == "staged";
									if( expect == "rejected" ) return result.isObject() && result.get( "status" ).asString() == "rejected";
									if( expect == "conflict" ) return result.isObject() && result.get( "status" ).asString() == "conflict";
									return false;
								};

								// The optional argsContains filter accepts EITHER a single
								// string OR an ARRAY of strings; a record matches iff EVERY
								// listed substring occurs in its serialized args (absent ->
								// vacuously matches).  Shared by both toolOutcomes and
								// toolCallAfterUserTurn.
								auto argsContainsMatch = []( const JsonValue& spec, const std::string& haystack ) -> bool {
									if( !spec.has( "argsContains" ) ) return true;
									const JsonValue& ac = spec.get( "argsContains" );
									if( ac.isString() ) return haystack.find( ac.asString() ) != std::string::npos;
									if( ac.isArray() ) {
										for( std::size_t i = 0; i < ac.size(); ++i ) {
											if( !ac.at( i ).isString() ) continue;
											if( haystack.find( ac.at( i ).asString() ) == std::string::npos ) return false;
										}
										return true;
									}
									return true;
								};
								// The human-readable filter note for a spec's argsContains
								// (spec-only, independent of any record) -- named in details.
								auto argsContainsNote = []( const JsonValue& spec ) -> std::string {
									if( !spec.has( "argsContains" ) ) return std::string();
									const JsonValue& ac = spec.get( "argsContains" );
									if( ac.isString() ) return " [args contains '" + ac.asString() + "']";
									if( ac.isArray() ) {
										std::string n = " [args contains all of";
										for( std::size_t i = 0; i < ac.size(); ++i )
											if( ac.at( i ).isString() ) n += " '" + ac.at( i ).asString() + "'";
										n += "]";
										return n;
									}
									return std::string();
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

								// askUserMin/askUserMax (stage 2 of the clarifying-
								// questions feature): count "ask_user" tool records
								// in the trajectory, one shared pass over toolRecords
								// so the two bounds can never disagree on what they
								// counted.
								if( cp.has( "askUserMin" ) || cp.has( "askUserMax" ) ) {
									long long askUserCount = 0;
									for( const auto& t : toolRecords )
										if( t.get( "name" ).asString() == "ask_user" ) ++askUserCount;

									if( cp.has( "askUserMin" ) && cp.get( "askUserMin" ).isNumber() ) {
										const long long wantMin = static_cast<long long>( cp.get( "askUserMin" ).asNumber() );
										if( askUserCount < wantMin )
											failures.push_back( "askUserMin: observed " + std::to_string( askUserCount ) +
												" ask_user call(s), want >= " + std::to_string( wantMin ) );
									}
									if( cp.has( "askUserMax" ) && cp.get( "askUserMax" ).isNumber() ) {
										const long long wantMax = static_cast<long long>( cp.get( "askUserMax" ).asNumber() );
										if( askUserCount > wantMax )
											failures.push_back( "askUserMax: observed " + std::to_string( askUserCount ) +
												" ask_user call(s), want <= " + std::to_string( wantMax ) );
									}
								}

								// askUserBeforeMutation: the FIRST "ask_user" record
								// must precede the FIRST document-mutating tool
								// record ("asked before building") -- mirrors the
								// blind-edit-nudge mutation set AgentChatLoop::
								// AddToolResult already tracks (insert_chunk/
								// insert_chunks/propose_patch/remove_chunk).
								if( cp.has( "askUserBeforeMutation" ) && cp.get( "askUserBeforeMutation" ).isBool() &&
								    cp.get( "askUserBeforeMutation" ).asBool() ) {
									static const char* const kMutatingToolNames[] = {
										"insert_chunk", "insert_chunks", "propose_patch", "remove_chunk"
									};
									auto isMutatingTool = [&]( const std::string& name ) -> bool {
										for( const char* m : kMutatingToolNames ) if( name == m ) return true;
										return false;
									};
									std::size_t firstAskUserIdx  = toolRecords.size();
									std::size_t firstMutationIdx = toolRecords.size();
									for( std::size_t ti = 0; ti < toolRecords.size(); ++ti ) {
										const std::string name = toolRecords[ti].get( "name" ).asString();
										if( firstAskUserIdx == toolRecords.size() && name == "ask_user" ) firstAskUserIdx = ti;
										if( firstMutationIdx == toolRecords.size() && isMutatingTool( name ) ) firstMutationIdx = ti;
									}
									// VACUOUS-PASS SEMANTIC (deliberate, red-proven in
									// TestTrajectoryAskUserAssertions): a trajectory with an
									// ask_user call and ZERO document-mutating calls PASSES --
									// "asked before mutating" is trivially satisfied when
									// nothing ever mutated.  A scenario using this field pairs
									// it with chunk_count, so a build-nothing run still fails
									// the battery overall; this field grades ORDERING only.
									if( firstAskUserIdx == toolRecords.size() ) {
										failures.push_back( "askUserBeforeMutation: no ask_user tool call occurred" );
									} else if( firstMutationIdx != toolRecords.size() && firstAskUserIdx > firstMutationIdx ) {
										failures.push_back( "askUserBeforeMutation: first ask_user call (tool-call position " +
											std::to_string( firstAskUserIdx ) + ") occurred AFTER the first document-mutating "
											"tool call (position " + std::to_string( firstMutationIdx ) + ")" );
									}
								}

								if( cp.has( "toolOutcomes" ) && cp.get( "toolOutcomes" ).isArray() ) {
									const JsonValue& specs = cp.get( "toolOutcomes" );
									for( std::size_t si = 0; si < specs.size(); ++si ) {
										const JsonValue& spec = specs.at( si );
										if( !spec.isObject() ) continue;   // load-time-validated shape; defensive skip only
										const std::string wantName = spec.get( "name" ).asString();
										const std::string occurrence = spec.has( "occurrence" ) && spec.get( "occurrence" ).isString()
											? spec.get( "occurrence" ).asString() : "any";
										const std::string expect = spec.get( "expect" ).asString();
										// argsContains (optional, load-validated non-empty string
										// OR non-empty array of non-empty strings): FILTERS which
										// same-named records the spec selects among -- a record
										// matches iff name matches AND its serialized args contain
										// EVERY listed substring.
										const std::string filterNote = argsContainsNote( spec );

										std::vector<const JsonValue*> matches;
										for( const auto& t : toolRecords ) {
											if( t.get( "name" ).asString() != wantName ) continue;
											if( !argsContainsMatch( spec, JsonSerialize( t.get( "args" ) ) ) ) continue;
											matches.push_back( &t );
										}

										if( matches.empty() ) {
											failures.push_back( "toolOutcomes[" + std::to_string( si ) + "]: no tool call named '" +
												wantName + "'" + filterNote + " occurred" );
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
											failures.push_back( "toolOutcomes[" + std::to_string( si ) + "]: '" + wantName + "'" + filterNote +
												" (" + occurrence + ") did not satisfy expect:'" + expect + "'" );
									}
								}

								if( cp.has( "toolCallAfterUserTurn" ) &&
								    ( cp.get( "toolCallAfterUserTurn" ).isObject() || cp.get( "toolCallAfterUserTurn" ).isArray() ) ) {
									// Accept the single-OBJECT (back-compat) form or an
									// ARRAY of specs; normalize into one spec list so the
									// per-spec walk is identical either way.
									const JsonValue& raw = cp.get( "toolCallAfterUserTurn" );
									std::vector<const JsonValue*> tcSpecs;
									if( raw.isObject() ) tcSpecs.push_back( &raw );
									else for( std::size_t i = 0; i < raw.size(); ++i ) tcSpecs.push_back( &raw.at( i ) );

									for( std::size_t si = 0; si < tcSpecs.size(); ++si ) {
										const JsonValue& spec = *tcSpecs[si];
										if( !spec.isObject() ) continue;   // load-time-validated shape; defensive skip only
										const std::string specLabel = raw.isObject()
											? std::string( "toolCallAfterUserTurn" )
											: ( "toolCallAfterUserTurn[" + std::to_string( si ) + "]" );
										const std::string wantName = spec.get( "name" ).asString();
										const bool hasMin = spec.has( "minUserTurns" ) && spec.get( "minUserTurns" ).isNumber();
										const bool hasMax = spec.has( "maxUserTurns" ) && spec.get( "maxUserTurns" ).isNumber();
										const long long minUserTurns = hasMin ? static_cast<long long>( spec.get( "minUserTurns" ).asNumber() ) : 0;
										const long long maxUserTurns = hasMax ? static_cast<long long>( spec.get( "maxUserTurns" ).asNumber() ) : 0;
										const std::string filterNote = argsContainsNote( spec );
										// Optional expect: the matching call must ALSO satisfy the
										// outcome (same enum as toolOutcomes) -- an early rejected/
										// errored edit no longer counts as "the edit happened here".
										const bool hasExpect = spec.has( "expect" ) && spec.get( "expect" ).isString();
										const std::string expect = hasExpect ? spec.get( "expect" ).asString() : std::string();

										// PASS iff SOME matching (name + argsContains + expect) tool
										// record occurs at a running user-turn count that is
										// >= min (when given) AND <= max (when given).
										long long userCount = 0;
										bool found = false;
										for( const auto& v : allRecords ) {
											const std::string rt = v.get( "run_type" ).asString();
											if( rt == "user" ) { ++userCount; continue; }
											if( rt != "tool" || v.get( "name" ).asString() != wantName ) continue;
											if( !argsContainsMatch( spec, JsonSerialize( v.get( "args" ) ) ) ) continue;
											if( hasMin && userCount < minUserTurns ) continue;
											if( hasMax && userCount > maxUserTurns ) continue;
											if( hasExpect && !satisfiesExpect( v, expect ) ) continue;
											found = true;
											break;
										}
										if( !found ) {
											std::string bound;
											if( hasMin ) bound += "userTurns >= " + std::to_string( minUserTurns );
											if( hasMin && hasMax ) bound += " and ";
											if( hasMax ) bound += "userTurns <= " + std::to_string( maxUserTurns );
											if( hasExpect ) bound += std::string( bound.empty() ? "" : " " ) + "with outcome '" + expect + "'";
											failures.push_back( specLabel + ": no tool call '" + wantName + "'" + filterNote +
												" occurred at " + bound );
										}
									}
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

				//! "proposal": {countMin?,countMax?,match?:{kindIs?,target?,param?,
				//! status?,valueMin?:[...],valueMax?:[...]}} -- asserts on the live
				//! session's staged-proposal queue (AgentSession::ListProposals).
				//! Under autonomy:"propose" the headless mock-Owner makes this a
				//! REAL queue; a read/commit run (no controller) returns an empty
				//! list.  countMin/countMax bound the TOTAL entries; `match`
				//! requires at least one entry satisfying ALL of its given fields
				//! (valueMin/valueMax grade the entry's whitespace-tokenized value
				//! floats component-wise, requiring the token count to equal the
				//! band length -- the same tolerance shape as param_range).  A
				//! null session is a FAILED checkpoint (the standard "no live
				//! session" detail), never a silent pass.
				CheckOutcome CheckProposalKind( const JsonValue& cp, AgentSession* session )
				{
					if( !session ) return { false, "proposal checkpoint: no live session (run did not complete)" };
					const std::vector<AgentSession::AgentProposalEntry> proposals = session->ListProposals();

					std::vector<std::string> failures;

					// Total-count band.
					if( cp.has( "countMin" ) && cp.get( "countMin" ).isNumber() ) {
						const double lo = cp.get( "countMin" ).asNumber();
						if( static_cast<double>( proposals.size() ) < lo )
							failures.push_back( "proposal count " + std::to_string( proposals.size() ) + " < countMin " + std::to_string( lo ) );
					}
					if( cp.has( "countMax" ) && cp.get( "countMax" ).isNumber() ) {
						const double hi = cp.get( "countMax" ).asNumber();
						if( static_cast<double>( proposals.size() ) > hi )
							failures.push_back( "proposal count " + std::to_string( proposals.size() ) + " > countMax " + std::to_string( hi ) );
					}

					// match: at least one entry satisfies ALL given fields.
					if( cp.has( "match" ) && cp.get( "match" ).isObject() ) {
						const JsonValue& m = cp.get( "match" );
						auto optStr = [&]( const char* key ) -> std::string {
							return ( m.has( key ) && m.get( key ).isString() ) ? m.get( key ).asString() : std::string();
						};
						const std::string wantKind   = optStr( "kindIs" );
						const std::string wantTarget = optStr( "target" );
						const std::string wantParam  = optStr( "param" );
						const std::string wantStatus = optStr( "status" );
						const bool hasValueMin = m.has( "valueMin" ) && m.get( "valueMin" ).isArray();
						const bool hasValueMax = m.has( "valueMax" ) && m.get( "valueMax" ).isArray();

						// Parse the numeric bands up-front (a shape error is a loud
						// rubric failure, not a silent miss).
						std::vector<double> vmin, vmax;
						if( hasValueMin ) {
							const JsonValue& a = m.get( "valueMin" );
							for( std::size_t i = 0; i < a.size(); ++i ) {
								if( !a.at( i ).isNumber() )
									return { false, "proposal: match.valueMin[" + std::to_string( i ) + "] is not a number" };
								vmin.push_back( a.at( i ).asNumber() );
							}
						}
						if( hasValueMax ) {
							const JsonValue& a = m.get( "valueMax" );
							for( std::size_t i = 0; i < a.size(); ++i ) {
								if( !a.at( i ).isNumber() )
									return { false, "proposal: match.valueMax[" + std::to_string( i ) + "] is not a number" };
								vmax.push_back( a.at( i ).asNumber() );
							}
						}
						if( hasValueMin && hasValueMax && vmin.size() != vmax.size() )
							return { false, "proposal: match.valueMin has " + std::to_string( vmin.size() ) +
								" component(s) but match.valueMax has " + std::to_string( vmax.size() ) + " -- they must match" };

						bool anyMatch = false;
						for( const AgentSession::AgentProposalEntry& p : proposals ) {
							if( !wantKind.empty()   && p.kind   != wantKind )   continue;
							if( !wantTarget.empty() && p.target != wantTarget ) continue;
							if( !wantParam.empty()  && p.param  != wantParam )  continue;
							if( !wantStatus.empty() && p.status != wantStatus ) continue;
							if( hasValueMin || hasValueMax ) {
								std::vector<double> pv;
								if( !CheckerParseFloatTokens( p.value, pv ) ) continue;
								const std::size_t bandLen = hasValueMin ? vmin.size() : vmax.size();
								if( pv.size() != bandLen ) continue;
								bool inBand = true;
								for( std::size_t i = 0; i < pv.size(); ++i ) {
									if( hasValueMin && pv[i] < vmin[i] ) { inBand = false; break; }
									if( hasValueMax && pv[i] > vmax[i] ) { inBand = false; break; }
								}
								if( !inBand ) continue;
							}
							anyMatch = true;
							break;
						}
						if( !anyMatch )
							failures.push_back( "proposal: no staged entry (of " + std::to_string( proposals.size() ) +
								") satisfied all match fields" );
					}

					if( !failures.empty() ) {
						std::string detail = "proposal checkpoint violated: ";
						for( std::size_t i = 0; i < failures.size(); ++i ) { if( i ) detail += "; "; detail += failures[i]; }
						return { false, detail };
					}
					return { true, "proposal assertion(s) satisfied (" + std::to_string( proposals.size() ) + " staged)" };
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
					if( kind == "proposal" )    return CheckProposalKind( cp, session );

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
