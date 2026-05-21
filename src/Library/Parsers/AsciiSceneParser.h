//////////////////////////////////////////////////////////////////////
//
//  AsciiSceneParser.h - Defines the AsciiSceneParser class.  The 
//    AsciiSceneParser parses an ascii scene file.  This is the new
//    ascii scene format introduced on December 22, 2003.  The scene
//    is broken down into chunks where each chunk can be parsed
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 22, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ASCII_SCENEPARSER_
#define ASCII_SCENEPARSER_

#include "../Interfaces/ISceneParser.h"
#include "../Utilities/Reference.h"
#include "../Utilities/RString.h"
#include "../Utilities/Math3D/Math3D.h"
#include "RawTokenCapture.h"
#include <istream>
#include <map>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class AsciiSceneParser : public virtual ISceneParser, public virtual Reference
		{
		protected:
			virtual ~AsciiSceneParser();

			char		szFilename[1024];

			typedef std::map<String,Scalar> MacroTable;
			typedef std::map<String,String> StringMacroTable;

			MacroTable	macros;
			StringMacroTable string_macros;

			// Phase 0 (docs/ROUND_TRIP_SAVE_PLAN.md §6.2): captures raw
			// pre-substitution tokens + byte offsets for each line of
			// the scene file.  Owned for the duration of a single
			// ParseAndLoadScene() call.  Phase 6.1's `SourceSpanIndex`
			// builder reads from this when a chunk is finalized so the
			// save engine can later decide between Mode A (in-place
			// line rewrite) and Mode B (managed override block) per
			// parameter.  Held by value here because RawTokenCapture
			// owns its own storage and is cheap to default-construct.
			RawTokenCapture		mRawTokens;

			// Phase 6.1: FOR-revisit detection map.  Keyed by the byte
			// offset of the chunk-keyword token on the chunk's header
			// line (which is stable across FOR-body re-reads); value is
			// the runtime name of the FIRST entity that came out of
			// that chunk.  On a subsequent visit we flip that entity's
			// SourceSpan.chunkRevisited = true and SKIP building a new
			// SourceSpan for the 2..N entity.
			std::map<std::size_t, std::string> mChunkHeaderOffsetToFirstName;

			// Phase 6.1 helpers.  See impl in AsciiSceneParser.cpp.
			void OnStandardObjectFinalized(
				IJob& pJob,
				const std::vector<String>& chunkparams,
				std::size_t chunkHeaderIdx,
				std::size_t openBraceIdx,
				std::size_t closeBraceIdx );
			void PopulateLoadedTransformSnapshot( IJob& pJob );

			// Phase 6.2: detection state for the managed override
			// sentinel block.  Flipped TRUE when the parser sees the
			// `# === RISE editor overrides ...` opening comment, FALSE
			// on the `# === end RISE editor overrides ===` closing
			// comment.  Read by OnOverrideObjectFinalized to record
			// each chunk as managed-or-not in OverrideSpanIndex.
			bool mInsideManagedOverrideBlock = false;

			// Phase 6.2: record an override_object chunk's bytes +
			// field set in the Job-owned OverrideSpanIndex.
			void OnOverrideObjectFinalized(
				IJob& pJob,
				const std::vector<String>& chunkparams,
				std::size_t chunkHeaderIdx,
				std::size_t openBraceIdx,
				std::size_t closeBraceIdx );

			char substitute_macro( String& token );
			bool substitute_macros_in_tokens( String* tokens, const unsigned int num_tokens );

			bool add_macro( String& name, String& value );
			bool remove_macro( String& name );

			// The LOOP strucuture contains the data we need to 
			// iterate through loops
			struct LOOP
			{
				std::istream::pos_type position;			// Position in the file that loop contents begin at
				String var;									// Loop variable
				Scalar curvalue;							// Current value of the loop var
				Scalar endvalue;							// The value to end the loop at
				Scalar increment;							// The increment amount
				unsigned int linecount;						// The line count at the begining of the loop
			};

		public:
			AsciiSceneParser( const char * szFilename_ );
			bool ParseAndLoadScene( IJob& pJob );
		};
	}
}

#endif
