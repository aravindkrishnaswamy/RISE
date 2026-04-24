//////////////////////////////////////////////////////////////////////
//
//  ChunkDescriptor.h - Grammar descriptor types shared by the scene
//    parser and any consumer of the scene grammar (the scene-editor
//    suggestion engine, future validators, documentation generators).
//
//    A ChunkDescriptor describes one chunk type the scene parser
//    accepts: its keyword ("pixelpel_rasterizer"), its category
//    (Rasterizer), and the parameters it understands.  Each
//    ParameterDescriptor couples the parameter's metadata
//    (name/kind/enum/reference/description/default) with the apply
//    function that interprets a value string.  The same descriptor
//    drives parsing (via ParseChunk) and suggestion (via
//    SceneEditorSuggestions) — one source of truth, no drift.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-24
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CHUNK_DESCRIPTOR_
#define CHUNK_DESCRIPTOR_

#include <string>
#include <vector>

#include "../Utilities/RString.h"

namespace RISE
{
	enum class ValueKind
	{
		Bool,         // TRUE / FALSE
		UInt,         // non-negative integer
		Double,       // double-precision floating point
		DoubleVec3,   // three space-separated doubles
		String,       // free identifier or string
		Filename,     // path, resolved via RISE_MEDIA_PATH
		Enum,         // fixed set — see ParameterDescriptor::enumValues
		Reference     // name of another chunk — see ParameterDescriptor::referenceCategory
	};

	enum class ChunkCategory
	{
		Painter,
		Function,
		Material,
		Camera,
		Geometry,
		Modifier,
		Medium,
		Object,
		ShaderOp,
		Shader,
		Rasterizer,
		RasterizerOutput,
		Light,
		PhotonMap,
		PhotonGather,
		IrradianceCache,
		Animation
	};

	// Base class for per-chunk parse state.  Each concrete chunk parser
	// defines its own subclass holding accumulator variables; the parser's
	// apply functions downcast to that subclass.  Lifetime is owned by the
	// parser for the duration of a single ParseChunk call.
	class IChunkParseState
	{
	public:
		virtual ~IChunkParseState() {}
	};

	// Applies a parameter value to the parser state.  Function pointer
	// (not std::function) to keep dispatch allocation-free and the
	// ChunkDescriptor trivially copyable.  The state reference is always
	// downcast to the concrete subclass associated with the owning chunk.
	typedef void (*ApplyParameterFn)( IChunkParseState& state, const RISE::String& value );

	struct ParameterDescriptor
	{
		std::string                name;
		ValueKind                  kind       = ValueKind::String;
		bool                       required   = false;
		bool                       repeatable = false;
		std::vector<std::string>   enumValues;                              // populated when kind == Enum
		std::vector<ChunkCategory> referenceCategories;                      // populated when kind == Reference; a param can accept references from multiple categories (e.g. any Painter or Function)
		std::vector<ValueKind>     tupleKinds;                               // populated when the value is a whitespace-separated tuple of typed tokens (e.g. "shaderop foo 0 5 +" on advanced_shader — each token is a Reference, UInt, UInt, Enum); empty means the whole value is kind-typed
		std::string                description;
		std::string                defaultValueHint;
		ApplyParameterFn           apply      = nullptr;
	};

	struct ChunkDescriptor
	{
		std::string                      keyword;
		ChunkCategory                    category = ChunkCategory::Painter;
		std::vector<ParameterDescriptor> parameters;
		std::string                      description;
	};
}

#endif
