//////////////////////////////////////////////////////////////////////
//
//  AgentSession.h - the headless read/validate surface over a Job
//    (Facet 5, the agentic surface -- slice 0a).
//
//    The FIRST code of the agentic surface: a single-threaded, HEADLESS
//    session object wrapping a Job whose scene was loaded via the
//    canonical CST path (Model-B: Scene = derive(CST)).  It exposes the
//    three READ/VALIDATE verbs of the design
//    (docs/agentic-redesign/50-agentic-surface.md §2.2.1 read tools,
//    §2.2.4 `validate`) as plain C++ methods -- no JSON, no networking
//    (the JSON-RPC transport is slice 0c):
//
//      * ReadDocument() -> the canonical `.RISEscene` text of the head
//                          (SerializeCst of the retained CST Document).
//      * ReadSchema(kw) -> the descriptor-generated JSON schema (L6).
//      * Validate(text) -> a structured, side-effect-free check of a
//                          CANDIDATE scene text (THE keystone).
//
//    This slice deliberately ships NO mutating verb (propose_patch),
//    NO render, and NO transport -- those are 0b / 0c.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_AGENT_AGENTSESSION_
#define RISE_AGENT_AGENTSESSION_

#include <memory>
#include <string>
#include <vector>

#include "AgentDiagnostic.h"

namespace RISE
{
	class IJobPriv;

	namespace Agent
	{
		//! A headless, single-threaded read/validate session over a Job.
		//! NOT thread-safe (slice 0a is deliberately single-threaded -- no
		//! mutex).  Owns the Job iff it created it (LoadFromFile); a wrapped
		//! Job (WrapJob) is non-owning.
		class AgentSession
		{
		public:
			//! Load `path` into a fresh Job via the canonical CST path and
			//! return a session that OWNS that Job.  Returns null when the
			//! scene fails to load (not native-v7, or a derive error) so a
			//! caller can distinguish "no session" from an empty document.
			static std::unique_ptr<AgentSession> LoadFromFile( const std::string& path );

			//! Wrap an EXISTING Job (non-owning: the caller keeps ownership
			//! and must outlive the session).  Used when the GUI / a host
			//! already holds a CST-loaded Job (L2: the GUI is just another
			//! agent).  Null `job` returns null.
			static std::unique_ptr<AgentSession> WrapJob( IJobPriv* job );

			~AgentSession();

			//! The canonical `.RISEscene` text of the head -- SerializeCst of
			//! the retained CST Document.  Returns "" when the Job retains no
			//! Document (a Job not loaded via the CST path); `HasDocument()`
			//! distinguishes that empty-because-absent case from a genuinely
			//! empty scene.
			std::string ReadDocument() const;

			//! True iff the wrapped Job retains a CST Document (so
			//! ReadDocument / the head is meaningful).
			bool HasDocument() const;

			//! The descriptor-generated JSON schema (charter L6): one chunk
			//! when `keyword` is non-empty, else the whole grammar.
			std::string ReadSchema( const std::string& keyword = std::string() ) const;

			//! THE KEYSTONE.  Validate a CANDIDATE scene text with NO side
			//! effects on this session's Job: parse it to a CST, derive it
			//! into a THROWAWAY Job, and map the derive diagnostics into
			//! structured AgentDiagnostics with best-effort byte-offset
			//! localization (see AgentSession.cpp for the localization
			//! strategy and its honest slice-0 limits).  An empty result
			//! means "no errors" (the candidate is valid at the semantic
			//! phase this slice covers).
			std::vector<AgentDiagnostic> Validate( const std::string& candidateText ) const;

		private:
			AgentSession( IJobPriv* job, bool owns );
			AgentSession( const AgentSession& );             // deleted
			AgentSession& operator=( const AgentSession& );  // deleted

			IJobPriv* mJob;    //!< the wrapped Job (owned iff mOwnsJob)
			bool      mOwnsJob;
		};
	}
}

#endif
