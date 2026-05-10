//////////////////////////////////////////////////////////////////////
//
//  FilmIntrospection.h - Descriptor-driven introspection of the
//    scene's Film for the interactive editor's "Output Settings"
//    panel.
//
//    Looks up the ChunkDescriptor that the parser uses to LOAD the
//    `film` chunk, then produces a list of {name, kind, value-as-
//    string, description} tuples by reading the active IFilm's current
//    state.  Adding a parameter to FilmAsciiChunkParser::Describe()
//    automatically surfaces it in the panel.
//
//    Editing routes through IJobPriv::SetFilm so the Scene's resync-
//    every-camera contract fires — every camera's projection matches
//    the new Film before the next render pass begins.
//
//  See docs/INTERACTIVE_EDITOR_PLAN.md and docs/ARCHITECTURE.md
//  "Camera / Film / Output separation".
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_FILMINTROSPECTION_
#define RISE_FILMINTROSPECTION_

#include "../Interfaces/IFilm.h"
#include "../Interfaces/IJobPriv.h"
#include "../Parsers/ChunkDescriptor.h"
#include "../Utilities/RString.h"
#include "CameraIntrospection.h"   // CameraProperty reused as the panel-row struct
#include <vector>

namespace RISE
{
	class FilmIntrospection
	{
	public:
		//! Returns one CameraProperty per parameter the `film` chunk
		//! descriptor declares.  Descriptor-driven so adding a param to
		//! FilmAsciiChunkParser::Describe() automatically appears in the
		//! panel.  Returns empty vector if the descriptor isn't
		//! registered (which would indicate a parser-wiring bug).
		static std::vector<CameraProperty> Inspect( const IFilm& film );

		//! Parse `value` according to the descriptor kind and apply to
		//! the active Film via `job.SetFilm(...)`.  Calling SetFilm
		//! resyncs every camera's projection to the new dims and
		//! refreshes the Job-level FrameStore.  Returns false on parse
		//! failure, zero dimensions, or non-positive pixelAR.
		static bool SetProperty( IJobPriv& job,
		                         const String& name,
		                         const String& value );

		//! Read the named property's current value as a formatted string
		//! (in the same form `SetProperty` accepts).  Empty string for
		//! unknown properties.  Used by the undo path to capture the
		//! prev-value before a panel edit.
		static String GetPropertyValue( const IFilm& film,
		                                const String& name );
	};
}

#endif
