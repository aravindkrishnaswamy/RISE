//////////////////////////////////////////////////////////////////////
//
//  SceneEdit.h - Value-typed mutation record for the interactive
//    scene editor.  Tagged-union of edit operations; trivially
//    copyable so it composes into ring buffers and history stacks
//    without dynamic allocation.
//
//  See docs/INTERACTIVE_EDITOR_PLAN.md for the design rationale.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SCENEEDIT_
#define RISE_SCENEEDIT_

#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/RString.h"

namespace RISE
{
	//! A single editor mutation captured as a value.
	/// The forward op uses the v3a / v3b / s payload fields.  The
	/// "prev*" fields capture the state immediately BEFORE the
	/// mutation so undo is a deterministic restore (no need to
	/// re-derive prior state from a setter delta).
	///
	/// Composite markers (CompositeBegin/End) bracket a user drag
	/// so undo collapses one drag into one history entry.
	struct SceneEdit
	{
		enum Op
		{
			// Object transforms (objectName must be non-empty)
			TranslateObject,        ///< v3a = world delta
			RotateObjectArb,        ///< v3a = unit axis, s = radians
			SetObjectPosition,      ///< v3a = absolute world pos
			SetObjectOrientation,   ///< v3a = euler XYZ
			SetObjectScale,         ///< s   = uniform scale
			SetObjectStretch,       ///< v3a = per-axis stretch
			SetObjectMaterial,      ///< propertyValue = material name; prev captured for undo
			SetObjectShader,        ///< propertyValue = shader name
			SetObjectShadowFlags,   ///< s as int: bit0=castsShadows, bit1=receivesShadows

			// Camera (objectName ignored)
			SetCameraTransform,     ///< v3a = pos, v3b = look-at
			//! Orbit around the look-at by deltas to the camera's
			//! `target_orientation` parameter.  `v3a.x` = phi delta
			//! (azimuth, around world up); `v3a.y` = theta delta
			//! (elevation, around camera right).  `vPosition` and
			//! `vLookAt` are NOT touched — they stay at their "rest"
			//! values; `Recompute` derives the post-orbit position
			//! from `target_orientation`.  This shape keyframes
			//! cleanly (each angle is its own scalar) and round-trips
			//! through `.RISEscene` (the angles are already declared
			//! parameters).
			OrbitCamera,
			PanCamera,              ///< v3a   = world delta
			ZoomCamera,             ///< s     = factor
			//! Roll along the (camera→look-at) axis by `s` radians,
			//! delta applied to `orientation.z`.  `vPosition` and
			//! `vLookAt` are NOT touched.
			RollCamera,

			// Time (objectName ignored)
			SetSceneTime,           ///< s = absolute time

			// Properties-panel edit.  objectName carries the property
			// name; propertyValue / prevPropertyValue carry the new
			// and previous string forms (so undo round-trips through
			// the same parser the panel uses on the way in).
			SetCameraProperty,

			//! Light edit.  objectName = light entity name (the
			//! manager-registered name).  propertyName carries the
			//! property identifier ("position" / "energy" / "color"
			//! / "target" / etc.).  propertyValue carries the new
			//! value, prevPropertyValue captures the prior value for
			//! undo.  Undo route: re-apply prevPropertyValue through
			//! the same KeyframeFromParameters + SetIntermediateValue
			//! + RegenerateData pipeline the forward path uses.
			SetLightProperty,

			// Composite markers — bracket a user drag so undo
			// collapses one drag into one history entry.
			CompositeBegin,         ///< objectName = label for UI
			CompositeEnd
		};

		Op       op;
		String   objectName;
		Vector3  v3a;
		Vector3  v3b;
		Scalar   s;

		//! Properties-panel value carriers (used by SetCameraProperty,
		//! SetLightProperty, SetObjectMaterial, SetObjectShader).
		//! `propertyValue` holds the typed-in new value;
		//! `prevPropertyValue` is captured before the mutation so undo
		//! can replay it through the same parser the forward path
		//! uses.
		String   propertyValue;
		String   prevPropertyValue;

		//! Used by `SetLightProperty` to disambiguate the property
		//! identifier from the entity name (objectName carries the
		//! light name, propertyName carries "position" / "energy" /
		//! etc.).  Camera ops overload `objectName` for the property
		//! name since cameras don't need an entity-name disambiguator
		//! (the active camera is implicit).
		String   propertyName;

		//! State captured BEFORE mutation, used by undo to restore.
		/// For object/camera transform ops this is the full
		/// GetFinalTransformMatrix().  Restore path is
		/// ClearAllTransforms() + PushTopTransStack(prevTransform)
		/// + FinalizeTransformations(), which yields a matrix
		/// equal to prevTransform.
		Matrix4  prevTransform;

		//! Previous scene time (for SetSceneTime undo).
		Scalar   prevTime;

		//! Previous camera state (for camera-op undo).  Captured by
		//! SceneEditor::Apply before the camera mutation runs.
		Point3   prevCameraPos;
		Point3   prevCameraLookAt;
		Vector3  prevCameraUp;
		//! Previous angular state — captured for OrbitCamera (uses
		//! prevCameraTargetOrient) and RollCamera (uses
		//! prevCameraOrient).  PanCamera / ZoomCamera don't touch
		//! these but capturing both for every camera op keeps Apply
		//! uniform; they're cheap.
		Vector2  prevCameraTargetOrient;
		Vector3  prevCameraOrient;

		//! Captured for SetObjectShadowFlags: prior cast/receive bits
		//! packed the same way as `s` (cast in bit0, receive in bit1)
		//! so undo restores both halves atomically.  Stored as Scalar
		//! to fit the trivially-copyable struct shape.
		Scalar   prevShadowFlags;

		SceneEdit()
		: op( CompositeBegin )
		, objectName()
		, v3a()
		, v3b()
		, s( 0 )
		, prevTransform( Matrix4Ops::Identity() )
		, prevTime( 0 )
		, prevCameraPos()
		, prevCameraLookAt()
		, prevCameraUp()
		, prevCameraTargetOrient()
		, prevCameraOrient()
		, prevShadowFlags( 0 )
		{}

		//! Returns true if this edit op refers to a named object.
		static bool IsObjectOp( Op op )
		{
			return op == TranslateObject
			    || op == RotateObjectArb
			    || op == SetObjectPosition
			    || op == SetObjectOrientation
			    || op == SetObjectScale
			    || op == SetObjectStretch
			    || op == SetObjectMaterial
			    || op == SetObjectShader
			    || op == SetObjectShadowFlags;
		}

		//! Returns true if this edit op mutates the camera.
		static bool IsCameraOp( Op op )
		{
			return op == SetCameraTransform
			    || op == OrbitCamera
			    || op == PanCamera
			    || op == ZoomCamera
			    || op == RollCamera;
		}

		//! Returns true if this is a composite bracket (no mutation).
		static bool IsCompositeMarker( Op op )
		{
			return op == CompositeBegin || op == CompositeEnd;
		}
	};
}

#endif
