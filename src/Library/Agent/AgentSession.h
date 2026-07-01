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
		//! A STRUCTURED set-param patch (slice 0b: set only -- no text-patch,
		//! no add/remove chunk; those are later slices).  Locates a named
		//! entity in the retained CST Document and sets one of its params to a
		//! new value string, routed through Job::ApplyCstParamEdit -- the SAME
		//! pathway the GUI property panel uses (L2: the agent is just another
		//! client of the edit surface).
		struct AgentSetPatch
		{
			std::string target;   //!< the entity NAME to edit (a chunk `name`; unnamed cameras resolve by kind)
			std::string kind;     //!< the entity KIND keyword (e.g. "material", "sphere_geometry", "camera") -- disambiguates a cross-category name clash; "" = any
			std::string param;    //!< the parameter role to set (e.g. "radius", "reflectance", "location")
			std::string value;    //!< the new value string (parsed by the derive layer per the param's declared kind)
		};

		//! The structured result of ProposePatch.  `applied` folds
		//! ApplyCstParamEdit's 0/1/2/3 return:
		//!   * 1/2 -> applied=true: the Document was mutated + the live Job
		//!            re-derived cleanly (1 incremental / 2 full re-derive).
		//!   * 3   -> applied=true, but the full re-derive EMITTED DIAGNOSTICS.
		//!            The source contract (Job.cpp DeriveEditedCstDocument_)
		//!            treats 3 as failure; slice 0b still folds it to
		//!            applied=true (the Document WAS mutated and the managers
		//!            WERE replaced) and surfaces it as applied-with-warning --
		//!            `rawCode` preserves the distinction so a caller can tell
		//!            a clean apply (1/2) from a diagnosed one (3).
		//!   * 0   -> applied=false: edit rejected; the head is byte-identical.
		//! `rawCode` preserves the underlying contract value; `message` is a
		//! human explanation (rebind codes 2/3 note the full re-derive).
		struct AgentPatchResult
		{
			bool        applied = false;
			int         rawCode = 0;      //!< 0 reject / 1 incremental / 2 D2 full re-derive / 3 replaced-but-diagnosed
			std::string message;
		};

		//! The structured result of Render: the rendered head as PNG bytes
		//! plus the film dims.  `ok` is false (and `png` empty) when no head
		//! is loaded or the render failed.
		//!
		//! `meanR/meanG/meanB` are the LINEAR (pre-sRGB, pre-quantization)
		//! per-channel means over all pixels -- a stable, order-independent
		//! image signature.  RISE's PT sampler draws from a per-worker RNG
		//! whose state depends on thread scheduling, so two renders of the
		//! same head are NOT byte-identical (the PNG stream diverges
		//! wholesale on a sub-LSB pixel change); the linear channel means, by
		//! contrast, differ only by the tiny MC noise floor between runs and
		//! shift measurably under a visible edit.  Callers wanting to compare
		//! images robustly (edit-changed-the-render, not-all-black) should use
		//! these, not raw PNG bytes.
		struct AgentRenderResult
		{
			bool                       ok = false;
			unsigned int               width = 0;
			unsigned int               height = 0;
			std::vector<unsigned char> png;   //!< 8-bit sRGB PNG bytes of the final image
			double                     meanR = 0.0;   //!< linear per-channel mean (order-independent image signature)
			double                     meanG = 0.0;
			double                     meanB = 0.0;
			std::string                message;
		};

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

			//! propose_patch (slice 0b: STRUCTURED set only).  Apply one
			//! param-value edit to the retained CST Document via
			//! Job::ApplyCstParamEdit -- the SAME call the GUI property panel
			//! makes -- then let that call re-derive the live Job (incremental
			//! or D2 full re-derive) so the head's derived Scene stays
			//! consistent with the mutated Document, EXACTLY as the GUI does.
			//! No retained Document -> applied=false with a clear message.
			//! Single-threaded headless: no revision / DocumentId precondition
			//! (that gating is slice 1).
			AgentPatchResult ProposePatch( const AgentSetPatch& patch );

			//! render + read_image (slice 0b): render the current head into an
			//! in-memory sRGB PNG and return the bytes + film dims.  Headless
			//! (no window).  A render NEVER mutates the retained Document --
			//! ReadDocument() is byte-identical across a Render call.
			//! `samplesOverride` is currently IGNORED (retained for API
			//! stability): a render-scoped sample override cannot be applied
			//! without either mutating the CST or a transient IRasterizer
			//! sample-count setter that does not exist in slice 0b, so it is
			//! deferred to the EffectiveRenderConfig layer
			//! (docs/agentic-redesign/50-agentic-surface.md §2.2.5); the render
			//! uses the AUTHORED sample count.  The bytes of a SUCCESSFUL
			//! render are cached for ReadImage().
			AgentRenderResult Render( int samplesOverride = -1 );

			//! The PNG bytes of the LAST successful Render (empty before the
			//! first render).  A convenience read of the cached result.
			std::vector<unsigned char> ReadImage() const;

		private:
			AgentSession( IJobPriv* job, bool owns );
			AgentSession( const AgentSession& );             // deleted
			AgentSession& operator=( const AgentSession& );  // deleted

			IJobPriv* mJob;    //!< the wrapped Job (owned iff mOwnsJob)
			bool      mOwnsJob;

			std::vector<unsigned char> mLastPng;   //!< cached PNG bytes of the last Render (for ReadImage)
		};
	}
}

#endif
