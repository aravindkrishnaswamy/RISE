//////////////////////////////////////////////////////////////////////
//
//  SceneEditor.h - The only sanctioned mutator of a loaded scene.
//    Owns three responsibilities:
//      1. Enforces the post-mutation invariant chain (FinalizeTrans-
//         formations -> ResetRuntimeData -> InvalidateSpatialStruct-
//         ure) on every transform edit so platform code can never
//         forget a step.
//      2. Records every mutation as a value-typed SceneEdit into an
//         EditHistory for undo/redo.
//      3. Tags a "dirty scope" enum that the render orchestrator
//         (SceneEditController) consults to decide between cheap
//         re-render and expensive photon regen.
//
//  This is the SceneEditor + SceneEdit value-record design from
//  docs/INTERACTIVE_EDITOR_PLAN.md §4.3.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SCENEEDITOR_
#define RISE_SCENEEDITOR_

#include "SceneEdit.h"
#include "EditHistory.h"
#include "../Interfaces/IScenePriv.h"

namespace RISE
{
	class IObjectPriv;

	class SceneEditor
	{
	public:
		//! Coarse "what kind of change happened" enum.  The render
		//! orchestrator consults LastDirtyScope() to decide how much
		//! work the next render needs to redo.
		enum DirtyScope
		{
			Dirty_None,
			Dirty_ObjectTransform,   ///< spatial structure invalidated
			Dirty_Camera,            ///< no spatial invalidation
			Dirty_Time,              ///< photons MAY need regen on production
			Dirty_TimeAndPhotons     ///< photons definitely need regen
		};

		SceneEditor( IScenePriv& scene );
		~SceneEditor();

		//! Apply an edit.  On success, runs the invariant chain,
		//! pushes the edit (with its prevTransform/prevTime captured
		//! before the mutation) onto the history, and updates
		//! LastDirtyScope().  Returns false if the edit references
		//! an unknown object or is otherwise invalid; the scene is
		//! not modified in that case.
		bool Apply( const SceneEdit& edit );

		//! Pop the most recent edit and apply its inverse.  Returns
		//! false if the undo stack is empty or the edit's target
		//! object has gone away.
		bool Undo();

		//! Re-apply an edit that was previously undone.
		bool Redo();

		//! Open a composite group.  Subsequent Apply() calls between
		//! BeginComposite/EndComposite collapse to one undo entry.
		void BeginComposite( const char* label );
		void EndComposite();

		DirtyScope LastDirtyScope() const { return mLastScope; }

		//! True if the scene had at least one populated photon map
		//! at construction.  The controller uses this to decide
		//! whether SetSceneTime needs to run before a production
		//! render after a scrub.
		bool ScenePhotonsExist() const { return mScenePhotonsExist; }

		//! Most recently applied scene time via SetSceneTime ops.
		//! Defaults to 0 (the implicit scene time before any scrub).
		//! The controller reads this so it can run a full
		//! `IScene::SetSceneTime(t)` (with photon regen) before a
		//! production render — the preview path uses
		//! SetSceneTimeForPreview which deliberately skips photon
		//! regen and would leave caustics from the pre-scrub time.
		Scalar LastSceneTime() const { return mLastSetTime; }

		//! Characteristic length of the scene — the diagonal of the
		//! axis-aligned union of all object bounding boxes (or a
		//! sensible fallback if the scene is empty / unbounded).
		//! Used by the camera-control math to scale pan / zoom rates
		//! by overall scene size, so a small scene gets small absolute
		//! changes per pixel and a large scene gets large ones,
		//! independent of where the camera is positioned.  Lazily
		//! computed on first use; cached thereafter.
		Scalar SceneScale() const;

		//! For round-trip save (Phase 6 / Phase A).
		const EditHistory& History() const { return mHistory; }
		EditHistory&       History()       { return mHistory; }

	private:
		IScenePriv&  mScene;
		EditHistory  mHistory;
		DirtyScope   mLastScope;
		int          mCompositeDepth;
		bool         mScenePhotonsExist;
		// IScene exposes SetSceneTime but no GetSceneTime, so we
		// track the most-recently-applied time locally.  Used to
		// capture prevTime for undo on SetSceneTime ops.
		Scalar       mLastSetTime;
		// Cached scene scale (bbox-union diagonal).  0 = uncomputed;
		// `SceneScale()` lazy-computes on first call.  `mutable`
		// because `SceneScale` is `const`-qualified so camera-op
		// code can call it from forward / inverse paths.
		mutable Scalar mSceneScale;

		//! Look up an object by name on the live ObjectManager and
		//! return its IObjectPriv*.  Returns null if the object is
		//! not found or does not implement IObjectPriv (which would
		//! indicate a non-mutable Object subclass — in practice
		//! every concrete Object inherits IObjectPriv).
		IObjectPriv* FindObject( const String& name ) const;

		//! Apply a forward transform op to an object.  Caller has
		//! already captured prevTransform.
		void ApplyObjectOpForward( IObjectPriv& obj, const SceneEdit& edit );

		//! Restore an object's transform from a captured matrix.
		void RestoreObjectTransform( IObjectPriv& obj, const Matrix4& prev );

		//! Run the post-mutation invariant chain on a single object
		//! and on the manager.
		void RunObjectInvariantChain( IObjectPriv& obj );

		//! Compute whether the loaded scene has any populated photon map.
		bool ComputeScenePhotonsExist() const;
	};
}

#endif
