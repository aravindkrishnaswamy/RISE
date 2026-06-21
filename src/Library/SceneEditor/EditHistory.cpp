//////////////////////////////////////////////////////////////////////
//
//  EditHistory.cpp
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "EditHistory.h"
#include <cstring>

using namespace RISE;

bool EditHistory::StringLess::operator()( const String& a, const String& b ) const
{
	const char* sa = a.c_str();
	const char* sb = b.c_str();
	if( !sa ) sa = "";
	if( !sb ) sb = "";
	return std::strcmp( sa, sb ) < 0;
}

EditHistory::EditHistory( unsigned int maxEntries )
: mMaxEntries( maxEntries )
, mNextSeq( 0 )
, mMaxTrimmedSeq( 0 )
, mDidTrim( false )
{
}

EditHistory::~EditHistory()
{
}

void EditHistory::Push( const SceneEdit& edit )
{
	SceneEdit stamped = edit;
	stamped.historySeq = mNextSeq++;   // F2: monotonic, trim-immune id
	mUndoStack.push_back( stamped );
	mRedoStack.clear();

	// Track dirty objects for write-back.  Composite markers and
	// camera/time ops contribute no name.
	if( SceneEdit::IsObjectOp( edit.op ) && edit.objectName.size() > 1 )
	{
		mDirtyObjects.insert( edit.objectName );
	}

	TrimToMax();
}

bool EditHistory::PopForUndo( SceneEdit& outEdit )
{
	if( mUndoStack.empty() ) return false;
	outEdit = mUndoStack.back();
	mUndoStack.pop_back();
	mRedoStack.push_back( outEdit );
	return true;
}

bool EditHistory::PopForRedo( SceneEdit& outEdit )
{
	if( mRedoStack.empty() ) return false;
	outEdit = mRedoStack.back();
	mRedoStack.pop_back();
	mUndoStack.push_back( outEdit );
	return true;
}

void EditHistory::ClearRedo()
{
	mRedoStack.clear();
}

void EditHistory::Clear()
{
	mUndoStack.clear();
	mRedoStack.clear();
	mDirtyObjects.clear();
	mMaxTrimmedSeq = 0;   // mNextSeq stays monotonic (no seq reuse)
	mDidTrim = false;
}

bool EditHistory::IsObjectDirty( const String& name ) const
{
	return mDirtyObjects.find( name ) != mDirtyObjects.end();
}

unsigned int EditHistory::UndoDepth() const
{
	return static_cast<unsigned int>( mUndoStack.size() );
}

unsigned int EditHistory::RedoDepth() const
{
	return static_cast<unsigned int>( mRedoStack.size() );
}

namespace
{
	const char* OpName( SceneEdit::Op op )
	{
		switch( op )
		{
		case SceneEdit::TranslateObject:        return "Translate";
		case SceneEdit::RotateObjectArb:        return "Rotate";
		case SceneEdit::SetObjectPosition:      return "Set Position";
		case SceneEdit::SetObjectOrientation:   return "Set Orientation";
		case SceneEdit::SetObjectScale:         return "Set Scale";
		case SceneEdit::SetObjectStretch:       return "Set Stretch";
		case SceneEdit::ScaleObjectFromAnchor:  return "Scale";
		case SceneEdit::SetObjectMaterial:      return "Set Material";
		case SceneEdit::SetObjectGeometry:      return "Set Geometry";
		case SceneEdit::SetObjectShader:        return "Set Shader";
		case SceneEdit::SetObjectShadowFlags:   return "Set Shadow Flags";
		case SceneEdit::SetObjectInteriorMedium:return "Set Interior Medium";
		case SceneEdit::SetCameraTransform:     return "Set Camera";
		case SceneEdit::OrbitCamera:            return "Orbit Camera";
		case SceneEdit::PanCamera:              return "Pan Camera";
		case SceneEdit::ZoomCamera:             return "Zoom Camera";
		case SceneEdit::RollCamera:             return "Roll Camera";
		case SceneEdit::SetSceneTime:           return "Set Time";
		case SceneEdit::SetCameraProperty:      return "Edit Property";
		case SceneEdit::SetLightProperty:       return "Edit Light Property";
		case SceneEdit::AddCamera:              return "Add Camera";
		case SceneEdit::SetMaterialProperty:    return "Edit Material Property";
		case SceneEdit::SetMediumProperty:      return "Edit Medium Property";
		case SceneEdit::CompositeBegin:         return "Edit";
		case SceneEdit::CompositeEnd:           return "Edit";
		}
		return "Edit";
	}
}

const char* EditHistory::LabelForUndo() const
{
	if( mUndoStack.empty() ) return "";
	const SceneEdit& top = mUndoStack.back();
	// If the top is a CompositeEnd, surface the most recent
	// CompositeBegin's label (which lives in objectName).
	if( top.op == SceneEdit::CompositeEnd )
	{
		// P1: nesting-aware -- return the OUTER composite's label (the Begin that
		// matches the top End at depth 0), NOT the first (inner) Begin.  Undo reverts
		// the whole outer group, so the tooltip must name that group.
		int depth = 0;
		for( std::deque<SceneEdit>::const_reverse_iterator it = mUndoStack.rbegin();
		     it != mUndoStack.rend(); ++it )
		{
			if( it->op == SceneEdit::CompositeEnd )        { ++depth; }
			else if( it->op == SceneEdit::CompositeBegin )
			{
				if( --depth == 0 )
					return it->objectName.size() > 1 ? it->objectName.c_str() : "Edit";
			}
		}
	}
	return OpName( top.op );
}

const char* EditHistory::LabelForRedo() const
{
	if( mRedoStack.empty() ) return "";
	const SceneEdit& top = mRedoStack.back();
	if( top.op == SceneEdit::CompositeBegin && top.objectName.size() > 1 )
	{
		return top.objectName.c_str();
	}
	return OpName( top.op );
}

bool EditHistory::PeekUndoSeq( unsigned long long& outSeq ) const
{
	if( mUndoStack.empty() ) return false;
	outSeq = mUndoStack.back().historySeq;
	return true;
}

void EditHistory::PopFrontTracked()
{
	// F2: remember the highest seq we drop, so a rollback can tell when
	// a still-open transaction's edit was trimmed off the front.
	if( !mUndoStack.empty() ) {
		mDidTrim = true;
		if( mUndoStack.front().historySeq > mMaxTrimmedSeq )
			mMaxTrimmedSeq = mUndoStack.front().historySeq;
		mUndoStack.pop_front();
	}
}

void EditHistory::RestoreLastUndoFromRedo()
{
	// P1: the inverse of one PopForUndo -- move the most recently popped edit
	// back from the redo stack to the undo stack.  Called only when the revert
	// FAILED, so a failed undo neither changes the depth nor leaves the
	// un-reverted edit on the redo stack.
	if( mRedoStack.empty() ) return;
	mUndoStack.push_back( mRedoStack.back() );
	mRedoStack.pop_back();
}

void EditHistory::RestoreLastRedoFromUndo()
{
	// P1: the inverse of one PopForRedo -- move the most recently popped edit
	// back from the undo stack to the redo stack.  Called only when the forward
	// mutation FAILED (a vanished redo target), so a failed redo neither changes
	// the depth nor leaves a phantom no-op edit on the undo stack.
	if( mUndoStack.empty() ) return;
	mRedoStack.push_back( mUndoStack.back() );
	mUndoStack.pop_back();
}

void EditHistory::TrimToMax()
{
	while( mUndoStack.size() > mMaxEntries )
	{
		if( mUndoStack.front().op == SceneEdit::CompositeBegin )
		{
			// Find the FRONT begin's matching End by tracking nesting depth
			// (P1: nesting-aware).  A bare "first End" scan would match an INNER
			// composite's End and trim the outer begin + inner history, leaving
			// the outer group malformed.
			int    depth    = 0;
			bool   closed   = false;
			size_t matchIdx = 0;
			for( size_t i = 0; i < mUndoStack.size(); ++i ) {
				const SceneEdit::Op op = mUndoStack[i].op;
				if( op == SceneEdit::CompositeBegin )      { ++depth; }
				else if( op == SceneEdit::CompositeEnd )    { if( --depth == 0 ) { closed = true; matchIdx = i; break; } }
			}
			// P1-#4: an OPEN composite (no matching End yet -- a gesture still in
			// progress) must NOT be trimmed; draining it would orphan the eventual
			// End.  Leave the stack over-cap until the gesture closes.
			if( !closed ) break;
			// P1: never trim the LAST/only undo unit, even if it alone exceeds the
			// cap.  If the group spans to the end of the stack it is the MOST-RECENT
			// action -- retain it over-cap rather than drop the entire gesture
			// (a ~17s 60Hz drag would otherwise lose ALL undoability).
			if( matchIdx == mUndoStack.size() - 1 ) break;
			// Closed group with newer content after it: trim the whole group
			// (Begin .. matching End, inclusive -- including any nested composites)
			// atomically.
			for( size_t k = 0; k <= matchIdx; ++k ) {
				PopFrontTracked();
			}
		}
		else
		{
			PopFrontTracked();
		}
	}
}
