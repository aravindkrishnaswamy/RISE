//////////////////////////////////////////////////////////////////////
//
//  PainterIntrospection.h - CST-descriptor-driven introspection of a
//    painter (color painter OR scalar painter -- the CST chunk is the
//    single source, so both pipes share one code path) for the
//    interactive editor's properties panel.
//
//    Unlike MaterialIntrospection (which reads a LIVE IMaterial's
//    slots), painters have no live per-parameter getter surface --
//    e.g. there is no `IPainter::GetColor()` for a
//    uniformcolor_painter.  Rows are built directly from the retained
//    CST chunk: resolve the painter's chunk keyword (its `role`),
//    look up the matching ChunkDescriptor (ChunkDescriptorRegistry --
//    the same descriptor the parser uses), and read each declared
//    parameter's CURRENT text value out of the chunk (or its
//    defaultValueHint when the scene omitted it).  Every row is
//    editable -- SetProperty for Category::Painter routes through
//    SceneEditController::ApplyAgentParamEdit (entityKind "painter"),
//    a generic CST param-edit path, so there is no per-parameter
//    setter surface to build here.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_PAINTERINTROSPECTION_
#define RISE_PAINTERINTROSPECTION_

#include "../Cst/Cst.h"
#include "../Utilities/RString.h"
#include "CameraIntrospection.h"   // CameraProperty (panel-row struct)
#include <vector>

namespace RISE
{
	class PainterIntrospection
	{
	public:
		//! Returns one CameraProperty per parameter the painter's chunk
		//! descriptor declares (the `name` param itself is surfaced as
		//! a leading read-only "Type" row instead -- renaming a
		//! painter is out of scope this slice).  Repeatable parameters
		//! (e.g. spectral_painter's `cp`) are skipped -- the single-
		//! value ApplyAgentParamEdit occurrence-0 edit path isn't a
		//! good fit for a repeated param; editing those stays scene-
		//! text-only for now.  Returns an empty vector when `doc` is
		//! null, `painterName` doesn't resolve to a painter-kind chunk,
		//! or no ChunkDescriptor is registered for its keyword.
		static std::vector<CameraProperty> Inspect(
			const RISE::Cst::Document* doc,
			const String& painterName );
	};
}

#endif
