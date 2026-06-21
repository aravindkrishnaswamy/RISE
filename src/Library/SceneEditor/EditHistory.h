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
		EditHistory( unsigned int maxEntries = 1024 );
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

		//! For round-trip save: has this object's transform been
		//! touched at least once since the scene was loaded?  This
		//! includes edits that have been undone — the serializer
		//! re-emits the live transform regardless, and undone edits
		//! still represent "user touched it."
		bool IsObjectDirty( const String& name ) const;

		//! Iterate dirty objects.  Yields each name once.
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
		void RestoreUndoFromSnapshot() { mUndoStack = mTxnUndoSnapshot; }

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

		void TrimToMax();
		void PopFrontTracked();   ///< pop_front + update mMaxTrimmedSeq (F2)
	};
}

#endif
