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

	class MaterialIntrospection
	{
	public:
		//! Inspect a single material.  `name` is the manager-registered
		//! name.  Optional `painters` lets the inspector reverse-lookup
		//! a painter slot's name from its pointer — pass null to omit
		//! painter-binding rows.
		static std::vector<CameraProperty> Inspect(
			const String& name,
			const IMaterial& material,
			const IPainterManager* painters = 0 );

		//! Concrete-type discriminator — returns "Lambertian",
		//! "Phong", "GGX", etc., or "(unknown type)" for an out-of-
		//! tree IMaterial subclass.  Used by `Inspect` to drive the
		//! "Type" header row + per-type field surfacing.
		static String GetTypeName( const IMaterial& material );
	};
}

#endif
