//////////////////////////////////////////////////////////////////////
//
//  IJobPriv.h - Priviledged interface which has getters
// 
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 21, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IJOBPRIV_
#define IJOBPRIV_

#include "IJob.h"
#include "IScenePriv.h"
#include "IGeometryManager.h"
#include "IObjectManager.h"
#include "ILightManager.h"
#include "ICameraManager.h"
#include "IPainterManager.h"
#include "IScalarPainterManager.h"
#include "IMaterialManager.h"
#include "IModifierManager.h"
#include "IFunction1DManager.h"
#include "IFunction2DManager.h"
#include "IShaderManager.h"
#include "IShaderOpManager.h"
#include "IRasterizer.h"

#include <string>
#include <vector>
#include "../Cst/Cst.h"   // Facet 5 slice 1a: CstHeadVersion (a trivial POD returned by value below; Cst.h is std-lib-only weight)

namespace RISE
{
	// Forward declarations — these types live in
	// `src/Library/SceneEditor/` so we keep the include light.  The
	// getters return raw pointers so this header doesn't have to
	// pull in unordered_map / Matrix4 / etc.
	class TransformSnapshot;
	struct FileIdentity;   // defined in SceneEditor/FileIdentity.h; CST-save external-mod guard reads it via GetCstLoadFileIdentity()

	//! Batched agent edits (2026-10-16): ONE element of IJobPriv::ApplyCstEditBatch.  Either a chunk
	//! INSERT (`isInsert`, `chunkText` -- the same text Job::ApplyCstInsertChunk takes) or a single
	//! param EDIT (the same (entityName, entityKind, role, occ, value) tuple Job::ApplyCstParamEditChecked
	//! takes; an empty `entityKind` means "no kind narrowing", exactly as a null kind does there).
	struct CstBatchEdit
	{
		bool        isInsert = false;
		std::string chunkText;
		std::string entityName;
		std::string entityKind;
		std::string role;
		std::string value;
		int         occ = 0;
		//! Param edit only: refuse it (outcome.refusedContainer) when the target resolves to a container
		//! `standard_object` IN THE BATCH'S EVOLVING DOCUMENT -- the agent route's surface-binding-on-a-
		//! container gate, set by the caller for a surface-binding param with a non-`none` value.
		bool        refuseSurfaceBindingOnContainer = false;
		//! Param edit only: the caller ADDRESSED a specific occurrence (propose_patch's `occurrence`) -- the
		//! batch's undo record then applies base's occurrence-drift guard to it (see CstBatchUndoOp).
		bool        occAddressed = false;
	};

	//! Batched agent edits (2026-10-16, review round 3): the OPERATION-LEVEL inverse of ONE landed batch element,
	//! captured against the document exactly as that element found it.  An AgentEditBatch history record is the
	//! ordered list of these: Undo applies them in REVERSE landing order to the CURRENT document (remove the
	//! inserted chunk; restore each touched param slot's prior value or absence), Redo re-applies them forward
	//! (re-insert the recorded chunk text after its recorded anchor; re-set each slot's post value), and either
	//! direction is realized by ONE dry-run-gated derive.  Nothing the batch did not touch is ever rewritten, so
	//! edits made outside the history (film / environment inspector edits) survive an Undo/Redo of the batch.
	struct CstBatchUndoOp
	{
		struct Slot
		{
			std::string role;
			int         occ = 0;
			bool        priorAbsent = true;
			std::string priorValue;
			bool        postAbsent = true;
			std::string postValue;
		};
		bool        isInsert = false;
		// INSERT: the exact chunk bytes, its identity, its top-level index right after its splice (the undo
		// tie-break among byte-identical twins), and the identity of the nearest chunk BEFORE it at splice time
		// ("" = no chunk before it) -- the redo anchor.
		std::string chunkText;
		std::string keyword;
		std::string namePath;          //!< Cst::ChunkNamePath ("keyword/name"), "" for an unnamed chunk
		int         chunkIndex = -1;
		std::string anchorKeyword;
		std::string anchorNamePath;
		bool        hasAnchor = false;
		//! Review round 5 (P1-2): the anchor's ORDINAL among the chunks sharing its identity (keyword + name
		//! path) -- unnamed chunks all share "" -- so Redo resolves the k-th, not the first; and whether the
		//! insert was spliced at the document END (Redo then appends at the end, whatever follows the anchor).
		int         anchorOrdinal = 0;
		bool        appendedAtEnd = false;
		// PARAM EDIT: how the FORWARD write addressed its target -- (entityName, entityKind, editRole) resolved
		// through Cst::DocFindByNameAnyRole + Cst::DocTransformOwnerId -- so Undo and Redo resolve it the SAME
		// way (review round 4); `postEntityName` is the entity's name AFTER the write (differs only when the
		// edit renamed it: Undo resolves by it, Redo by `entityName`).  `ownerKeyword` / `ownerNamePath` are the
		// resolved owner chunk's identity (diagnostic attribution only).  `slots` = every slot the write changed.
		std::string entityName;
		std::string entityKind;
		std::string editRole;
		std::string postEntityName;
		bool        occAddressed = false;
		//! The slots are the object's WHOLE transform (position / orientation / quaternion / scale / matrix):
		//! every one of them is re-applied, including one that was absent before AND after -- an editor gesture
		//! made after the batch and undone re-spells the pose (a `matrix` where the batch wrote a `position`),
		//! and that re-spelling must go too.  Otherwise only the slots the write CHANGED are applied.
		bool        wholeTransform = false;
		std::string ownerKeyword;
		std::string ownerNamePath;
		std::vector<Slot> slots;
		//! Undo fidelity (2026-10-17): the owner chunk's EXACT text before and after this element's write.  Undo
		//! swaps `ownerPostText` back to `ownerPreText` verbatim (and Redo the reverse) whenever the owner still
		//! holds the expected text -- replaying `slots` would re-spell the lines it restores and re-add a stripped
		//! param at the end of the chunk, which breaks an older insert record's exact-bytes inverse.  The slots
		//! stay the fallback for an owner something else has changed since.
		std::string ownerPreText;
		std::string ownerPostText;
	};

	//! Per-element outcome of IJobPriv::ApplyCstEditBatch.  `code` follows the SINGLE-element
	//! primitive's own contract: an insert reports Job::ApplyCstInsertChunk's 2/3/0/-1/-2, a param edit
	//! Job::ApplyCstParamEditChecked's 2/3/0 (never 1 -- a batch always realizes through ONE full
	//! re-derive).  `keyword`/`name` echo an insert's parsed identity; `diag` carries the refusal reason
	//! (the first dry-run diagnostic for a would-not-derive) or, for a landed element of a diagnosed batch
	//! (code 3), EVERY diagnostic of the batch's live re-derive, one per line.  For a LANDED element `undo` is
	//! its operation-level inverse and `landOrder` its position in the order elements were written into the
	//! committed document (batch order, except for elements an isolation retry landed later).
	struct CstBatchOutcome
	{
		int             code = 0;
		std::string     keyword;
		std::string     name;
		std::string     diag;
		int             insertedAt = -1;
		bool            isRasterizer = false;       //!< insert: the chunk is a `*_rasterizer` (Job's category+suffix rule)
		bool            refusedContainer = false;   //!< param: refused by the container gate (see CstBatchEdit)
		bool            wouldNotDerive = false;     //!< refused because it would not derive in context
		int             landOrder = -1;
		CstBatchUndoOp  undo;
	};

	//! IJobPriv::ApplyCstEditBatch's "isolation budget exhausted" return: NOTHING was committed, and the caller
	//! must fall back to committing the elements one at a time through the single primitives.
	const int kCstEditBatchNeedsSequential = -3;

	//! The object-transform params (they mask one another; see Job::PlanCstParamEdits_'s strip lists).
	inline bool CstIsObjectTransformRole( const std::string& r )
	{
		return r == "position" || r == "orientation" || r == "quaternion" || r == "scale" || r == "matrix";
	}

	//! Would re-writing the EARLIER object-transform param `earlier` after the LATER landed one `later` (both on
	//! the same entity) change the later write's effect?  Job::PlanCstParamEdits_'s masking rules (review round 5,
	//! directional rather than "every transform role is one slot"): a write strips exactly what outranks it
	//! (matrix > quaternion > orientation; position and scale are independent of everything but matrix), and a
	//! re-written param that outranks the later one masks it.  So the earlier write conflicts when
	//!   * it is the same role (the occurrence is compared by the caller);
	//!   * the later write is a `matrix` (re-writing any component strips it), or the earlier is (re-writing it
	//!     strips every later component);
	//!   * the pair is quaternion / orientation -- a re-written orientation strips a later quaternion, a
	//!     re-written quaternion masks a later orientation.
	//! position / scale / quaternion against one another (other than themselves) do not conflict.
	inline bool CstTransformRetryConflicts( const std::string& earlier, const std::string& later )
	{
		if( earlier == later ) return true;
		if( earlier == "matrix" || later == "matrix" ) return true;
		return ( earlier == "quaternion" && later == "orientation" ) ||
		       ( earlier == "orientation" && later == "quaternion" );
	}

	//! Batched agent edits (2026-10-16, review rounds 2-5): does an element AFTER `i` that LANDED write the same
	//! param slot (entity, role, occ) as param element `i` -- or, for the object-transform params, one a retry of
	//! `i` would strip or mask (CstTransformRetryConflicts)?  A retry of `i` would then overwrite the later
	//! value with an earlier one, so retry passes skip it.  An EMPTY kind on either side means "no kind
	//! narrowing" and therefore matches any kind.
	inline bool CstBatchLaterLandedWriteSameSlot( const std::vector<CstBatchEdit>& edits,
	                                              const std::vector<char>& landed, std::size_t i )
	{
		const CstBatchEdit& e = edits[i];
		if( e.isInsert ) return false;
		for( std::size_t j = i + 1; j < edits.size() && j < landed.size(); ++j ) {
			const CstBatchEdit& o = edits[j];
			if( !landed[j] || o.isInsert ) continue;
			const bool kindMatches = e.entityKind.empty() || o.entityKind.empty() || o.entityKind == e.entityKind;
			const bool sameRole = ( o.role == e.role && o.occ == e.occ ) ||
			                      ( CstIsObjectTransformRole( o.role ) && CstIsObjectTransformRole( e.role ) &&
			                        o.role != e.role && CstTransformRetryConflicts( e.role, o.role ) );
			if( o.entityName == e.entityName && kindMatches && sameRole )
				return true;
		}
		return false;
	}

	//! Batched agent edits (2026-10-16, review round 3): a batch is realized by ONE live re-derive, so when that
	//! re-derive reports diagnostics (code 3) every landed element shares them.  A diagnostic is attributed to an
	//! element ONLY when it OPENS with that element's own chunk identity -- `<keyword> `<name>`` (or the
	//! quote-delimited forms) -- never on a name merely QUOTED inside it, which is usually a REFERENCED entity
	//! ("voronoi2d_painter: gen 0 painter 'foo' not found" names foo, not the voronoi).  `diags` holds one
	//! diagnostic per line.  When nothing is attributed the caller marks EVERY landed element diagnosed.
	inline bool CstBatchDiagnosticIsFor( const std::string& diags, const std::string& keyword, const std::string& name )
	{
		if( keyword.empty() || name.empty() || diags.empty() ) return false;
		const std::string forms[3] = { keyword + " `" + name + "`", keyword + " '" + name + "'",
		                               keyword + " \"" + name + "\"" };
		std::size_t at = 0;
		while( at <= diags.size() ) {
			const std::size_t nl = diags.find( '\n', at );
			const std::string line = diags.substr( at, nl == std::string::npos ? std::string::npos : nl - at );
			for( int f = 0; f < 3; ++f )
				if( line.compare( 0, forms[f].size(), forms[f] ) == 0 ) return true;
			if( nl == std::string::npos ) break;
			at = nl + 1;
		}
		return false;
	}

	//! The bare name of a Cst::ChunkNamePath ("keyword/name" -> "name").
	inline std::string CstNameFromPath( const std::string& namePath )
	{
		const std::size_t slash = namePath.find( '/' );
		return slash == std::string::npos ? namePath : namePath.substr( slash + 1 );
	}

	//! IJobPriv - Priviledged interface with getters
	class IJobPriv : public virtual IJob
	{
	protected:
		IJobPriv(){};
		virtual ~IJobPriv(){};

	public:

		//
		// Getters
		//
		virtual IScenePriv*					GetScene() = 0;
		virtual IGeometryManager*			GetGeometries() = 0;
		virtual ICameraManager*				GetCameras() = 0;
		virtual IPainterManager*			GetPainters() = 0;
		virtual IScalarPainterManager*		GetScalarPainters() = 0;
		virtual IFunction1DManager*			GetFunction1Ds() = 0;
		virtual IFunction2DManager*			GetFunction2Ds() = 0;
		virtual IMaterialManager*			GetMaterials() = 0;
		virtual IModifierManager*			GetModifiers() = 0;
		virtual IObjectManager*				GetObjects() = 0;
		virtual ILightManager*				GetLights() = 0;
		virtual IRasterizer*				GetRasterizer() = 0;
		virtual IShaderManager*				GetShaders() = 0;
		virtual IShaderOpManager*			GetShaderOps() = 0;

		// Two transform snapshots the legacy streaming loader captured at
		// scene-load time (loader deleted, Model-B P5 Slice 6c).  The
		// CST-only load path never populates them — Job allocates them
		// empty so the getters return stable non-null pointers.  Currently
		// unused (no callers); retained pending deletion — the virtual
		// layout here is append-only.  (The related SourceSpanIndex /
		// OverrideSpanIndex byte-splice metadata was deleted in Slice 6d.)
		virtual TransformSnapshot*			GetBaseTransformSnapshotMutable() = 0;
		virtual const TransformSnapshot*	GetBaseTransformSnapshot() const = 0;
		virtual TransformSnapshot*			GetLoadedTransformSnapshotMutable() = 0;
		virtual const TransformSnapshot*	GetLoadedTransformSnapshot() const = 0;

		// Model-B P5 Slice 6a: the CST-load file identity (path + mtime + size) captured by
		// Job::RefreshCstLoadFileIdentity.  The CST-save external-modification guard reads THIS
		// so the guard has no dependency on any legacy load-time span index.  Job is the sole implementer.
		virtual const FileIdentity&			GetCstLoadFileIdentity() const = 0;

		// Facet 5 (live co-edit) slice 1a: the optimistic-concurrency identity of the retained CST
		// head -- (uuid, revision), see RISE::Cst::CstHeadVersion.  `uuid` is minted fresh on EVERY
		// load (incl. a reload of the same file) and is 0 when no CST head is retained; `revision`
		// bumps IFF the retained Document content changes.  The AgentSession baseHeadVersion conflict
		// precondition reads this to reject a stale patch.  Job is the sole implementer.
		virtual RISE::Cst::CstHeadVersion	GetCstHeadVersion() const = 0;

		// Model-B F2 slice S2a fix round 2 (P2-A): read-only accessor for the progress callback
		// installed via SetProgress -- null when none is installed.  Originally test-only (so a test
		// can assert the Job's progress hook is genuinely restored after a throwing render); since
		// the 2026-07-12 slot-ownership hardening it also has production callers (the macOS bridge's
		// setProgressBlock: refusal check reads it; the coordinator render paths use the atomic
		// ExchangeProgress instead for their capture).  Job is the sole implementer.
		// APPENDED AT THE TRUE END OF THE VIRTUAL TAIL (append-only ABI convention -- do NOT insert mid-tail).
		virtual IProgressCallback*			GetProgress() const = 0;

		// 87 step 4a round-4 (P1-A): how many times this Job's container set has been BUILT --
		// once at construction, once more per ClearAll (i.e. per DestroyContainers +
		// InitializeContainers pair), AND once per SetPrimaryAcceleration, which
		// replaces the ObjectManager on its own without going through
		// InitializeContainers.  Those are the two sites; an earlier version of this
		// comment claimed InitializeContainers was the only one, which was FALSE.
		// ANY future code that releases and recreates a manager must bump it too.
		// Monotonic for the Job's lifetime.
		//
		// THE POINT: a registration serial is NOT a cross-rebuild identity.
		// GenericManager::m_nNextSerial is per-manager-INSTANCE and restarts at construction, so a
		// ClearAll + re-derive of the same document re-registers in the same order and hands out
		// BYTE-IDENTICAL serials to genuinely new instances.  Any cache that keys on (name, serial)
		// and can outlive a rebuild -- SceneEditController's authored-tree snapshot is the one in
		// tree -- must compose this counter in, or a stale key silently re-resolves onto the
		// rebuild's fresh entity.  Reachable on the mainline: Job::ApplyCstParamEdit returning 2/3
		// (the D2 fallback taken by every edit the incremental path refuses), RederiveCstWithVariant,
		// and reopening a document into a reused Job -- the GUIs keep ONE Job and clearAll+load.
		// Job is the sole implementer.
		// APPENDED AT THE TRUE END OF THE VIRTUAL TAIL (append-only ABI convention).
		virtual unsigned long long			GetContainerRebuildCount() const = 0;

		// Batched agent edits (2026-10-16): apply `edits` IN ORDER to the retained CST Document and
		// realize every element that derives with ONE dry-run-guarded full re-derive -- instead of one
		// re-derive per element, which is what a loop over ApplyCstInsertChunk / ApplyCstParamEditChecked
		// costs (each of those dry-runs AND fully re-derives the whole scene).
		//
		// Every element is validated with the single primitives' own rules, in order, against the document
		// the earlier ACCEPTED elements left (a later element may reference an earlier one; a duplicate of an
		// earlier one is refused).  An element lands iff it is valid and the accepted document still derives
		// with it in, judged on the END state (an element listed before what it references lands) -- the same
		// verdict whatever else the batch holds: when the whole batch does not derive, the elements are
		// isolated by in-order halving over throwaway dry-runs and every would-not-derive element then gets
		// ONE in-order retry against the final accepted document.  A refused insert gets the single verb's
		// positioned-then-append retry.  If the CURRENT document itself does not derive, every element is
		// refused with that diagnostic.
		//
		// Isolation spends at most `maxIsolationDryRuns` dry-runs (< 0 = a default sized for two offenders, 4*ceil(log2 N) + 10, capped at 32);
		// beyond it NOTHING is committed and kCstEditBatchNeedsSequential is returned.
		//
		// Returns 0 when NOTHING landed (retained Document AND live scene byte-identical, head version
		// unbumped), else 2 (replaced, clean) / 3 (replaced, diagnosed) -- the caller MUST re-point cached
		// scene/manager pointers on 2/3 -- with the head revision bumped ONCE for the whole batch.
		// `outcomes` is resized to edits.size().  Job is the sole implementer.
		// APPENDED AT THE TRUE END OF THE VIRTUAL TAIL (append-only ABI convention).
		virtual int							ApplyCstEditBatch( const std::vector<CstBatchEdit>& edits,
		                                                       std::vector<CstBatchOutcome>& outcomes,
		                                                       int maxIsolationDryRuns = -1 ) = 0;

		// Batched agent edits (2026-10-16): apply ONE element to the caller's `doc` exactly as
		// ApplyCstEditBatch's validation does (no derive, no mutation of this Job, no refusal logging).
		// Returns 1 when written, else the refusal code (outcome filled).  Lets a caller that DEFERS
		// elements keep an incremental preview of the document the batch will produce.
		virtual int							ApplyCstBatchElement( RISE::Cst::Document& doc, const CstBatchEdit& edit,
		                                                          CstBatchOutcome& outcome ) = 0;

		// Batched agent edits (2026-10-16): copy the retained CST Document (a cheap persistent-structure
		// copy).  False when none is retained.
		virtual bool						CopyCstDocument( RISE::Cst::Document& out ) const = 0;

		// Batched agent edits (2026-10-16, review round 4): IJob::ApplyCstReplaceDocumentText for an already-built
		// Document, installed AS IS -- no serialize + re-parse, so its top-level item structure (which OLDER
		// history records' exact-index inverses rely on) is preserved.  Same 0 / 2 / 3 contract.
		virtual int							ApplyCstReplaceDocument( const RISE::Cst::Document& doc,
		                                                             bool restoreActiveRasterizer,
		                                                             char* outDiag, unsigned int diagMax,
		                                                             const char* diagContext ) = 0;

		// Batched agent edits (2026-10-16): the agent route's CONTAINER rule -- does (entityName, entityKind)
		// resolve, in `doc`, to a `standard_object` naming no resolvable `geometry` (`geometry none` counts)?
		virtual bool						CstTargetIsContainerObject( const RISE::Cst::Document& doc,
		                                                                const char* entityName,
		                                                                const char* entityKind ) const = 0;

		// Undo fidelity (2026-10-17): the EXACT text of the chunk a param edit addressed as (entityName, entityKind,
		// role) lands on in the retained Document -- resolved by the SAME calls the write makes (DocFindByNameAnyRole
		// with the camera / kind-addressed-singleton fallback rules, then the DocTransformOwnerId owner walk).
		// False when there is no retained Document or the target does not resolve.
		virtual bool						CstParamEditOwnerChunkText( const char* entityName, const char* entityKind,
		                                                                const char* role, std::string& out ) const = 0;

		// Undo fidelity (2026-10-17): the verbatim inverse of a param edit -- resolve the owner chunk exactly as
		// CstParamEditOwnerChunkText does and, ONLY when it serializes to exactly `expectText`, replace it with
		// `newText` byte for byte (Cst::DocReplaceChunkIfText), then re-derive through the same full-derivability-
		// gated tail ApplyCstParamEditChecked uses.  Returns -1 when the target does not resolve or does not hold
		// `expectText` (NOTHING changed -- the caller falls back to replaying values), else that tail's 0/1/2/3.
		// APPENDED AT THE TRUE END OF THE VIRTUAL TAIL (append-only ABI convention).
		virtual int							ApplyCstParamEditOwnerRestore( const char* entityName, const char* entityKind,
		                                                                   const char* role, const std::string& expectText,
		                                                                   const std::string& newText ) = 0;
	};


	//! Creates a new empty job
	bool RISE_CreateJobPriv( 
			IJobPriv** ppi										///< [out] Pointer to recieve the job
			);
}

#endif
