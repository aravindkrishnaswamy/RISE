//////////////////////////////////////////////////////////////////////
//
//  AsciiSceneParser.cpp - Implementation of the AsciiSceneParser
//    class plus every concrete IAsciiChunkParser subclass.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 22, 2003
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
//
//  ARCHITECTURE — descriptor-driven chunk parsing (April 2026)
//
//  Every chunk parser in this file derives from `IAsciiChunkParser`
//  and overrides exactly TWO virtual methods:
//
//    1. `Describe()` returns a `ChunkDescriptor` enumerating every
//       parameter the chunk accepts (name, kind, enum values,
//       reference categories, defaults, descriptions).  This
//       descriptor IS the parser's accepted-parameter set.
//
//    2. `Finalize(const ParseStateBag& bag, IJob& pJob)` reads
//       typed values out of the bag and emits the corresponding
//       `pJob.AddX(...)` / `pJob.SetX(...)` call.
//
//  No chunk parser overrides `ParseChunk` directly.  The default
//  `ParseChunk` impl (defined below, just after `CreateAllChunkParsers`)
//  walks the input lines, validates each name against
//  `Describe().parameters`, stores matched values in a `ParseStateBag`,
//  then invokes `Finalize` to emit the AddX call.  An input parameter
//  whose name is not in the descriptor fails the parse — exactly the
//  same behaviour as the legacy hand-rolled `if/else` chain's else
//  branch, but driven from a single source of truth.
//
//  THE INVARIANT this enforces:  drift between "what the parser
//  parses" and "what the descriptor advertises" is structurally
//  impossible.  The same descriptor feeds:
//    - the parser (via `DispatchChunkParameters` below)
//    - the syntax highlighters (Qt + AppKit, via `SceneGrammar`)
//    - the scene-editor suggestion engine (right-click menu and
//      inline autocomplete in both GUI apps)
//    - any future grammar consumer (linters, doc generators, …).
//
//  Adding a new chunk parser:
//
//    1. Define `struct YourAsciiChunkParser : public IAsciiChunkParser`
//       inside the `RISE::Implementation::ChunkParsers` namespace,
//       with `Describe()` (static `ChunkDescriptor` constructed via the
//       `auto P = [&cd]() -> ParameterDescriptor& { ... }` lambda
//       idiom) and `Finalize(const ParseStateBag&, IJob&) const override`.
//    2. Register it in `CreateAllChunkParsers()` further down in this
//       file — `add("your_chunk_keyword", new YourAsciiChunkParser());`.
//    3. The Library build will fail until both `Describe()` AND
//       `Finalize()` are implemented (both are pure-virtual on
//       `IAsciiChunkParser`).
//    4. The chunk now appears automatically in:
//          - syntax highlighting on Windows + macOS
//          - the right-click context menu and inline autocomplete
//          - any future grammar consumer.
//       No second site to update.
//
//  Adding a parameter to an existing chunk:
//
//    1. Add one entry to that chunk's `Describe()` parameter list:
//          { auto& p = P(); p.name = "..."; p.kind = ValueKind::...;
//            p.description = "..."; p.defaultValueHint = "..."; }
//       (set `p.repeatable = true` for repeatable params, populate
//       `p.enumValues` for `ValueKind::Enum`, populate
//       `p.referenceCategories` for `ValueKind::Reference`).
//    2. Read the new value in `Finalize`:
//          double x = bag.GetDouble("...", default_value);
//       (or `GetString` / `GetUInt` / `GetBool` / `GetVec3` /
//       `GetRepeatable` as appropriate.)  Pass the same default the
//       legacy code used as the local-variable initial value.
//    3. Pass it to the appropriate `pJob.AddX(...)` / `pJob.SetX(...)`
//       overload.  No third site to update.
//
//  Removing a parameter from an existing chunk:
//
//    1. Delete the descriptor entry.  Done.  Every consumer updates
//       in lock-step.  If scene-file backwards compatibility is
//       required, leave the entry but mark it "Legacy — ignored" in
//       the description and skip reading it in `Finalize`.
//
//  Helper templates that bundle parameter sets shared across many
//  chunks (`AddCameraCommonParams`, `AddSpectralCoreParams`,
//  `AddPhotonMapGenerateCommonParams`, …) live in this file just
//  after `DispatchChunkParameters` and just before the painter
//  parsers; reuse them rather than copy-pasting parameter lists.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include <vector>
#include <map>
#include <set>
#include <stack>
#include <string>
#include <sstream>
#include <algorithm>
#include <cstring>   // Phase 6.2: strstr for sentinel detection
#include <cstdio>    // Phase 6.2: sscanf in OnOverrideObjectFinalized
#include <sys/types.h>
#include <sys/stat.h>
#include "AsciiSceneParser.h"
#include "AsciiCommandParser.h"
#include "IAsciiChunkParser.h"
#include "ChunkParserRegistry.h"
#include "StdOutProgress.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/OrthonormalBasis3D.h"
#include "../Utilities/MediaPathLocator.h"
#include "../Sampling/HaltonPoints.h"
#include "MathExpressionEvaluator.h"
#include "../Utilities/RasterizerDefaults.h"
#include "../Rendering/Film.h"		// kDefaultFilm* constants for `film` chunk
#include "../RISE_API.h"				// IScalarPainter constructors (Phase 2)
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/IScalarPainterManager.h"
#include "../Interfaces/IFunction1DManager.h"
#include "../Interfaces/IFunction2DManager.h"
#include "../Interfaces/IModifierManager.h"	// bumpmap_modifier normalize_gradient path (via IJobPriv::GetModifiers)
#include "../Painters/RGBScalarPainter.h"		// for ScalarTriple::IsUniform et al
#include "../Painters/TexturePainter.h"		// for resolving a named image painter -> raster accessor (scalar_painter texture form)
// Phase 6.1: SourceSpanIndex + TransformSnapshot live behind IJobPriv
// (forward-declared in IJobPriv.h), full defs needed for population calls.
#include "../Interfaces/IJobPriv.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/IObjectPriv.h"
#include "../SceneEditor/SourceSpanIndex.h"
#include "../SceneEditor/TransformSnapshot.h"
#include "../SceneEditor/OverrideSpanIndex.h"
// Phase B: descriptor-driven introspection used by
// PopulateLoadedPropertySnapshot to capture loaded parameter values.
#include "../SceneEditor/CameraIntrospection.h"
#include "../SceneEditor/LightIntrospection.h"
#include "../SceneEditor/MaterialIntrospection.h"
#include "../SceneEditor/MediaIntrospection.h"
#include "../SceneEditor/ObjectIntrospection.h"
#include "../Interfaces/ICameraManager.h"
#include "../Interfaces/ILightManager.h"
#include "../Interfaces/IMaterialManager.h"

#ifdef WIN32
#include <malloc.h>
#else
#include <alloca.h>
#endif

using namespace RISE;
using namespace RISE::Implementation;

#define MAX_CHARS_PER_LINE		8192
#define CURRENT_SCENE_VERSION	6

// std_string_npos no longer needed - using std::string::npos directly
// `mh` is the LIVE hal(d) QMC consumer: evaluate_first_function_in_expression
// below advances it via next_halton().  It is Reset() per TOP-LEVEL parse in
// ParseAndLoadScene (see the isTopLevel-gated mh.Reset() there), so each
// standalone LoadAsciiScene evaluates hal() from a fresh sequence and sample
// state does not leak across top-level loads in one process.  This is the ONLY
// MultiHalton in the parser code; ChunkParserRegistry.cpp does NOT keep a copy.
static MultiHalton mh;

inline bool string_split( const String& s, String& first, String& second, const char ch )
{
	String::const_iterator it = std::find( s.begin(), s.end(), ch );
	if( it==s.end() ) {
		return false;
	}

	first = String( s.begin(), it );
	second = String( it+1, s.end() );
	return true;
}

inline void make_string_from_tokens( String& s, String* tokens, const unsigned int num_tokens, const char* ch )
{
	// Concatenate all the tokens together with the given character between each
	// of the tokens

	if( num_tokens < 1 ) {
		return;
	}

	s.clear();
	s.concatenate(tokens[0]);

	for( unsigned int i=1; i<num_tokens; i++ ) {
		s.concatenate( ch );
		s.concatenate( tokens[i] );
	}
}

inline char evaluate_first_function_in_expression( String& token )
{
	std::string str( token.c_str() );
	std::string processed;

	std::string::size_type x = str.find_first_of( "scth" );

	if( x == std::string::npos ) {
		return 0;
	}

	if( x > 0 ) {
		processed = str.substr( 0, x );
		str = str.substr( x, str.size()-1 );
	}

	x = str.find_first_of( "(" );
	if( x == std::string::npos ) {
		return 2;
	}

	std::string::size_type y = str.find_first_of( ")" );
	if( y == std::string::npos ) {
		return 2;
	}

	// Take the expression from to y and evaluate it
	std::string szexpr = str.substr( x, y-x+1 );

	MathExpressionEvaluator::Expression expr( szexpr.c_str() );
	if( expr.error() ) {
		return 2;
	}

	Scalar val = 0;

	switch( str[0] )
	{
	case 's':
		// Sin
		if( str[1] == 'i' && str[2] == 'n' ) {
			val = sin( expr.eval() );
		} else if( str[1] == 'q' && str[2] == 'r' && str[3] == 't' ) {
			val = sqrt( expr.eval() );
		} else {
			return 2;
		}
		break;
	case 'c':
		// Cos
		if( str[1] == 'o' && str[2] == 's' ) {
			val = cos( expr.eval() );
		} else {
			return 2;
		}
		break;
	case 't':
		// Tan
		if( str[1] == 'a' && str[2] == 'n' ) {
			val = tan( expr.eval() );
		} else {
			return 2;
		}
		break;
	case 'h':
		// Halton random number sequence
		if( str[1] == 'a' && str[2] == 'l' ) {
			const int d = int(expr.eval());
			if( d < 0 || d >= QMC_NUM_PRIMES ) {
				GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: hal(d): dimension d=%d is out of range [0, %d); clamp or pick a smaller value", d, QMC_NUM_PRIMES );
				return 2;
			}
			val = mh.next_halton(d);
		} else {
			return 2;
		}
		break;
	}

	// assemble together
	static const unsigned int MAX_CHARS = 512;
	char evaluated[MAX_CHARS] = {0};
	snprintf( evaluated, MAX_CHARS, "%.17f", val );

	processed.append( evaluated );
	processed.append( str.substr( y+1, str.length()-1 ) );

	token = String(processed.c_str());

	return 1;
}

inline bool evaluate_functions_in_expression( String& token )
{
	for(;;) {
		char c = evaluate_first_function_in_expression( token );

		if( c==0 ) {
			return true;
		}

		if( c==2 ) {
			return false;
		}
	}
}

inline bool evaluate_expression( String& token )
{
	// The definition of an expression is very simple
	//   All it is is a sequence of numbers seperated by either a +, -, / or *
	//   Brackets may be used to ensure processing order
	//
	// All expressions are evaluated as double precision floating point
    //
	// Expressions must be in the form $(expr)

	// Before evaluating the expression, we should first go through and
	//   evaluate all the functional stuff like sin, cos, tan, sqrt, etc

	if( token.size() <= 4 ) {
		return false;
	}

	if( token[0] != '$' ||
		token[1] != '(' ||
		token[strlen(token.c_str())-1] != ')' ) {
		return false;
	}

	if( !evaluate_functions_in_expression( token ) ) {
		return false;
	}

	// We clamp out string
	const char * str = token.c_str();
	char* s = (char*)&str[1];

	MathExpressionEvaluator::Expression expr( s );
	if( expr.error() ) {
		return false;
	}

	static const unsigned int MAX_CHARS = 512;
	char evaluated[MAX_CHARS] = {0};
	snprintf( evaluated, MAX_CHARS, "%.17f", expr.eval() );

	token = String(evaluated);
	return true;
}

inline bool evaluate_expressions_in_tokens( String* tokens, const unsigned int num_tokens )
{
	for( unsigned int i=0; i<num_tokens; i++ ) {
		// Check to see if we have an expression
		if( tokens[i][0] == '$' ) {
			// This token contains an expression
			if( !evaluate_expression( tokens[i] ) ) {
				return false;
			}
		}
	}

	return true;
}

// Phase 6.2 helper: build a RISE Matrix4 from a column-major 16-double
// array — the same layout as glTF and as the existing
// `Job::AddObjectMatrix` consumer (Job.cpp:5103-5107).  Inline here so
// `OverrideObjectAsciiChunkParser` doesn't need to introduce a new
// public Matrix4 constructor.
inline Matrix4 BuildMatrix4FromColumnMajor( const double m[16] )
{
	Matrix4 out;
	out._00 = m[ 0]; out._01 = m[ 1]; out._02 = m[ 2]; out._03 = m[ 3];
	out._10 = m[ 4]; out._11 = m[ 5]; out._12 = m[ 6]; out._13 = m[ 7];
	out._20 = m[ 8]; out._21 = m[ 9]; out._22 = m[10]; out._23 = m[11];
	out._30 = m[12]; out._31 = m[13]; out._32 = m[14]; out._33 = m[15];
	return out;
}

//////////////////////////////////////////////////
// Implementation AsciiSceneParser itself
//////////////////////////////////////////////////
AsciiSceneParser::AsciiSceneParser( const char * szFilename_ )
{
	memset( szFilename, 0, 1024 );
	if( szFilename_ ) {
		strcpy( szFilename, GlobalMediaPathLocator().Find(szFilename_).c_str() );
	}

	// Populate the default macros
	macros["PI"] = PI;
	macros["E"] = E_;
}

AsciiSceneParser::~AsciiSceneParser( )
{
}

namespace
{
	// Phase 6.1: depth counter for nested ParseAndLoadScene calls.
	// `> load child.RISEscene` and `> run script.RISEscript` invoke
	// `pJob.LoadAsciiScene(filename)` mid-parse of the outer scene,
	// which constructs a fresh AsciiSceneParser and runs its own
	// ParseAndLoadScene.  The Job-owned SourceSpanIndex /
	// TransformSnapshot containers are SHARED across both parses, so
	// the inner parse must NOT clear them — that would blow away the
	// outer scene's already-recorded entries.
	//
	// Tracked via a process-wide static because the inner parser is a
	// different AsciiSceneParser instance from the outer one; a per-
	// instance flag wouldn't propagate.  Thread-safety: RISE parses
	// are single-threaded (one scene-load drives one render); a future
	// concurrent-parse architecture would need TLS or a per-Job
	// counter.  The guard is RAII so any return path correctly
	// decrements.
	int gParseDepth = 0;

	struct ParseDepthGuard
	{
		bool isTopLevel;
		ParseDepthGuard() : isTopLevel( gParseDepth == 0 ) { ++gParseDepth; }
		~ParseDepthGuard() { --gParseDepth; }
	};

	// Phase 6.1 helper: extract the runtime name from a standard_object
	// chunk's parameter list.  Each entry in `chunkparams` is a
	// space-joined token sequence after macro substitution — the entry
	// for the name field looks like `name "sphere_0"` (quoted form) or
	// `name sphere_0` (bare-identifier form).  Returns the value
	// verbatim — quotes are PRESERVED to match what
	// `StandardObjectAsciiChunkParser::Finalize` reads via
	// `bag.GetString("name", ...)` and passes to `pJob.AddObject*`.
	// The IObjectManager uses the SAME quoted-or-not form as its map
	// key; stripping here would mis-key the BaseTransformSnapshot
	// lookup that follows.
	//
	// Returns the LAST `name ...` entry to match the descriptor-driven
	// dispatch's `bag.SetSingle("name", ...)` semantics — on a
	// malformed chunk with two name lines, bag.GetString returns the
	// last write.  Matching that ensures our IObjectManager lookup
	// uses the same key that AddObject was called with.
	// True iff `s` is an explicit `name <value>` line (leading "name " -- 5 chars).  Hoisted so the name
	// extractor below and the entity-index hook's name-omitted-camera branch share ONE detection and cannot drift.
	static bool IsNameLine( const String& s )
	{
		return s.size() >= 6 && s[0]=='n' && s[1]=='a' && s[2]=='m' && s[3]=='e' && s[4]==' ';
	}
	// True iff the chunk has an explicit `name` line at all.
	static bool HasExplicitName( const std::vector<String>& chunkparams )
	{
		for( std::vector<String>::const_iterator it = chunkparams.begin(); it != chunkparams.end(); ++it )
			if( IsNameLine( *it ) ) return true;
		return false;
	}
	std::string ExtractObjectName( const std::vector<String>& chunkparams )
	{
		std::string found = "noname";
		for( std::vector<String>::const_iterator it = chunkparams.begin();
		     it != chunkparams.end(); ++it ) {
			const String& s = *it;
			// Leading "name " (5 chars), then the value.
			if( IsNameLine( s ) ) {
				found = std::string( s.c_str() + 5 );
				// keep scanning — bag.SetSingle overwrites on dupes
			}
		}
		return found;
	}

	// Phase 6.1 helper: convert a body line's RawLine into a
	// ParameterSpan, or return false if the line is not a parameter
	// (blank / `#` comment-only / `}` / nested `>` command).
	bool BuildParameterSpan( const RISE::Implementation::RawLine& ln,
	                         std::string& outParamName,
	                         RISE::ParameterSpan& outSpan )
	{
		if( ln.tokens.empty() ) return false;
		const RISE::Implementation::RawToken& head = ln.tokens[0];
		if( head.text.empty() ) return false;
		const char c = head.text[0];
		if( c == '{' || c == '}' || c == '>' || c == '#' ) return false;
		// First token is the parameter name; remaining tokens (if any)
		// are values.  A parameter with zero value tokens is unusual
		// (e.g., a boolean shorthand) — still record the span with
		// valueBegin == valueEnd == byteEnd of the name token, so
		// Mode A can splice "after the keyword" if needed.  No
		// production scene uses that pattern; the safe-default keeps
		// us from emitting weird zero-length ranges.
		outParamName = head.text;
		outSpan.lineBeginOffset = ln.lineBeginOffset;
		outSpan.lineEndOffset   = ln.lineEndOffset;
		outSpan.commentBegin    = ln.comment.present ? ln.comment.byteBegin : ln.lineEndOffset;
		if( ln.tokens.size() >= 2 ) {
			outSpan.valueBegin = ln.tokens[1].byteBegin;
			outSpan.valueEnd   = ln.tokens.back().byteEnd;
			outSpan.isSymbolic = false;
			for( std::size_t k = 1; k < ln.tokens.size(); ++k ) {
				if( ln.tokens[k].isSymbolic ) {
					outSpan.isSymbolic = true;
					break;
				}
			}
		} else {
			outSpan.valueBegin = head.byteEnd;
			outSpan.valueEnd   = head.byteEnd;
			outSpan.isSymbolic = false;
		}
		return true;
	}
}

void AsciiSceneParser::OnStandardObjectFinalized(
	IJob& pJob,
	const std::vector<String>& chunkparams,
	std::size_t chunkHeaderIdx,
	std::size_t openBraceIdx,
	std::size_t closeBraceIdx )
{
	using namespace RISE::Implementation;

	// Need IJobPriv to reach the Phase 6.1 storage.  Existing parser
	// code already uses dynamic_cast<IJobPriv*>(&pJob) (see line ~399).
	IJobPriv* pPriv = dynamic_cast<IJobPriv*>( &pJob );
	if( !pPriv ) return;
	SourceSpanIndex* pSpans = pPriv->GetSourceSpanIndexMutable();
	TransformSnapshot* pBase = pPriv->GetBaseTransformSnapshotMutable();
	if( !pSpans || !pBase ) return;

	const std::vector<RawLine>& lines = mRawTokens.AllLines();
	if( chunkHeaderIdx >= lines.size() ||
	    openBraceIdx   >= lines.size() ||
	    closeBraceIdx  >= lines.size() ) {
		// Defensive: something is out of sync — bail without crashing.
		return;
	}

	const RawLine& headerLine = lines[chunkHeaderIdx];
	const RawLine& closeLine  = lines[closeBraceIdx];
	if( headerLine.tokens.empty() ) return;

	// chunkBeginOffset is the byte offset of the chunk-keyword token
	// on the header line (the "standard_object" word itself).
	const std::size_t chunkBeginOffset = headerLine.tokens[0].byteBegin;
	// chunkEndOffset is one past the end of the closing `}` line's
	// content bytes.  Save-time placement logic treats this as the
	// boundary after which the next chunk/command starts.
	const std::size_t chunkEndOffset = closeLine.lineEndOffset;

	// Extract the runtime name from the post-substitution params.
	const std::string runtimeName = ExtractObjectName( chunkparams );

	// Always record creation location — used by the save engine's
	// placement loop for FOR-body 2..N entities (which don't get
	// their own SourceSpan).  R5 §1 / R7 §1.
	pSpans->RecordCreationLocation( runtimeName, std::string(szFilename), chunkEndOffset );

	// Capture base transform: query the just-added object's runtime
	// matrix BEFORE any subsequent override_object chunk has had a
	// chance to mutate it.  This is the §7.4 "Mbase" baseline.
	IObjectManager* pObjMgr = pPriv->GetObjects();
	if( pObjMgr ) {
		IObjectPriv* pObj = pObjMgr->GetItem( runtimeName.c_str() );
		if( pObj ) {
			pBase->Add( runtimeName, pObj->GetFinalTransformMatrix() );
		}
	}

	// FOR-revisit detection: if we've seen this chunk-header offset
	// before, the parser is iterating a FOR body and the prior runtime
	// entity owns the SourceSpan.  Flip chunkRevisited on that span
	// and skip building a new one.  This is the §6.4 revisit hook.
	std::map<std::size_t, std::string>::iterator it =
		mChunkHeaderOffsetToFirstName.find( chunkBeginOffset );
	if( it != mChunkHeaderOffsetToFirstName.end() ) {
		SourceSpan* firstSpan = pSpans->FindMutable( it->second );
		if( firstSpan ) {
			firstSpan->chunkRevisited = true;
		}
		return;
	}

	// First visit: build a fresh SourceSpan.
	SourceSpan span;
	span.filePath             = szFilename;
	span.chunkBeginOffset     = chunkBeginOffset;
	span.chunkEndOffset       = chunkEndOffset;
	span.bodyCloseBraceLineBegin = closeLine.lineBeginOffset;
	span.chunkRevisited       = false;

	// Open-brace offset: locate the `{` token on the open-brace line.
	// It may share that line with the chunk-keyword (`standard_object {`)
	// or live on its own line — search both possibilities.
	{
		const RawLine& openLine = lines[openBraceIdx];
		for( std::size_t k = 0; k < openLine.tokens.size(); ++k ) {
			if( openLine.tokens[k].text == "{" ) {
				span.bodyOpenBraceOffset = openLine.tokens[k].byteBegin;
				break;
			}
		}
		// Some scenes put `{` on the same line as the chunk keyword;
		// fall back to the header line in that case.
		if( span.bodyOpenBraceOffset == 0 ) {
			for( std::size_t k = 0; k < headerLine.tokens.size(); ++k ) {
				if( headerLine.tokens[k].text == "{" ) {
					span.bodyOpenBraceOffset = headerLine.tokens[k].byteBegin;
					break;
				}
			}
		}
	}

	// Close-brace offset: locate the `}` token on the close-brace line.
	for( std::size_t k = 0; k < closeLine.tokens.size(); ++k ) {
		if( closeLine.tokens[k].text == "}" ) {
			span.bodyCloseBraceOffset = closeLine.tokens[k].byteBegin;
			break;
		}
	}

	// Build per-parameter spans from body lines.  Body range is
	// (openBraceIdx, closeBraceIdx) — exclusive on both ends since
	// those carry `{` / `}` themselves.
	for( std::size_t i = openBraceIdx + 1; i < closeBraceIdx; ++i ) {
		std::string paramName;
		ParameterSpan ps;
		if( BuildParameterSpan( lines[i], paramName, ps ) ) {
			span.parameterSpans[paramName] = ps;
		}
	}

	// Author-mode: matrix > quaternion > euler precedence, matching
	// the parser's runtime decision (§6.3).
	if( span.parameterSpans.find("matrix") != span.parameterSpans.end() ) {
		span.authorMode = AuthorMode::Matrix;
	} else if( span.parameterSpans.find("quaternion") != span.parameterSpans.end() ) {
		span.authorMode = AuthorMode::Quaternion;
	} else {
		span.authorMode = AuthorMode::Euler;
	}

	pSpans->Add( runtimeName, std::move(span) );
	mChunkHeaderOffsetToFirstName[chunkBeginOffset] = runtimeName;
}

void AsciiSceneParser::OnEntityChunkFinalized(
	IJob& pJob,
	EntityCategory category,
	const std::vector<String>& chunkparams,
	std::size_t chunkHeaderIdx,
	std::size_t openBraceIdx,
	std::size_t closeBraceIdx )
{
	using namespace RISE::Implementation;

	// Phase B: build a SourceSpan for a camera / light / material /
	// medium chunk.  Same byte-range + per-parameter-span machinery
	// as OnStandardObjectFinalized; the object-transform-only bits
	// (BaseTransformSnapshot, CreationLocation, authorMode) are
	// deliberately omitted — the property save path diffs introspected
	// values, not transform matrices.
	IJobPriv* pPriv = dynamic_cast<IJobPriv*>( &pJob );
	if( !pPriv ) return;
	SourceSpanIndex* pSpans = pPriv->GetSourceSpanIndexMutable();
	if( !pSpans ) return;

	const std::vector<RawLine>& lines = mRawTokens.AllLines();
	if( chunkHeaderIdx >= lines.size() ||
	    openBraceIdx   >= lines.size() ||
	    closeBraceIdx  >= lines.size() ) {
		return;
	}

	const RawLine& headerLine = lines[chunkHeaderIdx];
	const RawLine& closeLine  = lines[closeBraceIdx];
	if( headerLine.tokens.empty() ) return;

	const std::size_t chunkBeginOffset = headerLine.tokens[0].byteBegin;
	const std::size_t chunkEndOffset   = closeLine.lineEndOffset;

	// Derive the runtime name this entity is keyed under.  Cameras are
	// special: a NAME-OMITTED camera's runtime name comes from
	// AllocateCameraName ("default", auto-suffixed), NOT
	// ExtractObjectName's "noname".  Key its span under the allocator's
	// result so the editor (which enumerates the runtime "default") can
	// retrieve it for a property-edit round-trip.  For an EXPLICITLY-named
	// camera (HasExplicitName==true) we KEEP ExtractObjectName -- it already
	// equals the runtime name, because AllocateCameraName returns a non-empty
	// request verbatim.  Non-camera producers likewise key under
	// ExtractObjectName, which matches their runtime name
	// (bag.GetString("name","noname")).
	// s_lastAllocatedCameraName is fresh here: this hook fires right after
	// the camera's Finalize ran AllocateCameraName.  Settings chunks with
	// no `name` param (global_medium, hosek_wilkie_skylight, scene_options,
	// camera_defaults) never reach this hook -- the caller gates on the
	// descriptor declaring a `name` parameter.
	std::string runtimeName = ExtractObjectName( chunkparams );
	if( category == EntityCategory::Camera && !HasExplicitName( chunkparams ) ) {
		runtimeName = RISE::LastAllocatedCameraName();
	}

	// FOR-revisit detection: a chunk byte offset seen before means the
	// parser is iterating a FOR body.  Flip chunkRevisited on the
	// first entity's span and SKIP — the save engine refuses to splice
	// FOR-generated chunks (same policy as objects).
	std::map<std::size_t, DirtyEntity>::iterator it =
		mEntityChunkHeaderOffsetToFirst.find( chunkBeginOffset );
	if( it != mEntityChunkHeaderOffsetToFirst.end() ) {
		SourceSpan* firstSpan =
			pSpans->FindEntityMutable( it->second.first, it->second.second );
		if( firstSpan ) {
			firstSpan->chunkRevisited = true;
		}
		return;
	}

	SourceSpan span;
	span.filePath                = szFilename;
	span.chunkBeginOffset        = chunkBeginOffset;
	span.chunkEndOffset          = chunkEndOffset;
	span.bodyCloseBraceLineBegin = closeLine.lineBeginOffset;
	span.chunkRevisited          = false;
	// Phase C (round-2 review): a camera/light/material/medium chunk
	// parsed INSIDE the sentinel block is one the editor emitted in a
	// previous session.  Flag it so the save engine re-emits it
	// wholesale (rather than letting the block lifecycle erase it).
	span.insideManagedBlock      = mInsideManagedOverrideBlock;

	// Open-brace offset: the `{` may share the header line with the
	// chunk keyword or live on its own line — check both.
	{
		const RawLine& openLine = lines[openBraceIdx];
		for( std::size_t k = 0; k < openLine.tokens.size(); ++k ) {
			if( openLine.tokens[k].text == "{" ) {
				span.bodyOpenBraceOffset = openLine.tokens[k].byteBegin;
				break;
			}
		}
		if( span.bodyOpenBraceOffset == 0 ) {
			for( std::size_t k = 0; k < headerLine.tokens.size(); ++k ) {
				if( headerLine.tokens[k].text == "{" ) {
					span.bodyOpenBraceOffset = headerLine.tokens[k].byteBegin;
					break;
				}
			}
		}
	}

	// Close-brace offset.
	for( std::size_t k = 0; k < closeLine.tokens.size(); ++k ) {
		if( closeLine.tokens[k].text == "}" ) {
			span.bodyCloseBraceOffset = closeLine.tokens[k].byteBegin;
			break;
		}
	}

	// Per-parameter spans from body lines (exclusive of the `{`/`}`).
	for( std::size_t i = openBraceIdx + 1; i < closeBraceIdx; ++i ) {
		std::string paramName;
		ParameterSpan ps;
		if( BuildParameterSpan( lines[i], paramName, ps ) ) {
			span.parameterSpans[paramName] = ps;
		}
	}
	// authorMode stays at AuthorMode::Euler (object-only field; unused
	// for non-object entities).

	pSpans->AddEntity( category, runtimeName, std::move(span) );
	mEntityChunkHeaderOffsetToFirst[chunkBeginOffset] =
		std::make_pair( category, runtimeName );
}

void AsciiSceneParser::OnOverrideObjectFinalized(
	IJob& pJob,
	const std::vector<String>& chunkparams,
	std::size_t chunkHeaderIdx,
	std::size_t /*openBraceIdx*/,
	std::size_t closeBraceIdx )
{
	using namespace RISE::Implementation;
	IJobPriv* pPriv = dynamic_cast<IJobPriv*>( &pJob );
	if( !pPriv ) return;
	OverrideSpanIndex* pIdx = pPriv->GetOverrideSpanIndexMutable();
	if( !pIdx ) return;

	const std::vector<RawLine>& lines = mRawTokens.AllLines();
	if( chunkHeaderIdx >= lines.size() || closeBraceIdx >= lines.size() ) {
		return;
	}
	const RawLine& headerLine = lines[chunkHeaderIdx];
	const RawLine& closeLine  = lines[closeBraceIdx];
	if( headerLine.tokens.empty() ) return;

	OverrideRecord rec;
	rec.targetName        = ExtractObjectName( chunkparams );
	rec.filePath          = std::string( szFilename );
	rec.chunkBeginOffset  = headerLine.tokens[0].byteBegin;
	rec.chunkEndOffset    = closeLine.lineEndOffset;
	rec.managed           = mInsideManagedOverrideBlock;

	// Scan chunkparams to record which fields were present + their
	// applied values.  The descriptor-driven dispatch has already
	// validated that field names are legal for the override_object
	// chunk; we just re-read for our own bookkeeping.  Values are
	// already macro-substituted and expression-evaluated by the time
	// chunkparams was built.
	for( std::vector<String>::const_iterator it = chunkparams.begin();
	     it != chunkparams.end(); ++it ) {
		String key, value;
		if( !string_split( *it, key, value, ' ' ) ) continue;
		const std::string k( key.c_str() );

		if( k == "position" ) {
			double v[3] = {0,0,0};
			std::sscanf( value.c_str(), "%lf %lf %lf", &v[0], &v[1], &v[2] );
			rec.hasPosition = true;
			rec.position = Vector3( v[0], v[1], v[2] );
		} else if( k == "orientation" ) {
			double v[3] = {0,0,0};
			std::sscanf( value.c_str(), "%lf %lf %lf", &v[0], &v[1], &v[2] );
			rec.hasOrientation = true;
			rec.orientation = Vector3( v[0], v[1], v[2] );
		} else if( k == "quaternion" ) {
			double v[4] = {0,0,0,1};
			std::sscanf( value.c_str(), "%lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3] );
			rec.hasQuaternion = true;
			rec.quaternion[0] = v[0];
			rec.quaternion[1] = v[1];
			rec.quaternion[2] = v[2];
			rec.quaternion[3] = v[3];
		} else if( k == "scale" ) {
			double v[3] = {1,1,1};
			std::sscanf( value.c_str(), "%lf %lf %lf", &v[0], &v[1], &v[2] );
			rec.hasScale = true;
			rec.scale = Vector3( v[0], v[1], v[2] );
		} else if( k == "matrix" ) {
			double m[16];
			std::sscanf( value.c_str(),
				"%lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf",
				&m[0],&m[1],&m[2],&m[3], &m[4],&m[5],&m[6],&m[7],
				&m[8],&m[9],&m[10],&m[11], &m[12],&m[13],&m[14],&m[15] );
			rec.hasMatrix = true;
			rec.matrix = BuildMatrix4FromColumnMajor( m );
		}
	}

	pIdx->Add( std::move(rec) );
}

void AsciiSceneParser::PopulateLoadedTransformSnapshot( IJob& pJob )
{
	IJobPriv* pPriv = dynamic_cast<IJobPriv*>( &pJob );
	if( !pPriv ) return;
	TransformSnapshot* pBase   = pPriv->GetBaseTransformSnapshotMutable();
	TransformSnapshot* pLoaded = pPriv->GetLoadedTransformSnapshotMutable();
	IObjectManager*    pObjMgr = pPriv->GetObjects();
	if( !pBase || !pLoaded || !pObjMgr ) return;

	// LoadedTransformSnapshot mirrors BaseTransformSnapshot's name set
	// (V1: override_object only modifies existing objects, never adds
	// new ones).  Iterate the base names and snapshot each object's
	// CURRENT transform — which reflects any override_object chunks
	// that ran during parse.  §7.4.
	std::vector<std::string> names = pBase->Names();
	for( std::size_t i = 0; i < names.size(); ++i ) {
		IObjectPriv* pObj = pObjMgr->GetItem( names[i].c_str() );
		if( pObj ) {
			pLoaded->Add( names[i], pObj->GetFinalTransformMatrix() );
		}
	}
}

void AsciiSceneParser::PopulateLoadedPropertySnapshot( IJob& pJob )
{
	// Phase B: capture each editable entity's loaded parameter values
	// (descriptor-introspected, as strings) into its SourceSpan.  Runs
	// at the END of the top-level parse — after every chunk AND every
	// `>` command — so it reflects true loaded state.  The save
	// engine's property pass diffs current introspection against this.
	IJobPriv* pPriv = dynamic_cast<IJobPriv*>( &pJob );
	if( !pPriv ) return;
	SourceSpanIndex* pSpans = pPriv->GetSourceSpanIndexMutable();
	if( !pSpans ) return;
	const IScene* scene = pPriv->GetScene();
	if( !scene ) return;

	auto storeInto = [&]( SourceSpan* span,
	                      const std::vector<CameraProperty>& props ) {
		if( !span ) return;
		span->loadedPropertyValues.clear();
		for( std::size_t i = 0; i < props.size(); ++i ) {
			span->loadedPropertyValues[ std::string( props[i].name.c_str() ) ] =
				std::string( props[i].value.c_str() );
		}
	};

	// Objects — for material / shader / shadow / interior-medium edits.
	// (Transform edits use the matrix snapshots, not this map.)
	IObjectManager* objs = pPriv->GetObjects();
	if( objs ) {
		// Copy the key set first: storeInto mutates span values but
		// not the map structure, so iterating Entries() directly is
		// safe — still, take names to keep the lookup uniform.
		std::vector<std::string> objNames;
		objNames.reserve( pSpans->Entries().size() );
		for( const auto& kv : pSpans->Entries() ) objNames.push_back( kv.first );
		for( const std::string& nm : objNames ) {
			const IObject* obj = objs->GetItem( nm.c_str() );
			if( !obj ) continue;
			storeInto( pSpans->FindMutable( nm ),
				ObjectIntrospection::Inspect(
					String( nm.c_str() ), *obj,
					pPriv->GetMaterials(), pPriv->GetShaders(), &pJob ) );
		}
	}

	// Camera / light / material / medium entity spans.
	std::vector<DirtyEntity> entityKeys;
	entityKeys.reserve( pSpans->EntityEntries().size() );
	for( const auto& kv : pSpans->EntityEntries() ) entityKeys.push_back( kv.first );
	for( const DirtyEntity& key : entityKeys ) {
		const EntityCategory cat  = key.first;
		const std::string&   name = key.second;
		SourceSpan* span = pSpans->FindEntityMutable( cat, name );
		if( !span ) continue;
		switch( cat ) {
		case EntityCategory::Camera: {
			const ICameraManager* m = scene->GetCameras();
			const ICamera* cam = m ? m->GetItem( name.c_str() ) : 0;
			// includeRollOrientation = true: the loaded snapshot must
			// carry the `orientation` row so the save engine can diff a
			// roll edit against it (the panel hides the row).
			if( cam ) storeInto( span, CameraIntrospection::Inspect( *cam, true ) );
			break;
		}
		case EntityCategory::Light: {
			const ILightManager* m = scene->GetLights();
			const ILight* lt = m ? m->GetItem( name.c_str() ) : 0;
			if( lt ) storeInto( span,
				LightIntrospection::Inspect( String( name.c_str() ), *lt ) );
			break;
		}
		case EntityCategory::Material: {
			IMaterialManager* m = pPriv->GetMaterials();
			const IMaterial* mat = m ? m->GetItem( name.c_str() ) : 0;
			if( mat ) storeInto( span,
				MaterialIntrospection::Inspect( String( name.c_str() ), *mat,
					pPriv->GetPainters(), pPriv->GetScalarPainters(), &pJob ) );
			break;
		}
		case EntityCategory::Medium: {
			const IMedium* med = pJob.GetMedium( name.c_str() );
			if( med ) storeInto( span,
				MediaIntrospection::Inspect( String( name.c_str() ), *med ) );
			break;
		}
		case EntityCategory::Object:
		default:
			break;
		}
	}
}

bool AsciiSceneParser::ParseAndLoadScene( IJob& pJob )
{
	// Phase 6.1: detect whether THIS is a top-level scene-load or a
	// recursive one (triggered by `> load file.RISEscene` or
	// `> run script.RISEscript` mid-parse of an outer scene).
	// Top-level: clear the Job-owned per-entity metadata so repeated
	// LoadAsciiScene calls start fresh.  Recursive: skip the clear so
	// the outer parse's already-recorded entries survive while we
	// append the child file's entries beneath them.  Each entry's
	// `filePath` (set in OnStandardObjectFinalized) distinguishes
	// outer- vs inner-file owners — the save engine's R7 §1 / pinned
	// 2.25 cross-file-refusal check relies on that.
	ParseDepthGuard depthGuard;
	RISE::ClearChunkParserState( depthGuard.isTopLevel );

	// Restart the legacy hal() QMC sequence per TOP-LEVEL parse.  `mh`
	// (declared file-scope in THIS streaming TU) is the live consumer that
	// evaluate_first_function_in_expression advances via next_halton(); it
	// must evaluate hal() from a FRESH sequence per standalone LoadAsciiScene
	// (standalone-equivalent + order-independent) and NOT leak sample state
	// across top-level loads in one process.  A nested `> load` / `> run`
	// parse passes isTopLevel == false, so the sequence stays CONTINUOUS
	// across a scene's includes.  Reset here -- before the parse body below
	// consumes any hal() -- matching the pre-Slice-6b reset point.
	if( depthGuard.isTopLevel ) {
		mh.Reset();
	}

	// Phase 0 (docs/ROUND_TRIP_SAVE_PLAN.md §6.2): begin recording raw
	// per-line byte spans + tokens.  Parser-local state — always
	// reset, even on a recursive parse (the inner parser has its own
	// instance with its own mRawTokens).
	mRawTokens.BeginScene();
	mChunkHeaderOffsetToFirstName.clear();
	mEntityChunkHeaderOffsetToFirst.clear();  // Phase B FOR-revisit map
	mInsideManagedOverrideBlock = false;  // Phase 6.2 sentinel state

	if( depthGuard.isTopLevel ) {
		// Phase 6.1 + 6.2: reset Job-owned metadata containers on the
		// outermost parse only.  Containers themselves live for the
		// Job's lifetime; only their CONTENTS reset here.
		IJobPriv* pPriv = dynamic_cast<IJobPriv*>( &pJob );
		if( pPriv ) {
			if( SourceSpanIndex* p = pPriv->GetSourceSpanIndexMutable() )   p->Clear();
			if( TransformSnapshot* p = pPriv->GetBaseTransformSnapshotMutable() )   p->Clear();
			if( TransformSnapshot* p = pPriv->GetLoadedTransformSnapshotMutable() ) p->Clear();
			if( OverrideSpanIndex* p = pPriv->GetOverrideSpanIndexMutable() )   p->Clear();
		}
	}

	// Build the dispatch map from the canonical parser registry.  The
	// parser_entries vector owns each chunk parser via unique_ptr for
	// the duration of this call; when it goes out of scope every parser
	// is destroyed automatically.  Same registry feeds SceneEditorSuggestions.
	std::vector<ChunkParserEntry> parser_entries = CreateAllChunkParsers();
	std::map<std::string, IAsciiChunkParser*> chunks;
	for( std::vector<ChunkParserEntry>::iterator it = parser_entries.begin(); it != parser_entries.end(); ++it ) {
		chunks[it->keyword] = it->parser.get();
	}

	// Open up the file and start parsing!
	struct stat file_stats = {0};
	stat( szFilename, &file_stats );
	unsigned int nSize = static_cast<unsigned int>(file_stats.st_size);

	// R-final P1 #3: capture the top-level file's identity (path +
	// mtime + size) so the save engine can refuse on external
	// modification (§11.6).  Only the top-level (depth-0) parse
	// records identity — recursive `> load`/`> run` parses target
	// child files, but the save engine writes to the TOP-LEVEL file.
	// `depthGuard.isTopLevel` is established a few lines down; we
	// pre-compute the identity here and apply it only when top-level.
	FileIdentity loadIdentity;
	loadIdentity.filePath  = szFilename;
	loadIdentity.mtimeSec  = static_cast<long long>(file_stats.st_mtime);
#if defined(__APPLE__)
	loadIdentity.mtimeNsec = static_cast<long long>(file_stats.st_mtimespec.tv_nsec);
#elif defined(__linux__) || defined(__unix__)
	loadIdentity.mtimeNsec = static_cast<long long>(file_stats.st_mtim.tv_nsec);
#else
	loadIdentity.mtimeNsec = 0;
#endif
	loadIdentity.sizeBytes = static_cast<long long>(file_stats.st_size);
	loadIdentity.captured  = true;
	// Only the top-level parse populates FileIdentity — the save
	// engine writes to the TOP-LEVEL file, so recursive child-file
	// parses do not contribute to the identity check.
	if( depthGuard.isTopLevel ) {
		IJobPriv* pPriv = dynamic_cast<IJobPriv*>( &pJob );
		if( pPriv ) {
			if( SourceSpanIndex* p = pPriv->GetSourceSpanIndexMutable() ) {
				p->SetFileIdentity( loadIdentity );
			}
		}
	}

	// I realize this is ugly, but it is necessary for proper
	// clean up after breaking part way
	String strBuffer;
	strBuffer.resize( nSize + 1 );
	char* pBuffer = (char*)strBuffer.c_str();
	memset( pBuffer, 0, nSize + 1 );

	FILE* f = fopen( szFilename, "rb" );
	if( f ) {
		fread( pBuffer, nSize, 1, f );
		fclose( f );
	} else {
		GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Failed to load scene file \'%s\'", szFilename );
		return false;
	}

	std::istringstream		in( pBuffer );
	unsigned int			linenum = 0;

	// Command parser for parsing commands embedded in the scene
	AsciiCommandParser* parser = new AsciiCommandParser();
	GlobalLog()->PrintNew( parser, __FILE__, __LINE__, "command parser" );

	{
		char				line[MAX_CHARS_PER_LINE] = {0};		// <sigh>....

		// Verify version number
		const std::streampos versionLineBeginPos = in.tellg();
		in.getline( line, MAX_CHARS_PER_LINE );
		linenum++;
		if( versionLineBeginPos != static_cast<std::streampos>(-1) ) {
			mRawTokens.RecordLine( static_cast<std::size_t>(versionLineBeginPos), line );
		}

		// First check the first few characters to see if it contains our marker
		static const char* id = "RISE ASCII SCENE";
		if( strncmp( line, id, strlen(id) ) ) {
			GlobalLog()->Print( eLog_Error, "AsciiSceneParser: Scene does not contain RISE ASCII SCENE marker" );
			return false;
		}

		// Next find the scene version number
		const char* num = &line[strlen(id)];

		int version = atoi( num );

		if( version != CURRENT_SCENE_VERSION ) {
			if( version == 5 ) {
				// Phase B2 (2026-05) introduced format v6.  v5 scenes
				// authored width / height / pixelAR INSIDE camera
				// chunks; v6 moves those into a top-level `film` chunk
				// and drops them from camera chunks.  Run the
				// migration script:
				//     python tools/migrate_scenes_v5_to_v6.py <path>
				// to update v5 scenes in place.
				GlobalLog()->PrintEx( eLog_Error,
					"AsciiSceneParser: Scene is version 5; the parser expects version %d.  "
					"v5 authored width/height/pixelAR inside camera chunks; v6 moves them "
					"into a top-level `film` chunk.  Migrate with: "
					"`python tools/migrate_scenes_v5_to_v6.py <path>` (in-place; idempotent). "
					"See docs/SCENE_CONVENTIONS.md \"The `film` chunk\".",
					CURRENT_SCENE_VERSION );
			} else {
				GlobalLog()->PrintEx( eLog_Error,
					"AsciiSceneParser: Scene version problem, scene is version \'%d\', we require \'%d\'",
					version, CURRENT_SCENE_VERSION );
			}
			return false;
		}
	}

	//
	// Parse the rest of the scene, basically read each line and see if
	//  we have a chunk, a comment or a command to pass to the command
	//  parser
	//

	std::stack<LOOP> loops;

	bool bInCommentBlock = false;
	for(;;) {
		char				line[MAX_CHARS_PER_LINE] = {0};		// <sigh>....
		const std::streampos lineBeginPos = in.tellg();
		in.getline( line, MAX_CHARS_PER_LINE );

		linenum++;

		if( in.fail() || in.eof() ) {
			break;
		}

		// Phase 0 (§6.2): record this line's raw tokens + byte offsets.
		// `lineBeginPos` is the offset where the next getline call would
		// have started reading — i.e., the first byte of this line in
		// the source buffer.  Skipped on tellg failure (rare; only on a
		// stream error which the in.fail check above also catches).
		// Phase 6.1: `outerLineIdx` is the index in mRawTokens.AllLines()
		// where THIS line lands — captured BEFORE RecordLine appends.
		// When this line turns out to be a chunk-name line, we'll pass
		// outerLineIdx as `chunkHeaderIdx` to OnStandardObjectFinalized.
		const std::size_t outerLineIdx = mRawTokens.AllLines().size();
		if( lineBeginPos != static_cast<std::streampos>(-1) ) {
			mRawTokens.RecordLine( static_cast<std::size_t>(lineBeginPos), line );
		}

		// Tokenize the string to get rid of comments etc
		String			tokens[1024];
		unsigned int numTokens = AsciiCommandParser::TokenizeString( line, tokens, 1024 );

		if( bInCommentBlock ) {
			if( tokens[0].size() >= 2 && tokens[0][0] == '*' && tokens[0][1] == '/' ) {
				bInCommentBlock = false;
			}
			continue;
		}

		if( numTokens == 0 ) {
			// Empty
			continue;
		}

		if( tokens[0][0] == '#' ) {
			// Comment.  Phase 6.2 (§9.6 sentinel block): detect the
			// managed override block markers so OnOverrideObjectFinalized
			// can classify subsequent chunks as managed-or-unmanaged.
			//
			// R-final P2 fix: match the canonical sentinel strings
			// EXACTLY (after stripping leading whitespace + trailing
			// CR) instead of substring-matching.  A user-written
			// comment like `# TODO: RISE editor overrides go here`
			// would otherwise wrongly flip the in-block flag, causing
			// downstream `override_object` chunks to be classified
			// `managed = true` even though the LocateManagedBlock
			// pass in SaveEngine (which uses the same exact-line
			// match) sees no managed block.  That divergence would
			// cause the save engine to drop or normalise hand-written
			// overrides.  Both sides now share the constants in
			// OverrideSpanIndex.h.
			//
			// Build a stripped view of `line`: trim leading spaces /
			// tabs and a trailing `\r` (CRLF source files leave the
			// CR behind after `getline` strips only `\n`).
			std::size_t bIdx = 0;
			while( line[bIdx] == ' ' || line[bIdx] == '\t' ) ++bIdx;
			std::size_t eIdx = std::strlen( line );
			while( eIdx > bIdx && ( line[eIdx-1] == '\r' || line[eIdx-1] == ' ' || line[eIdx-1] == '\t' ) ) --eIdx;
			const std::size_t lineLen = eIdx - bIdx;
			const char* stripped = line + bIdx;
			if( lineLen == std::strlen( RISE::kManagedBlockSentinelOpen )
			    && std::memcmp( stripped, RISE::kManagedBlockSentinelOpen, lineLen ) == 0 ) {
				mInsideManagedOverrideBlock = true;
			} else if( lineLen == std::strlen( RISE::kManagedBlockSentinelClose )
			    && std::memcmp( stripped, RISE::kManagedBlockSentinelClose, lineLen ) == 0 ) {
				mInsideManagedOverrideBlock = false;
			}
			continue;
		}

		if( tokens[0].size() >= 2 && tokens[0][0] == '/' && tokens[0][1] == '*' ) {
			// Comment block
			bInCommentBlock = true;
			continue;
		}

		if( tokens[0][0] == '>' ) {
			// Command
			if( !parser->ParseCommand( &tokens[1], numTokens-1, pJob ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Failed to parse line \'%s\' (%u)", line, linenum );
				return false;
			}
			continue;
		}

		// Check for a macro definition
		if( tokens[0][0] == '!' || tokens[0] == "DEFINE" || tokens[0] == "define" ) {
			// We have a macro
			if( numTokens < 3 ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Not enough parameters for macro definition line (%u)", linenum );
				return false;
			}

			if( !substitute_macros_in_tokens( tokens, numTokens ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error while performing macro subsitution on line %u", linenum );
				return false;
			}

			if( !evaluate_expressions_in_tokens( tokens, numTokens ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error while performing math expression evaluation %u", linenum );
				return false;
			}

			if( !add_macro( tokens[1], tokens[2] ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error adding new macro (%u)", linenum );
				return false;
			}
			continue;
		}

		// Check for macro removal
		if( tokens[0][0] == '~' || tokens[0] == "undef" || tokens[0] == "UNDEF" ) {
			if( numTokens < 2 ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Not enough parameters for macro removal line (%u)", linenum );
				return false;
			}

			if( !remove_macro( tokens[1] ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Couldn't find the macro to remove (%u)", linenum );
				return false;
			}
			continue;
		}

		// Check for loops
		if( tokens[0] == "FOR" ) {
			// loops require the following format
			// FOR <variable name> <start value> <end value> <increment size>
			if( numTokens < 5 ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Not enough paramters for loop line (%u)", linenum );
				return false;
			}

			// First check to see if the variable name is already in the macro map
			if( macros.find( tokens[1] ) != macros.end() ) {
				// Already there
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Variable \'%s\' already exists line (%u)", tokens[1].c_str(), linenum );
				return false;
			}

			if( !substitute_macros_in_tokens( tokens, numTokens ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error while performing macro subsitution on line %u", linenum );
				return false;
			}

			if( !evaluate_expressions_in_tokens( tokens, numTokens ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error while performing math expression evaluation %u", linenum );
				return false;
			}

			LOOP l;
            l.position = in.tellg();
			l.var = tokens[1];
			l.curvalue = atof( tokens[2].c_str() );
			l.endvalue = atof( tokens[3].c_str() );
			l.increment = atof( tokens[4].c_str() );
			l.linecount = linenum;

			macros[l.var] = l.curvalue;

			loops.push( l );
			continue;
		}

		// Check for loop end
		if( tokens[0] == "ENDFOR" ) {
            // We are at the end of the current loop
			if( loops.size() == 0 ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: LOOPEND found with no current loop, line (%u)", linenum );
			}

			LOOP& l = loops.top();
			l.curvalue += l.increment;

			MacroTable::iterator it = macros.find( l.var );

			if( l.curvalue > l.endvalue ) {
				// This loop is done, remove it from the queue and continue
				loops.pop();
				if( it == macros.end() ) {
					GlobalLog()->PrintEasyError( "AsciiSceneParser:: Fatal error in trying to remove loop variable" );
					return false;
				}
				macros.erase( it );
				continue;
			}

			// Otherwise, update the value in the macro list and continue
			if( it == macros.end() ) {
				GlobalLog()->PrintEasyError( "AsciiSceneParser:: Fatal error in trying to update loop variable" );
				return false;
			}

			it->second = l.curvalue;

			// Set the file back to the line this loop begins at and continue
			in.seekg( l.position );
			linenum = l.linecount;
			continue;
		}

		// Otherwise must be a chunk
		// Read the chunk type
		std::map<std::string,IAsciiChunkParser*>::iterator it = chunks.find( std::string(tokens[0].c_str()) );

		if( it == chunks.end() ) {
			GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Failed to find chunk \'%s\' on line %u", tokens[0].c_str(), linenum );
			return false;
		}

		const IAsciiChunkParser* pChunkParser = it->second;

		// Parse the '{'
		{
			const std::streampos braceLineBeginPos = in.tellg();
			in.getline( line, MAX_CHARS_PER_LINE );
			linenum++;
			if( in.fail() ) {
				GlobalLog()->PrintEasyError( "AsciiSceneParser::ParseScene:: Failed reading looking for '{' for chunk" );
				break;
			}

			// Phase 0: record the `{` line AFTER the fail-check so a
			// truncated read doesn't push phantom bytes into mRawTokens.
			// Phase 6.1: `openBraceIdx` is the index of THIS line in
			// mRawTokens.AllLines(); captured BEFORE RecordLine appends.
			const std::size_t openBraceIdx = mRawTokens.AllLines().size();
			if( braceLineBeginPos != static_cast<std::streampos>(-1) ) {
				mRawTokens.RecordLine( static_cast<std::size_t>(braceLineBeginPos), line );
			}

			String			toks[8];
			unsigned int numTokens = AsciiCommandParser::TokenizeString( line, toks, 8 );

			if( numTokens < 1 ) {
				return false;
			}

			if( toks[0][0] != '{' ) {
				GlobalLog()->PrintEasyError( "AsciiSceneParser::ParseScene:: Cannot find '{' for chunk" );
				return false;
			}

			// Keep reading the parameters for the chunk until we encounter the closing '}'
			IAsciiChunkParser::ParamsList chunkparams;
			// Phase 6.1: only call OnStandardObjectFinalized if we
			// actually saw the closing `}` — a `getline` failure
			// mid-chunk would leave the last-recorded line pointing
			// at a body line (not `}`), and computing chunk byte
			// offsets from it would produce wrong splice targets.
			bool sawCloseBrace = false;
			for(;;) {
				const std::streampos bodyLineBeginPos = in.tellg();
				in.getline( line, MAX_CHARS_PER_LINE );

				linenum++;
				if( in.fail() ) {
					GlobalLog()->PrintEasyError( "AsciiSceneParser::ParseScene:: Failed reading while reading chunk" );
					break;
				}

				// Phase 0: record every body line (including blanks,
				// comment-only lines, and the closing `}` line).
				if( bodyLineBeginPos != static_cast<std::streampos>(-1) ) {
					mRawTokens.RecordLine( static_cast<std::size_t>(bodyLineBeginPos), line );
				}

				// Don't bother reading comments or commands
				String			tokens[1024];
				unsigned int numTokens = AsciiCommandParser::TokenizeString( line, tokens, 1024 );

				if( numTokens < 1 ) {
					continue;
				}

				if( tokens[0][0] == '#' ) {
					continue;
				}

				if( tokens[0][0] == '>' ) {
					// We could optionally just parse the command...
					continue;
				}

				if( tokens[0][0] == '}' ) {
					// End of chunk, so break out
					sawCloseBrace = true;
					break;
				}

				if( !substitute_macros_in_tokens( tokens, numTokens ) ) {
					GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error while performing macro subsitution on line %u", linenum );
				}

				if( !evaluate_expressions_in_tokens( tokens, numTokens ) ) {
					GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error while performing math expression evaluation %u", linenum );
				}

				// Otherwise, assemble the tokens and add it to the chunk list
				String s;
				make_string_from_tokens( s, tokens, numTokens, " " );
				chunkparams.push_back( s );
			}

			// Finished reading a chunk so parse it.
			if( !pChunkParser->ParseChunk( chunkparams, pJob ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Failed to load chunk \'%s\' on line %u", tokens[0].c_str(), linenum );
				return false;
			}

			// Phase 6.1: standard_object chunks populate SourceSpanIndex
			// + BaseTransformSnapshot + CreationLocation.  Other chunks
			// don't contribute (V1 save engine only round-trips object
			// transforms).  Gated on sawCloseBrace so a truncated chunk
			// that somehow ParseChunk-succeeded doesn't drive
			// OnStandardObjectFinalized with a stale closeBraceIdx.
			if( sawCloseBrace && tokens[0] == "standard_object"
			    && !mRawTokens.AllLines().empty() ) {
				const std::size_t closeBraceIdx = mRawTokens.AllLines().size() - 1;
				OnStandardObjectFinalized(
					pJob, chunkparams,
					outerLineIdx, openBraceIdx, closeBraceIdx );
			}

			// Phase 6.2: override_object chunks populate OverrideSpanIndex
			// with their byte range + the field set + managed/unmanaged
			// classification (tracked via mInsideManagedOverrideBlock,
			// flipped by sentinel comment detection in the outer loop).
			if( sawCloseBrace && tokens[0] == "override_object"
			    && !mRawTokens.AllLines().empty() ) {
				const std::size_t closeBraceIdx = mRawTokens.AllLines().size() - 1;
				OnOverrideObjectFinalized(
					pJob, chunkparams,
					outerLineIdx, openBraceIdx, closeBraceIdx );
			}

			// Phase B: camera / light / material / medium chunks populate
			// the SourceSpanIndex's entity map so the save engine's
			// property pass can splice their parameter lines.  Category
			// comes from the chunk parser's own descriptor — the same
			// descriptor that drove parsing — so this never drifts from
			// the registered chunk set.
			if( sawCloseBrace && pChunkParser
			    && !mRawTokens.AllLines().empty() ) {
				EntityCategory ec = EntityCategory::Object;
				const ChunkDescriptor& desc = pChunkParser->Describe();
				bool savableEntity = false;
				switch( desc.category ) {
					case ChunkCategory::Camera:
						ec = EntityCategory::Camera;   savableEntity = true; break;
					case ChunkCategory::Light:
						ec = EntityCategory::Light;    savableEntity = true; break;
					case ChunkCategory::Material:
						ec = EntityCategory::Material; savableEntity = true; break;
					case ChunkCategory::Medium:
						ec = EntityCategory::Medium;   savableEntity = true; break;
					default:
						break;
				}
				// Index ONLY named-entity PRODUCERS -- chunks whose DESCRIPTOR declares a `name` parameter.  A
				// name-OMITTED producer (homogeneous_medium / lambertian_material with no `name` line) still
				// creates a real runtime entity defaulting to "noname" and MUST be saveable.  A chunk in an
				// entity CATEGORY but with NO `name` param is a SETTINGS chunk (global_medium,
				// hosek_wilkie_skylight, scene_options, camera_defaults): none creates an entity keyed by its OWN name (hosek synthesizes a global radiance map + an internal sun), so indexing
				// its nameless span would collide on "noname" and corrupt a real one -- skip it.  Gate on the
				// DESCRIPTOR, not the source text (which would wrongly drop a name-omitted producer).
				bool producesNamedEntity = false;
				if( savableEntity )
					for( const ParameterDescriptor& pd : desc.parameters )
						if( pd.name == "name" ) { producesNamedEntity = true; break; }
				// Cameras are NOT carved out here.  A name-OMITTED camera's
				// runtime name is AllocateCameraName's "default" (auto-
				// suffixed), not "noname" -- we still index it, and
				// OnEntityChunkFinalized keys its span under that runtime name
				// (matching what the editor enumerates) rather than
				// ExtractObjectName's "noname".  That is what lets an unnamed
				// camera's property edits round-trip.
				if( producesNamedEntity ) {
					const std::size_t closeBraceIdx = mRawTokens.AllLines().size() - 1;
					OnEntityChunkFinalized(
						pJob, ec, chunkparams,
						outerLineIdx, openBraceIdx, closeBraceIdx );
				}
			}
		}
	}

	safe_release( parser );
	GlobalLog()->PrintEx( eLog_Info, "AsciiSceneParser: Successfully loaded \'%s\'", szFilename );

	// Phase 6.1 (§7.4): snapshot every object's CURRENT runtime
	// transform.  These are the "loaded" values — what the user
	// starts editing from.  Differs from BaseTransformSnapshot iff
	// an override_object chunk (Phase 6.2) ran during parse.
	//
	// Top-level only: the inner parse (via `> load`/`> run`) hasn't
	// seen the outer parse's remaining chunks yet — those may include
	// override_object lines that mutate the just-loaded child objects.
	// Snapshotting at the OUTER parse's end captures the fully-
	// composed state.
	if( depthGuard.isTopLevel ) {
		PopulateLoadedTransformSnapshot( pJob );
		// Phase B: capture loaded camera/light/material/medium/object
		// property values for the save engine's property-pass diff.
		PopulateLoadedPropertySnapshot( pJob );
	}

	// parser_entries unique_ptrs destroy the parsers when they go out of scope
	return true;
}

//////////////////////////////////////////////////
// Implementation of the macro substituion code
//////////////////////////////////////////////////

char AsciiSceneParser::substitute_macro( String& token )
{
	// A macro can be any part of a token
	std::string str( token.c_str() );
	std::string::size_type x = str.find_first_of( "@!" );

	std::string processed;

	if( x != std::string::npos ) {
		char macro_char = str[x];		// remember this, depending on whether its an @ or % we do different operations

		// We have a macro!
		if( x > 0 ) {
			processed = str.substr( 0, x );
		}
		str = str.substr( x+1, str.size() );

		// Find the end of the macro
		x = str.find_first_not_of( "ABCDEFGHIJKLMNOPQRSTUVWXYZ_" );

		std::string macro;
		if( x == std::string::npos ) {
			macro = str;
		} else {
			macro = str.substr( 0, x );
		}

		MacroTable::const_iterator it = macros.find( macro.c_str() );

		if( it == macros.end() ) {
			return 2;	// Error
		}

		// Re-assemble the string
		static const int MAX_BUF_SIZE = 64;
		char buf[MAX_BUF_SIZE] = {0};
		if( macro_char == '@' ) {
			snprintf( buf, MAX_BUF_SIZE, "%.12f", it->second );
		} else {
			snprintf( buf, MAX_BUF_SIZE, "%.4d", (int)it->second );
		}
		processed.append( buf );

		if( x<str.size() ) {
			processed.append( str.substr( x, str.size() ) );
		}

		token = processed.c_str();

		return 1;	// Successfull subsitution
	}

	return 0;	// No substituion
}

bool AsciiSceneParser::substitute_macros_in_tokens( String* tokens, const unsigned int num_tokens )
{
	for( unsigned int i=0; i<num_tokens; i++ ) {
		for(;;) {
			char c = substitute_macro( tokens[i] );

			if( c==0 ) {
				break;
			}

			if( c==2 ) {
				return false;
			}
		}
	}

	return true;
}

bool AsciiSceneParser::add_macro( String& name, String& value  )
{
	// Add a new macro
	std::string str( name.c_str() );

	// Make sure only valid things are in the macro
	if( str.find_first_not_of( "ABCDEFGHIJKLMNOPQRSTUVWXYZ_" ) != std::string::npos ) {
		return false;
	}

	// Check to see if it already exists
	if( macros.find( name ) != macros.end() ) {
		return false;
	}

	macros[name] = atof(value.c_str() );
	return true;
}

bool AsciiSceneParser::remove_macro( String& name )
{
	MacroTable::iterator it = macros.find( name );
	if( it == macros.end() ) {
		return false;
	}

	macros.erase( it );
	return true;
}
