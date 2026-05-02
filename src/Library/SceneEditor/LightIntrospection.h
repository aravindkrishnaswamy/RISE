//////////////////////////////////////////////////////////////////////
//
//  LightIntrospection.h - Read-only introspection of an ILight for
//    the interactive editor's properties panel.
//
//    Phase 1: returns Name + Position + ConeHalfAngle + Direction
//    rows.  All `editable=false`; Phase 2 will descriptor-drive the
//    full editable surface for each light type (point / spot /
//    directional / ambient).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_LIGHTINTROSPECTION_
#define RISE_LIGHTINTROSPECTION_

#include "../Utilities/RString.h"
#include "CameraIntrospection.h"   // CameraProperty re-used as the panel-row struct
#include <vector>

namespace RISE
{
	// Forward-declared.  ILight.h pulls in IRayCaster.h →
	// ILuminaryManager.h → IScene.h → ILightManager.h → ILightPriv.h,
	// which inherits from ILight before ILight's class body has been
	// declared if the chain enters here.  Forward-declaring keeps the
	// header lightweight and lets the .cpp pull the full include in
	// the right order.
	class ILight;

	class LightIntrospection
	{
	public:
		//! Inspect a single light.  Returns the read-only property rows
		//! the panel renders.  Empty vector if the light is degenerate.
		static std::vector<CameraProperty> Inspect(
			const String& name,
			const ILight& light );
	};
}

#endif
