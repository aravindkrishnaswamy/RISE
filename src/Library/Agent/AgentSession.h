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

#include "../Cst/Cst.h"   // Facet 5 slice 1a: RISE::Cst::CstHeadVersion (the (uuid,revision) optimistic-concurrency identity)

namespace RISE
{
	class IJobPriv;
	class SceneEditController;   // Facet 5 slice 1b: LIVE mode routes ProposePatch through the controller's render-safe edit path (fwd-decl only -- no header dep)

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

			//! Facet 5 slice 1a: OPTIONAL optimistic-concurrency precondition.  When `hasBaseVersion` is
			//! true, ProposePatch REJECTS the patch with a CONFLICT (WITHOUT mutating) unless `baseVersion`
			//! equals the Job's CURRENT retained head-version -- so a patch built against a stale head (the
			//! head moved since the agent last read it) is rejected rather than silently clobbering the newer
			//! head (docs/agentic-redesign/50-agentic-surface.md §2.2.2 baseHeadVersion precondition).  When
			//! false (the default), the edit is UNCONDITIONAL (slice-0 back-compat: no gating).
			bool                     hasBaseVersion = false;
			RISE::Cst::CstHeadVersion baseVersion;   //!< the head-version the patch was built against (checked iff hasBaseVersion)
		};

		//! The structured result of ProposePatch.  `applied` means CLEAN
		//! SUCCESS ONLY, and `status` is the tri-state gate a client keys on;
		//! together they fold ApplyCstParamEdit's 0/1/2/3 return HONESTLY:
		//!   * 1/2 -> applied=true,  status="applied": the Document was mutated
		//!            + the live Job re-derived CLEANLY (1 incremental /
		//!            2 full re-derive).  The ONLY clean-success codes.
		//!   * 3   -> applied=FALSE, status="diagnosed": the Document WAS
		//!            mutated and the live managers WERE replaced, BUT the full
		//!            re-derive EMITTED DIAGNOSTICS.  The source contract
		//!            (Job.cpp DeriveEditedCstDocument_) treats 3 as FAILURE
		//!            ("the edit FAILED -- treat as failure"), so slice 0 does
		//!            too: `applied` is false and the caller MUST NOT proceed as
		//!            if the patch cleanly succeeded.  This is DISTINCT from a
		//!            reject: the head is NOT byte-identical -- the Document was
		//!            mutated and the managers replaced -- so `message` states
		//!            that nuance plainly (a caller must not assume nothing
		//!            changed either).
		//!   * 0   -> applied=false, status="rejected": edit refused; the head
		//!            is byte-identical (nothing changed).  The pre-flight
		//!            guards (no Document / empty target/param/value, and --
		//!            LIVE mode only -- the controller's open-editor-
		//!            transaction refusal) map here too -- they are refusals
		//!            with an unchanged head.
		//! Facet 5 slice 1a adds a FOURTH status, the optimistic-concurrency
		//! CONFLICT: when the patch carries `hasBaseVersion` and its `baseVersion`
		//! does NOT equal the Job's CURRENT head-version, ProposePatch returns
		//!   * CONFLICT -> applied=FALSE, status="conflict", rawCode=0: the base
		//!            precondition failed, so the patch was REJECTED WITHOUT
		//!            touching the Document (the head is byte-identical -- a stale
		//!            patch must never mutate).  Distinct from "rejected": the
		//!            entity/param may be perfectly valid; the patch is just stale.
		//!            The caller should re-read the head and re-propose against the
		//!            new `headVersion` this result carries.
		//! `rawCode` preserves the underlying contract value (0/1/2/3) for a
		//! caller that wants the raw code (0 for a conflict, since nothing was
		//! applied); `status` is the recommended gate:
		//!   applied  <=> a CLEAN apply (rawCode 1 or 2);
		//!   status   in {"applied","rejected","diagnosed","conflict"}.
		//! `headVersion` is the Job's head-version AFTER the call: the POST-COMMIT
		//! head on a clean apply (its revision bumped), and the CURRENT (unchanged)
		//! head on a reject / diagnosed / conflict.  A caller keying on optimistic
		//! concurrency reads it to learn where the head now is.
		//! `message` is a human explanation (rebind codes 2/3 note the full
		//! re-derive; the code-3 message spells out mutated-but-diagnosed; the
		//! conflict message reports the revision the head moved to).
		//! `retriable` disambiguates the "rejected" bucket for a machine
		//! client: true means the refusal is TRANSIENT -- resubmitting the
		//! IDENTICAL patch later can succeed.  Today the only transient
		//! reject is the LIVE-mode controller's open-editor-transaction
		//! refusal (retry after the gesture completes); permanent rejects
		//! (no Document / empty field / unknown entity / bad value) stay
		//! false -- retrying them verbatim can never succeed.  A version
		//! CONFLICT does NOT set it: conflicts carry their own
		//! status="conflict" and are retriable-by-protocol via re-read
		//! (re-read the head, rebase, re-propose), not by verbatim
		//! resubmission.  Carried 1:1 from the controller's
		//! AgentCommitResult in LIVE mode; the direct (headless) path has
		//! no transaction surface, so its rejects are all permanent.
		struct AgentPatchResult
		{
			bool        applied = false;
			bool        retriable = false;   //!< always emitted on the wire; meaningful for status="rejected" only: true = transient refusal (open editor transaction) -- retry the SAME patch later; false = permanent
			int         rawCode = 0;         //!< 0 reject/conflict / 1 incremental / 2 D2 full re-derive / 3 replaced-but-diagnosed
			std::string status;              //!< "applied" (clean) / "rejected" (head intact) / "diagnosed" (mutated but re-derive diagnosed) / "conflict" (stale baseVersion, head intact)
			RISE::Cst::CstHeadVersion headVersion;   //!< the head-version AFTER the call (post-commit on success; current head on reject/diagnosed/conflict)
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

		//! Facet 5 slice S1: one entry of the skills INDEX -- `name` is the
		//! skill's filename minus ".md" (the handle a client passes back to
		//! fetch the full markdown), `title` the "# ..." first line, `hook`
		//! the "> hook: ..." second line (the one-line when-to-read cue).
		struct AgentSkillEntry
		{
			std::string name;
			std::string title;
			std::string hook;
		};

		//! Facet 5 slice S1: the result of ReadSkill.  Progressive
		//! disclosure: an EMPTY requested name returns the INDEX (`index`
		//! filled, `markdown` empty); a named request returns the full
		//! `markdown`.  `ok` is false (and `error` states why) for an
		//! unknown skill name or a rejected (unsafe / non-bare) name.
		struct AgentSkillResult
		{
			bool                         ok = false;
			std::string                  error;      //!< filled iff !ok
			std::string                  name;       //!< the requested name (named fetch)
			std::string                  markdown;   //!< the full skill markdown (named fetch)
			std::vector<AgentSkillEntry> index;      //!< the index (empty-name fetch)
			std::string                  note;       //!< index-only advisory: set when the skills ROOT DIRECTORY itself was not found (distinguishing a miswired root from a present-but-empty one; both give an empty index)
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

			//! Facet 5 slice 1b: attach a LIVE SceneEditController so
			//! ProposePatch routes its set-param commit through the
			//! controller's render-thread-SAFE edit path (cancel-and-park +
			//! rebind-after-D2) instead of calling Job::ApplyCstParamEdit
			//! DIRECTLY.  Attach this when the session shares a Job with a
			//! running interactive editor (a live GUI), so an agent commit
			//! cannot race the render thread or dangle the editor's cached
			//! pointers on a D2 re-derive.  Pass `nullptr` to DETACH (revert
			//! to the direct-Job path).  The controller is BORROWED -- the
			//! caller keeps ownership and must outlive the session (or detach
			//! first).  The controller's Job MUST be the SAME Job this session
			//! wraps (the caller's responsibility; a live GUI builds both over
			//! one Job).  When NOT attached (the default / headless slice-0
			//! mode), ProposePatch is byte-for-byte its prior behaviour.
			void AttachController( SceneEditController* controller );

			//! True iff a live controller is attached (ProposePatch routes
			//! through it).
			bool HasController() const { return mController != nullptr; }

			//! The canonical `.RISEscene` text of the head -- SerializeCst of
			//! the retained CST Document.  Returns "" when the Job retains no
			//! Document (a Job not loaded via the CST path); `HasDocument()`
			//! distinguishes that empty-because-absent case from a genuinely
			//! empty scene.
			std::string ReadDocument() const;

			//! True iff the wrapped Job retains a CST Document (so
			//! ReadDocument / the head is meaningful).
			bool HasDocument() const;

			//! Facet 5 slice 1a: the retained CST head's (uuid,revision)
			//! optimistic-concurrency identity (see RISE::Cst::CstHeadVersion).
			//! {0,0} when there is no wrapped Job (or the Job retains no head).
			//! An agent reads this alongside ReadDocument, then passes it back as
			//! a patch's baseVersion so a stale edit is rejected with a CONFLICT.
			RISE::Cst::CstHeadVersion HeadVersion() const;

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

			//! The STATELESS validation core.  `Validate()` above is a thin
			//! forwarder to this; the logic references NO member state (it
			//! parses `candidateText` to a CST and derives it into a THROWAWAY
			//! Job -- never this session's mJob), so it is exposed as a static
			//! for the transport's no-head bootstrap: an agent CONSTRUCTING or
			//! REPAIRING a scene from scratch (the CLI's `--agent-stdio` with
			//! no scene loaded) must be able to `validate` a candidate BEFORE
			//! any head exists.  Identical result to `Validate()`.
			static std::vector<AgentDiagnostic> ValidateText( const std::string& candidateText );

			//! Facet 5 slice S1: read_skill -- STATELESS, like ReadSchema /
			//! ValidateText (references NO member state; exposed static so the
			//! transport's no-head bootstrap can read skills before any head
			//! exists).  Progressive disclosure: an EMPTY `name` returns the
			//! INDEX (name + title + one-line hook per skill, scanned from the
			//! *.md files in the skills root); a non-empty `name` returns that
			//! skill's full markdown.
			//!
			//! SKILLS ROOT RESOLUTION (first hit wins):
			//!   1. $RISE_SKILLS_PATH               (when set and non-empty)
			//!   2. $RISE_MEDIA_PATH + "skills/agent/"  (when set and non-empty)
			//!   3. "./skills/agent/"               (cwd fallback -- the repo
			//!      root when run from a checkout, matching run_all_tests.sh)
			//! Reads are READ-ONLY; nothing is ever written under the root.
			//!
			//! PATH SAFETY: `name` must be a BARE filename component -- any
			//! '/', '\\', or ".." is REJECTED (no traversal), and only files
			//! ending ".md" inside the root are served (the ".md" suffix is
			//! appended by this call; the caller passes the bare skill name).
			//! An unknown name -> ok=false with a clean error message.
			//!
			//! MEMBERSHIP: a named fetch is served ONLY for a name present in
			//! the index (the same regular-*.md-files listing the no-name call
			//! returns) -- the fetchable set IS the listed set.  So dotfiles,
			//! directories named "<x>.md", FIFOs, and Windows device names
			//! (CON / NUL) are never opened.  SYMLINKS ARE FOLLOWED: a symlink
			//! under the root that resolves to a regular file is listed and
			//! served.  That is deliberate and honest -- this is a
			//! trusted-OPERATOR surface (the root comes from the operator's
			//! environment above, never from agent input; the agent only picks
			//! names off the operator's own index).
			static AgentSkillResult ReadSkill( const std::string& name = std::string() );

			//! propose_patch (slice 0b: STRUCTURED set only).  Apply one
			//! param-value edit to the retained CST Document via
			//! Job::ApplyCstParamEdit -- the SAME call the GUI property panel
			//! makes -- then let that call re-derive the live Job (incremental
			//! or D2 full re-derive) so the head's derived Scene stays
			//! consistent with the mutated Document, EXACTLY as the GUI does.
			//! The result's `applied` is TRUE only for a CLEAN apply (rawCode
			//! 1 or 2); `status` is the tri-state gate {"applied","rejected",
			//! "diagnosed"} (see AgentPatchResult).  A rawCode-3 re-derive
			//! (mutated + managers replaced BUT diagnostics emitted) maps to
			//! applied=FALSE / status="diagnosed" -- the source contract treats
			//! 3 as a failure, so this surface does NOT report it as a success.
			//! No retained Document (or an empty target/param/value) ->
			//! applied=false, status="rejected", head byte-identical.
			//! Facet 5 slice 1a: optimistic concurrency.  When `patch.hasBaseVersion`
			//! is set, the base precondition is checked FIRST, BEFORE any mutation:
			//! if `patch.baseVersion` != the Job's current head-version the patch is
			//! REJECTED with status="conflict" (applied=false, head byte-identical) --
			//! a stale patch never touches the Document.  On every path the result's
			//! `headVersion` is populated (post-commit on a clean apply; the current
			//! head on reject/diagnosed/conflict).  Absent baseVersion -> the edit is
			//! UNCONDITIONAL (slice-0 back-compat).
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

			//! Facet 5 slice 1b: the attached LIVE controller (null = headless
			//! direct-Job mode).  BORROWED (never released); the caller owns it.
			//! When non-null, ProposePatch delegates to
			//! SceneEditController::ApplyAgentParamEdit.
			SceneEditController* mController = nullptr;

			std::vector<unsigned char> mLastPng;   //!< cached PNG bytes of the last Render (for ReadImage)
		};
	}
}

#endif
