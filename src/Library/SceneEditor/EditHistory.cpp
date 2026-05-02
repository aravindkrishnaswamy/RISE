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
{
}

EditHistory::~EditHistory()
{
}

void EditHistory::Push( const SceneEdit& edit )
{
	mUndoStack.push_back( edit );
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

void EditHistory::Clear()
{
	mUndoStack.clear();
	mRedoStack.clear();
	mDirtyObjects.clear();
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
		case SceneEdit::SetObjectMaterial:      return "Set Material";
		case SceneEdit::SetObjectShader:        return "Set Shader";
		case SceneEdit::SetObjectShadowFlags:   return "Set Shadow Flags";
		case SceneEdit::SetCameraTransform:     return "Set Camera";
		case SceneEdit::OrbitCamera:            return "Orbit Camera";
		case SceneEdit::PanCamera:              return "Pan Camera";
		case SceneEdit::ZoomCamera:             return "Zoom Camera";
		case SceneEdit::RollCamera:             return "Roll Camera";
		case SceneEdit::SetSceneTime:           return "Set Time";
		case SceneEdit::SetCameraProperty:      return "Edit Property";
		case SceneEdit::SetLightProperty:       return "Edit Light Property";
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
		for( std::deque<SceneEdit>::const_reverse_iterator it = mUndoStack.rbegin();
		     it != mUndoStack.rend(); ++it )
		{
			if( it->op == SceneEdit::CompositeBegin && it->objectName.size() > 1 )
			{
				return it->objectName.c_str();
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

void EditHistory::TrimToMax()
{
	while( mUndoStack.size() > mMaxEntries )
	{
		// Atomic-by-composite trim: if the head is a CompositeBegin,
		// pop the whole composite (every entry up to and including
		// the matching End) so we never leave orphaned markers that
		// would corrupt subsequent Undo.  If the head is a stray
		// CompositeEnd (shouldn't happen if EndComposite's depth-0
		// guard works, but be defensive), drop it on its own.
		if( mUndoStack.front().op == SceneEdit::CompositeBegin )
		{
			mUndoStack.pop_front();
			while( !mUndoStack.empty()
			    && mUndoStack.front().op != SceneEdit::CompositeEnd )
			{
				mUndoStack.pop_front();
			}
			if( !mUndoStack.empty()
			 && mUndoStack.front().op == SceneEdit::CompositeEnd )
			{
				mUndoStack.pop_front();
			}
		}
		else
		{
			mUndoStack.pop_front();
		}
	}
}
