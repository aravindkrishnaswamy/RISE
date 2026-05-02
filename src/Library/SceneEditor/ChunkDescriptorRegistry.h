//////////////////////////////////////////////////////////////////////
//
//  ChunkDescriptorRegistry.h - Shared lookup for `ChunkDescriptor`s
//    keyed by their chunk keyword (e.g. "pinhole_camera",
//    "omni_light", "bdpt_pel_rasterizer", "standard_object").  Lazily
//    builds the registry on first call from `CreateAllChunkParsers()`
//    and caches it for the lifetime of the program.
//
//    Used by every introspection class to source its panel rows from
//    the same parser-side descriptors that drive scene-file syntax.
//    Keeps the panel and the parser in lockstep — adding a parameter
//    to a chunk's `Describe()` automatically surfaces it in the
//    interactive editor.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_CHUNKDESCRIPTORREGISTRY_
#define RISE_CHUNKDESCRIPTORREGISTRY_

#include "../Parsers/ChunkDescriptor.h"
#include "../Utilities/RString.h"

namespace RISE
{
	//! Look up the `ChunkDescriptor` for a given chunk keyword.
	//! Returns null when the keyword isn't registered.  The returned
	//! pointer lives until program exit (the registry is a function-
	//! static populated once via `std::call_once`).
	const ChunkDescriptor* DescriptorForKeyword( const String& keyword );
}

#endif
