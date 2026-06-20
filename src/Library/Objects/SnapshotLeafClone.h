//////////////////////////////////////////////////////////////////////
//
//  SnapshotLeafClone.h - Snapshot-time clone of a material "leaf".
//
//  feature/gui-snapshot-prototype, increment A (immutability
//  correctness).  Backs Object::CloneSnapshot / CSGObject::CloneSnapshot.
//
//  WHY THIS EXISTS
//  ---------------
//  The original snapshot design addref-SHARED an object's material with
//  the live scene, calling it an "immutable leaf".  It is not immutable:
//  the interactive editor rebinds a material's painter slots IN PLACE via
//  MaterialIntrospection::SetSlot (SceneEditor::SetMaterialProperty, the
//  Phase-B Material SetProperty path) — e.g. Lambertian::SetReflectance
//  rebinds the BRDF + SPF reflectance pointer.  A snapshot that shares the
//  instance would therefore observe live edits, defeating immutability.
//
//  This helper produces an INDEPENDENT material instance whose painter
//  slots are copies of the live bindings at snapshot time, so a later
//  in-place rebind on the live material does NOT bleed into the snapshot.
//
//  ABI NOTE (docs/skills/abi-preserving-api-evolution.md, Layer 2):
//  IMaterial is an abstract interface; adding a `Clone()` virtual to it —
//  even appended at the vtable end — breaks new-caller -> old-out-of-tree-
//  implementation.  So this is a FREE function that dispatches on the
//  concrete material type via dynamic_cast, never a new interface virtual.
//
//  PAINTER / TEXTURE SUB-LEAVES ARE ADDREF-SHARED, NOT CLONED.
//  The painters (IPainter / IScalarPainter) a material's slots point at
//  are NOT property-edited today (the editor rebinds WHICH painter a slot
//  uses; it does not mutate a painter's internal pixels/values in place),
//  so sharing them by reference is correct and cheap.  If a future
//  increment adds in-place painter mutation, those sub-leaves would need
//  cloning too.
//
//  SCOPE / FIDELITY (honest):
//  Faithful ctor-reconstruction is implemented for the 15 standard
//  reflectance/BRDF materials whose entire construction state is readable
//  from public accessors (Lambertian, PerfectReflector, PerfectRefractor,
//  Polished, Dielectric, IsotropicPhong, OrenNayar, Schlick, Sheen,
//  AshikminShirley, CookTorrance, GGX [non-emissive], WardIsotropic,
//  WardAnisotropic, Translucent).  These are the common editable materials
//  (incl. the test-pinned Lambertian) and carry NO residual.
//
//  A handful of materials bake construction-time state that cannot be read
//  back from their public surface — a sampled diffusion profile, a
//  random-walk parameter snapshot, a tissue chromophore mix, or a wrapped
//  base material: SubSurfaceScattering, RandomWalkSSS, GenericHumanTissue,
//  Lambertian/Phong luminaires.  CloneMaterialForSnapshot falls back to
//  ADDREF for them (and for emissive GGX / NullMaterial / any unknown
//  out-of-tree type).
//
//  HONEST RESIDUAL (deferred to increment B): those baked/wrapping
//  materials DO expose editor-mutable slots (SSS/RandomWalkSSS `ior`,
//  tissue `sca`/`g`, luminaire `exitance`/`N` via SetSlot), so an in-place
//  rebind of one of those specific slots would still bleed into a snapshot
//  that addref-shared them.  A correct independent clone of these needs
//  per-class clone methods reaching private state; that is intentionally
//  out of scope for increment A.
//
//////////////////////////////////////////////////////////////////////

#ifndef SNAPSHOT_LEAF_CLONE_
#define SNAPSHOT_LEAF_CLONE_

namespace RISE
{
	class IMaterial;
	class ILight;
	class IMedium;
	class ICamera;

	namespace Implementation
	{
		//! Returns an independent snapshot clone of `mat` (caller owns one
		//! reference and must release()).  For the faithfully-clonable
		//! material types this is a fresh instance with copied painter-slot
		//! bindings (sub-painters addref-shared).  For materials whose
		//! construction state is not publicly readable (and which the editor
		//! never mutates in place), or for an unknown type, it addrefs and
		//! returns `mat` itself.  Returns 0 iff `mat` is 0.
		const IMaterial* CloneMaterialForSnapshot( const IMaterial* mat );

		//! Returns an independent snapshot clone of `light` (caller owns one
		//! reference and must release()).  Lights are edited IN PLACE by the
		//! interactive editor (SceneEditor::SetLightProperty rebinds
		//! color / energy / direction / cone via SetIntermediateValue), so
		//! addref-sharing them would bleed live edits into the snapshot.
		//!
		//! The four built-in light types (Point / Spot / Directional /
		//! Ambient) are reconstructed from their public emission accessors
		//! via the RISE_API_Create*Light factories; the transform-derived
		//! world position (point / spot) is restored by SetPosition() +
		//! FinalizeTransformations() so the clone is render-faithful.  An
		//! unknown out-of-tree ILight type falls back to ADDREF.  Returns 0
		//! iff `light` is 0.
		//!
		//! Same ABI rationale as CloneMaterialForSnapshot: a free function
		//! dispatching on the concrete type via dynamic_cast, never a new
		//! ILight virtual (which would break out-of-tree implementors).
		const ILight* CloneLightForSnapshot( const ILight* light );

		//! Returns an independent snapshot clone of `medium` (caller owns
		//! one reference and must release()).  HomogeneousMedium is edited
		//! IN PLACE by the editor (MediaIntrospection SetAbsorption /
		//! SetScattering / SetEmission), so it is reconstructed from its
		//! public coefficient accessors (the phase function is addref-shared
		//! — it is not property-edited in place).  HeterogeneousMedium bakes
		//! its dataset / bounds at construction and the editor refuses to
		//! edit it (MediaIntrospection), so it is ADDREF-shared, matching the
		//! baked-material residual policy of CloneMaterialForSnapshot.  An
		//! unknown type also falls back to ADDREF.  Returns 0 iff 0.
		const IMedium* CloneMediumForSnapshot( const IMedium* medium );

		//! Returns an independent, render-faithful snapshot clone of the
		//! active `camera` (caller owns one reference and must release()).
		//! The editor edits camera params IN PLACE (CameraIntrospection
		//! SetProperty -> the per-type setters + SetIntermediateValue), so a
		//! pose-only capture is not faithful; this rebuilds the concrete
		//! camera (pinhole / thinlens / orthographic / fisheye) through its
		//! RISE_API factory copying ALL parameters.
		//!
		//! ONB-CONSTRUCTED CAMERAS: returns 0.  An ONB camera
		//! (CameraCommon::IsFromONB) bypasses the lookAt/up + orientation
		//! math the non-ONB factories use; rebuilding it through them would
		//! silently degrade its basis.  Per the CameraCommon.h warning we
		//! REFUSE rather than lose params — the caller falls back to the
		//! pose summary.  Also returns 0 for an unknown out-of-tree camera
		//! type (no factory to rebuild it) and iff `camera` is 0.
		ICamera* CloneCameraForSnapshot( const ICamera* camera );
	}
}

#endif
