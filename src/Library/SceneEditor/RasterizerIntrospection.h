//////////////////////////////////////////////////////////////////////
//
//  RasterizerIntrospection.h - Read-only introspection of the
//    currently-active rasterizer for the interactive editor's
//    properties panel.
//
//    Phase 1: identifies the rasterizer by its registered type name
//    (e.g. "bdpt_pel_rasterizer") and surfaces a small "Active" /
//    "Type" pair plus a registry summary.  All rows are
//    `editable=false`; Phase 2 will pull rasterizer parameters from
//    the parser's ChunkDescriptor surface and expose them as a
//    descriptor-driven editable panel just like cameras.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_RASTERIZERINTROSPECTION_
#define RISE_RASTERIZERINTROSPECTION_

#include "../Interfaces/IJob.h"
#include "../Utilities/RString.h"
#include "CameraIntrospection.h"   // CameraProperty re-used as the panel-row struct
#include <vector>

namespace RISE
{
	class RasterizerIntrospection
	{
	public:
		//! Inspect the rasterizer registered under `typeName` in `job`.
		//! Returns the read-only rows the panel should render.  The
		//! rows describe the rasterizer at a high level (type name,
		//! whether it's the currently-active one, registry size); per-
		//! parameter editing arrives in Phase 2.
		static std::vector<CameraProperty> Inspect(
			const IJob& job,
			const String& typeName );
	};
}

#endif
