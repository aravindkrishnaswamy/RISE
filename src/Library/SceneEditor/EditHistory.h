//////////////////////////////////////////////////////////////////////
//
//  EditHistory.h - Bounded undo/redo stack of SceneEdit records.
//    Also tracks the set of object names that have been mutated
//    versus the loaded baseline, which the round-trip serializer
//    consults on Save (Phase 6 / Phase A).
//
//  See docs/INTERACTIVE_EDITOR_PLAN.md.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_EDITHISTORY_
#define RISE_EDITHISTORY_

#include "SceneEdit.h"
#include <deque>
#include <set>

namespace RISE
{
	class EditHistory
	{
	public:
		//! Default byte budget (S20 review round 1 P2-4): the ENTRY cap alone
		//! (`maxEntries`, default 1024) bounds the undo stack's LENGTH, not its
		//! WEIGHT.  Three op kinds carry a whole retained-Document text copy per
		//! payload field rather than a typed value or a single chunk's bytes --
		//! `AgentDuplicateNode` and `AgentReplaceGeometry` carry TWO
		//! (`propertyValue` = pre-doc, `prevPropertyValue` = post-doc; both are
		//! `Job::ApplyCstReplaceDocumentText` swaps, so both directions need the
		//! full text), `AgentRemoveChunks` carries ONE (`propertyValue` = the
		//! pre-batch doc; its `prevPropertyValue` is a short per-target
		//! `kind\tname` redo descriptor, not document-sized) -- see each op's
		//! own doc in SceneEdit.h.  On a large ("sombrero-scale") document, 1024
		//! entries of up to 2x that document's bytes each is GIGABYTES held
		//! live in the undo stack alone.  256MB is generous headroom for
		//! interactive editing on any document RISE can load into memory
		//! comfortably, while still bounding the pathological case.
		static constexpr unsigned long long kDefaultByteBudget = 256ull * 1024ull * 1024ull;

		//! `maxBytes` bounds the SUM of every undo-stack entry's heavyweight
		//! document-payload bytes (see `kDefaultByteBudget`'s doc for exactly
		//! which fields count) -- evaluated ALONGSIDE `maxEntries` in
		//! `TrimToMax`, oldest-first, same as the entry cap.  Pass 0 to disable
		//! the byte budget entirely (entry-cap-only behaviour, the pre-P2-4
		//! contract) -- no production caller does this; it exists for a test
		//! that wants to isolate the entry-cap path.
		EditHistory( unsigned int maxEntries = 1024, unsigned long long maxBytes = kDefaultByteBudget );
		~EditHistory();

		//! Append an edit (forward op) to the undo stack and clear
		//! the redo stack.  Honors the bounded-size invariant by
		//! dropping the oldest entries when the cap is reached.
		void Push( const SceneEdit& edit );

		//! Pops the most recent edit and returns it (untouched —
		//! caller invokes the inverse via SceneEditor::Apply).
		//! Returns false if the stack is empty.
		bool PopForUndo( SceneEdit& outEdit );

		//! Re-applies an edit that was previously undone.
		//! Returns false if the redo stack is empty.
		bool PopForRedo( SceneEdit& outEdit );

		/// P1: reverse the last PopForUndo when its revert failed -- move the
		/// most recently popped edit back from the redo stack to the undo stack.
		void RestoreLastUndoFromRedo();

		/// P1: reverse the last PopForRedo when its forward mutation failed -- move
		/// the most recently popped edit back from the undo stack to the redo stack.
		void RestoreLastRedoFromUndo();

		//! Discard the redo stack ONLY, leaving the undo stack and the
		//! dirty-object set untouched.  The transactional rollback uses
		//! this AFTER it has reverted live state by applying the inverse
		//! edits (via SceneEditor::Undo, which moves each reverted record
		//! onto the redo stack): a rolled-back gesture must NOT be
		//! redoable, so the redo residue those inverse-applies left behind
		//! is dropped.  By the time rollback calls this -- after SceneEditor::Undo's
		//! seq-walk has reverted the undo stack to the transaction baseline
		//! depth -- only the redo stack needs clearing.  No-op when the redo stack is empty.
		void ClearRedo();

		//! Drop everything.
		void Clear();

		//! Has this object's transform been touched at least once since
		//! the scene was loaded?  Includes edits that have been undone —
		//! undone edits still represent "user touched it."  HISTORICAL:
		//! fed the pre-CST round-trip save's per-object re-emit (serializer
		//! deleted, Model-B P5 Slice 6d; today's SaveEngine serializes the
		//! whole Document and never consults this).  No production
		//! consumers remain — exercised by tests only.
		bool IsObjectDirty( const String& name ) const;

		//! Iterate dirty objects.  Yields each name once.  Currently
		//! unused (no callers); retained pending deletion.
		template <class Fn>
		void EnumerateDirtyObjects( Fn fn ) const
		{
			for( std::set<String, StringLess>::const_iterator it = mDirtyObjects.begin();
			     it != mDirtyObjects.end(); ++it )
			{
				fn( *it );
			}
		}

		//! F2 (sequence-marker rollback): the seq the NEXT pushed edit will
		//! get.  A transaction records this at Begin; rollback undoes while
		//! the top edit's seq >= the recorded marker -- robust to front-trim
		//! (which the 1024-cap depth-only baseline was not).
		unsigned long long NextSeq() const { return mNextSeq; }

		//! Peek the most-recent (top) undo edit's historySeq without popping.
		//! Returns false when the undo stack is empty.
		bool PeekUndoSeq( unsigned long long& outSeq ) const;

		//! Highest historySeq ever dropped by TrimToMax.  Rollback uses this
		//! to detect that a transaction edit was trimmed (seq >= marker) and
		//! report an honest partial rollback.
		unsigned long long MaxTrimmedSeq() const { return mMaxTrimmedSeq; }

		//! Whether TrimToMax has dropped ANY entry (guards the MaxTrimmedSeq
		//! comparison: seq 0 is a valid edit id, so a bare >= would false-flag).
		bool DidTrim() const { return mDidTrim; }

		unsigned int UndoDepth() const;
		unsigned int RedoDepth() const;

		//! P2-4: the SUM of every current undo-stack entry's heavyweight
		//! document-payload bytes (see `kDefaultByteBudget`'s doc) -- what
		//! `TrimToMax` compares against `ByteBudget()`.  Exposed for tests and
		//! for a future UI memory readout; not consulted by any production
		//! decision besides `TrimToMax` itself.
		unsigned long long CurrentByteUsage() const { return mCurrentBytes; }
		//! The configured budget (constructor's `maxBytes`, 0 = disabled).
		unsigned long long ByteBudget() const { return mByteBudget; }

		//! P1-#3 (transaction atomicity): snapshot/restore the REDO stack across a
		//! transaction.  The first edit in a transaction clears the redo stack
		//! (standard new-edit-invalidates-redo); on a FULL rollback the transaction
		//! never committed, so that redo-clear side effect must be undone too.
		//! BeginTransaction snapshots; a fully-reverted RollbackTransaction restores.
		void SnapshotRedoForRollback() { mTxnRedoSnapshot = mRedoStack; }
		void RestoreRedoFromSnapshot() { mRedoStack = mTxnRedoSnapshot; }

		//! P1: same for the UNDO stack -- a transaction edit can evict the oldest
		//! PRE-transaction undo record at the cap; a full rollback restores it so
		//! the rolled-back gesture leaves NO permanent history side effect.
		void SnapshotUndoForRollback() { mTxnUndoSnapshot = mUndoStack; }
		//! P2-4: a wholesale stack REPLACEMENT (not an incremental push/pop), so
		//! `mCurrentBytes` is re-summed from scratch afterward rather than
		//! adjusted -- see `RecomputeCurrentBytes_`.
		void RestoreUndoFromSnapshot() { mUndoStack = mTxnUndoSnapshot; RecomputeCurrentBytes_(); }

		//! P1 review: free both rollback snapshots when a transaction closes
		//! (commit or rollback).  They are dead the moment the transaction ends;
		//! without this they'd hold deep copies of up to 2x the history until the
		//! next BeginTransaction overwrote them.  swap-with-empty guarantees the
		//! heap is actually released (clear() alone would not).
		void ClearRollbackSnapshots() {
			std::deque<SceneEdit>().swap( mTxnUndoSnapshot );
			std::deque<SceneEdit>().swap( mTxnRedoSnapshot );
		}

		//! Label of the most recent composite (or top edit op name)
		//! for the UI's "Undo <X>" menu item.
		const char* LabelForUndo() const;
		const char* LabelForRedo() const;

	private:
		// String comparator for std::set<String> — String is
		// std::vector<char>-derived and doesn't have operator<.
		struct StringLess
		{
			bool operator()( const String& a, const String& b ) const;
		};

		std::deque<SceneEdit>           mUndoStack;
		std::deque<SceneEdit>           mRedoStack;
		std::deque<SceneEdit>           mTxnRedoSnapshot;   // P1-#3: pre-transaction redo stack
		std::deque<SceneEdit>           mTxnUndoSnapshot;   // P1: pre-transaction undo stack
		std::set<String, StringLess>    mDirtyObjects;
		unsigned int                    mMaxEntries;
		unsigned long long              mNextSeq;       ///< F2 monotonic edit id
		unsigned long long              mMaxTrimmedSeq; ///< F2 highest trimmed seq
		bool                            mDidTrim;       ///< F2 anything trimmed?
		unsigned long long              mByteBudget;    ///< P2-4: constructor's `maxBytes` (0 = disabled)
		unsigned long long              mCurrentBytes;  ///< P2-4: running sum, mUndoStack ONLY (see CurrentByteUsage)

		void TrimToMax();
		void PopFrontTracked();   ///< pop_front + update mMaxTrimmedSeq (F2)

		//! P2-4: the heavyweight document-payload bytes ONE edit contributes to
		//! the byte budget -- see `kDefaultByteBudget`'s doc for exactly which
		//! op/field pairs count.  Every other op (transform deltas, typed
		//! property values, single-chunk CRUD bytes) returns 0: this budget
		//! targets the WHOLE-DOCUMENT-TEXT ops specifically, not the ordinary
		//! per-edit bookkeeping that already fits comfortably within 1024
		//! entries.
		static unsigned long long HeavyPayloadBytes_( const SceneEdit& e );

		//! P2-4: re-sum `mCurrentBytes` from scratch over `mUndoStack` as it
		//! stands NOW.  Used after a wholesale stack replacement
		//! (`RestoreUndoFromSnapshot`), where incremental add/subtract has no
		//! single edit to key off of.
		void RecomputeCurrentBytes_();
	};
}

#endif
