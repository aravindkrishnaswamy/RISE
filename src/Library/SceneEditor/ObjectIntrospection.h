//////////////////////////////////////////////////////////////////////
//
//  ObjectIntrospection.h - Read-only introspection of an IObject for
//    the interactive editor's properties panel.
//
//    Phase 1: returns a small fixed list of {name, kind, value-as-
//    string, description} tuples — Name, Geometry name, Material name,
//    final-transform position.  Every row is `editable=false`; full
//    descriptor-driven object editing arrives in Phase 2 (would need
//    object-side ChunkDescriptors and a SetObjectProperty SceneEdit
//    op so undo/redo work end-to-end with the camera path).
//
//    The shape mirrors CameraIntrospection so the platform UIs and
//    the SceneEditController dispatcher can treat all category
//    introspection identically.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_OBJECTINTROSPECTION_
#define RISE_OBJECTINTROSPECTION_

#include "../Interfaces/IObject.h"
#include "../Utilities/RString.h"
#include "CameraIntrospection.h"   // CameraProperty (re-used as the panel-row struct)
#include <vector>

namespace RISE
{
	class IMaterialManager;
	class IShaderManager;

	class ObjectIntrospection
	{
	public:
		//! Inspect a single object.  `name` is the manager-registered
		//! name.  Optional manager pointers let the editable surface
		//! emit material / shader presets as quick-pick combo entries
		//! (so the user can swap material by selecting from a list).
		//! Pass null managers to skip those rows.
		static std::vector<CameraProperty> Inspect(
			const String& name,
			const IObject& obj,
			const IMaterialManager* materials = 0,
			const IShaderManager*   shaders   = 0 );
	};
}

#endif
