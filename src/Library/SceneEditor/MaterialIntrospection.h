//////////////////////////////////////////////////////////////////////
//
//  MaterialIntrospection.h - Read-only introspection of an IMaterial
//    for the interactive editor's properties panel.
//
//  Phase-2 scope (this PR):
//    - Surface the material's manager-registered name + concrete
//      type (e.g. "Lambertian", "GGX", "PerfectReflector") in the
//      Materials accordion's property panel.
//    - Lambertian additionally surfaces its `reflectance` painter
//      binding (reverse-looked-up via the IPainterManager) — also
//      read-only.
//    - Every row's `editable` flag is false.  Editing requires
//      per-material setter API (or material-rebuild-on-edit) that
//      doesn't exist on IBSDF / ISPF today — those store their
//      painter as `const IPainter&` (a reference, not a rebindable
//      pointer).  Adding the setter surface across ~20 material
//      classes is the documented Phase 4 follow-up; the panel
//      already shows the bindings so the user has a discoverable
//      starting point.
//
//  Future work (Phase 4):
//    - Add `SetReflectance(IPainter&)` / `SetSpecular(IPainter&)`
//      / ... on each BRDF / SPF, rebinding via stored pointer.
//    - Add a SceneEdit op `SetMaterialPainterSlot` that swaps the
//      painter by name + walks downstream consumers if needed.
//    - Extend this introspection layer to mark those rows editable
//      and supply preset lists from IPainterManager.
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_MATERIALINTROSPECTION_
#define RISE_MATERIALINTROSPECTION_

#include "../Interfaces/IMaterial.h"
#include "../Utilities/RString.h"
#include "CameraIntrospection.h"   // CameraProperty (panel-row struct)
#include <vector>

namespace RISE
{
	class IPainterManager;
	class IScalarPainterManager;
	class IPainter;
	class IScalarPainter;

	//! Tagged-union for a painter slot's binding.  `kind` discriminates
	//! between the two painter pipes (per CLAUDE.md's IScalarPainter
	//! refactor — IPainter is the colour pipe, IScalarPainter is the
	//! physical-scalar pipe; the two are NOT interchangeable).  Used
	//! by `GetSlot` / `SetSlot` to return-or-accept whichever pipe a
	//! material's named slot expects.
	struct MaterialSlotRef
	{
		enum Kind { None = 0, Painter = 1, ScalarPainter = 2 };
		Kind                    kind;
		const IPainter*         painter;        // valid iff kind == Painter
		const IScalarPainter*   scalarPainter;  // valid iff kind == ScalarPainter
		MaterialSlotRef() : kind( None ), painter( 0 ), scalarPainter( 0 ) {}
	};

	class MaterialIntrospection
	{
	public:
		//! Inspect a single material.  `name` is the manager-registered
		//! name.  Optional `painters` / `scalarPainters` let the
		//! inspector reverse-lookup a slot's name from its pointer —
		//! pass null to omit painter-binding rows.  Optional `job`
		//! lets `Inspect` consult `IsMaterialComposed` so composed
		//! materials' slot rows are surfaced read-only with an
		//! explanatory note.
		static std::vector<CameraProperty> Inspect(
			const String& name,
			const IMaterial& material,
			const IPainterManager* painters = 0,
			const IScalarPainterManager* scalarPainters = 0,
			const class IJob* job = 0 );

		//! Concrete-type discriminator — returns "Lambertian",
		//! "Phong", "GGX", etc., or "(unknown type)" for an out-of-
		//! tree IMaterial subclass.  Used by `Inspect` to drive the
		//! "Type" header row + per-type field surfacing.
		static String GetTypeName( const IMaterial& material );

		//! Read the current binding of a named slot on a material.
		//! Returns Kind::None for unknown slot names or non-painter
		//! slots (e.g. CompositeMaterial's `thickness` scalar field
		//! — those won't be addressed via this API; a future Phase 5
		//! will add a scalar-property setter for them).
		static MaterialSlotRef GetSlot(
			const IMaterial& material,
			const String& slotName );

		//! Rebind a named painter slot on a material.  Dispatches by
		//! the material's concrete type and calls the matching
		//! per-material `Set*` setter (Lambertian::SetReflectance,
		//! GGXMaterial::SetDiffuse, etc.).  Returns false on:
		//!   - unknown slot name for this material type,
		//!   - wrong-pipe binding (e.g. trying to bind an IPainter
		//!     into an IScalarPainter slot),
		//!   - material is from a composing factory (PBR-MR / GGX-E)
		//!     — caller should consult `IJob::IsMaterialComposed`
		//!     BEFORE calling `SetSlot` and reject up-front for a
		//!     clean user-facing error message.
		//! Caller is responsible for prev-state capture (via `GetSlot`
		//! + reverse-lookup) and for the cancel-and-park gate.
		static bool SetSlot(
			IMaterial& material,
			const String& slotName,
			const IPainter* painter,           // null if scalarPainter is set
			const IScalarPainter* scalarPainter );
	};
}

#endif
