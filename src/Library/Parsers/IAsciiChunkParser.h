//////////////////////////////////////////////////////////////////////
//
//  IAsciiChunkParser.h - Interface for the per-chunk parsers used by
//    AsciiSceneParser.  Was defined inline in AsciiSceneParser.cpp
//    until it was extracted here so that consumers of the scene
//    grammar (the scene-editor suggestion engine, future validators)
//    can include it without pulling in the 8000-line scene parser
//    implementation.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-24
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IASCII_CHUNKPARSER_
#define IASCII_CHUNKPARSER_

#include <vector>

#include "ChunkDescriptor.h"
#include "../Utilities/RString.h"

namespace RISE
{
	class IJob;

	class IAsciiChunkParser
	{
	protected:
		IAsciiChunkParser() {}

	public:
		typedef std::vector<String> ParamsList;

		virtual ~IAsciiChunkParser() {}

		// Describes this chunk's grammar: keyword, category, parameter
		// metadata.  Every concrete chunk parser implements this via a
		// static descriptor, and SceneEditorSuggestions / the syntax
		// highlighters / the generic parameter dispatcher all read it.
		// Pure-virtual so adding a new chunk parser cannot silently
		// surface as an empty grammar entry — the build fails until
		// Describe() is implemented.  Returned reference must remain
		// valid for the parser's lifetime; consumers that need longer-
		// lived copies should copy the descriptor out at registry-
		// build time.
		[[nodiscard]] virtual const ChunkDescriptor& Describe() const = 0;

		// Default ParseChunk: dispatches each input line via the
		// chunk's descriptor (validating the parameter name against
		// Describe().parameters), populates a ParseStateBag with the
		// matched values, then invokes Finalize to emit the
		// pJob.AddX call.  This is the single source of truth: an
		// input line whose name is not in the descriptor fails the
		// parse, eliminating the drift that the legacy hand-rolled
		// if/else chains permitted.  Override only for chunks that
		// genuinely need custom dispatch (none in the current tree).
		virtual bool ParseChunk( const ParamsList& in, IJob& pJob ) const;

		// Reads typed parameter values out of the bag and emits the
		// corresponding pJob.AddX call.  Pure-virtual so a parser
		// cannot ship without one and cannot accidentally leave its
		// emit-step out — the build fails until Finalize() is
		// implemented.  Combined with the default ParseChunk above,
		// this makes the descriptor the single source of truth for
		// what parameters the parser accepts: a parameter that is
		// not in the descriptor cannot reach Finalize, and a
		// parameter that is in the descriptor flows automatically.
		[[nodiscard]] virtual bool Finalize( const ParseStateBag& bag, IJob& pJob ) const = 0;
	};

	// Validates `params` against `desc` and populates `bag` with the typed
	// values -- the descriptor-driven parameter dispatch that ParseChunk uses
	// internally, exposed so the CST derive path (src/Library/Cst) binds
	// through the SAME live validation as the legacy parser rather than a
	// second, drifting validator. Returns false on any of three conditions:
	// a line with no space separating name from value (NOT logged); an
	// undeclared parameter name (logged); or a numeric-kind param whose value
	// has a non-finite (nan/inf) OR non-numeric token (logged). On success
	// `bag` holds the values for a subsequent Finalize().
	// Separating this validate+populate step from Finalize is what lets a
	// caller validate every chunk first and apply none on a VALIDATION failure
	// (an apply-time Finalize failure -- e.g. an unresolved reference -- is a
	// separate concern the caller handles when it invokes Finalize).
	[[nodiscard]] bool DispatchChunkParameters( const ChunkDescriptor& desc, ParseStateBag& bag, const IAsciiChunkParser::ParamsList& params );

	// Resets the chunk parsers' cross-chunk parse state -- the file-scope caches
	// some Finalize()s read/write WITHIN one scene (the uniformcolor_painter
	// colour cache that translucent_material's energy-conservation check reads,
	// the camera/scene-option default carry-over, the camera-name dedup set).
	// The legacy ParseAndLoadScene calls this at the START of every parse; the
	// CST derive path MUST do the same before deriving, so that state does not
	// leak between successive derives (or in from a prior legacy parse on the
	// same thread) -- the redesign's edit -> re-derive loop runs DeriveToJob
	// repeatedly. Without it, deriving scene A then scene B can give B a Job that
	// a fresh parse of B would not (e.g. spurious energy-auto-scaled painters).
	void ClearChunkParserState();
}

#endif
