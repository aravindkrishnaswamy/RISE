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

		unsigned int UndoDepth() const;
		unsigned int RedoDepth() const;

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
		std::set<String, StringLess>    mDirtyObjects;
		unsigned int                    mMaxEntries;

		void TrimToMax();
	};
}

#endif
