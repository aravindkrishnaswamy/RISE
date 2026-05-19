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
	//! Value-typed snapshot of an ICamera's stored state.  Used by
	//! `SceneEdit::AddCamera` so Redo can re-create the cloned
	//! camera deterministically even if the source camera's state
	//! changed between the original Apply and the Redo.
	//!
	//! `type` discriminator picks which subset of fields the Add*
	//! factory consumes — the unused fields carry zeros and are
	//! harmless if read.  Kept trivially-copyable so it composes
	//! into the SceneEdit value-record.
	struct CameraSnapshot
	{
		enum Type { Pinhole = 0, ThinLens = 1, Fisheye = 2, Orthographic = 3 };

		int          type;

		// Shared across all camera kinds.
		double       location[3];
		double       lookat[3];
		double       up[3];
		double       exposure;
		double       scanningRate;
		double       pixelRate;
		double       orientation[3];
		double       target_orientation[2];
		//! Source camera's pixelAR.  The Add*Camera factories read
		//! pixelAR from the scene's active Film (last-Set wins), but
		//! `SetPixelAR` lets a camera diverge from the Film value.
		//! Captured here so the clone matches the source even after
		//! the Film's pixelAR has moved.
		double       pixelAR;

		// Pinhole / ThinLens.
		double       iso;
		double       fov;             // Pinhole only

		// ThinLens.
		double       sensorSize;
		double       focalLength;
		double       fstop;
		double       focusDistance;
		double       sceneUnitMeters;
		unsigned int apertureBlades;
		double       apertureRotation;
		double       anamorphicSqueeze;
		double       tiltX;
		double       tiltY;
		double       shiftX;
		double       shiftY;

		// Fisheye.
		double       fisheyeScale;

		// Orthographic.
		double       viewportScale[2];

		CameraSnapshot()
		: type( Pinhole )
		, exposure( 0 ), scanningRate( 0 ), pixelRate( 0 )
		, pixelAR( 1.0 )
		, iso( 0 ), fov( 0 )
		, sensorSize( 0 ), focalLength( 0 ), fstop( 0 ), focusDistance( 0 )
		, sceneUnitMeters( 1 )
		, apertureBlades( 0 ), apertureRotation( 0 ), anamorphicSqueeze( 1 )
		, tiltX( 0 ), tiltY( 0 ), shiftX( 0 ), shiftY( 0 )
		, fisheyeScale( 1 )
		{
			location[0] = location[1] = location[2] = 0;
			lookat[0]   = lookat[1]   = lookat[2]   = 0;
			up[0]       = 0; up[1] = 1; up[2] = 0;
			orientation[0] = orientation[1] = orientation[2] = 0;
			target_orientation[0] = target_orientation[1] = 0;
			viewportScale[0] = viewportScale[1] = 1;
		}
	};

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
			//! Anchor-based scale used by the gizmo drag.  Carries the
			//! per-axis FACTOR in `v3a` and the object's final transform
			//! matrix at drag-start in `prevTransform` — Apply restores
			//! the drag-start matrix, then pushes a stretch-by-factor on
			//! top so the result is `prevTransform · Stretch(v3a)`.
			//! Unlike `SetObjectStretch` (which overwrites just the
			//! stretch component of the transform stack), this op
			//! composes the factor with whatever drag-start state the
			//! object actually had — including any baked-in scale from
			//! `AddObjectMatrix` (glTF / quaternion / imported objects)
			//! or earlier `SetObjectScale` calls.  Without it, a tiny
			//! scale drag on a 10×-imported glTF reads its column
			//! magnitudes back as a fresh stretch of 10× and then
			//! ALSO leaves the original 10× matrix on the stack →
			//! object jumps toward 100×.  Unlike the other transform
			//! ops, the controller pre-populates `prevTransform` at
			//! drag-start and Apply does NOT overwrite it (every
			//! per-frame Apply needs the SAME anchor, not the
			//! previous frame's already-scaled state).
			ScaleObjectFromAnchor,
			SetObjectMaterial,      ///< propertyValue = material name; prev captured for undo
			SetObjectShader,        ///< propertyValue = shader name
			SetObjectShadowFlags,   ///< s as int: bit0=castsShadows, bit1=receivesShadows
			//! Swap (or clear) an object's interior participating medium
			//! by name.  Non-empty propertyValue is a medium name that
			//! must resolve through IJob::GetMedium; empty propertyValue
			//! routes through IObjectPriv::ClearInteriorMedium for
			//! parser-parity with `interior_medium "none"` (which the
			//! load-time parser treats as "leave the interior empty").
			//! prevPropertyValue captures the old medium name via
			//! reverse-lookup against IJob::EnumerateMediumNames so
			//! undo round-trips losslessly — empty prev means "no
			//! medium was bound" and the undo path calls
			//! ClearInteriorMedium accordingly.  Object reference IS
			//! resolved (FindObject), but the invariant chain is NOT
			//! run — interior_medium swap is a pointer change that
			//! doesn't invalidate spatial structure.
			SetObjectInteriorMedium,

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

			//! Register a new camera by cloning the state of an
			//! existing one.  `objectName` carries the NEW camera's
			//! name (the name the user picked or the auto-generated
			//! dedup suffix).  `prevPropertyValue` captures the name
			//! of the camera that was active immediately before the
			//! Add so undo can restore the prior active-camera
			//! selection.  `cameraSnapshot` carries the full field
			//! set captured from the source camera at Apply time so
			//! Redo can recreate the new camera deterministically
			//! even if the source has since changed.  Forward path
			//! goes through `IJob::Add{Pinhole,Thinlens,Fisheye,
			//! Orthographic}Camera` (which set the new camera active
			//! by the "last added wins" policy).  Undo route:
			//! `RemoveCamera(newName)` + `SetActiveCamera(prev)`.
			AddCamera,

			//! Material edit.  `objectName` carries the material's
			//! manager-registered name.  `propertyName` carries the
			//! slot identifier (matches the chunk parameter name —
			//! "reflectance", "ior", "alphax", "diffuse", etc.).
			//! `propertyValue` carries the painter name (IPainter or
			//! IScalarPainter depending on slot type — the per-
			//! material dispatcher in `SceneEditor::Apply` knows
			//! which manager to consult).  `prevPropertyValue` is
			//! the previously-bound painter name, captured via the
			//! material's Get accessor + reverse-lookup through the
			//! matching manager.
			//!
			//! Forward path: routes through per-material `SetXxx`
			//! virtual setters (added across all 25 material types
			//! in Phase 4) which release the prior painter, addref
			//! the new one, and bind it on the BRDF / SPF / Material.
			//! No spatial-structure invalidation — material edits are
			//! pointer swaps the render thread reads coherently
			//! between passes (with cancel-and-park at the
			//! controller).  Composed materials (PBRMetallicRoughness,
			//! GGXEmissive — composed via painter graph) are rejected
			//! up-front because rebinding a slot would break the
			//! graph; the rejection comes via `IJob::IsMaterialComposed`.
			SetMaterialProperty,

			//! Edit a property on a medium.  objectName carries the
			//! medium's manager-registered name, propertyName the slot
			//! identifier ("absorption" / "scattering" / "emission"
			//! per the parser), propertyValue a `"r g b"` triple
			//! parsed via ParseStrictVec3.  Only HomogeneousMedium
			//! accepts edits today — Heterogeneous rejects because its
			//! majorant grid was baked at construction.
			//!
			//! Caller is responsible for the cancel-and-park gate
			//! around Apply — Set* on the medium re-derives sigma_t
			//! and sigma_t_max which are read by distance-sampling
			//! workers; the swap is racy without the park.
			SetMediumProperty,

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

		//! Used by AddCamera: full source-camera state at Apply time.
		//! Embedded directly (rather than via shared_ptr) so SceneEdit
		//! stays trivially-copyable.
		CameraSnapshot cameraSnapshot;

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
			    || op == ScaleObjectFromAnchor
			    || op == SetObjectMaterial
			    || op == SetObjectShader
			    || op == SetObjectShadowFlags
			    || op == SetObjectInteriorMedium;
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
