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

#include <cstdio>
#include <cstdlib>
#include <map>
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
		DoubleVec4,   // four space-separated doubles (e.g. quaternion xyzw)
		DoubleMat4,   // sixteen space-separated doubles, column-major 4x4
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

	// Base class for per-chunk parse state.  In the registry-driven
	// architecture, the standard subclass is ParseStateBag (below); a
	// chunk parser only needs a custom subclass when the bag's typed
	// accessors are insufficient (vanishingly rare).
	class IChunkParseState
	{
	public:
		virtual ~IChunkParseState() {}
	};

	// Type-erased property bag populated by the generic parameter
	// dispatcher (DispatchChunkParameters in AsciiSceneParser.cpp) and
	// consumed by each parser's Finalize() method.  This is the
	// mechanism that makes the descriptor the single source of truth:
	// the dispatcher walks the input lines, validates each name
	// against the chunk's ChunkDescriptor::parameters, and stores
	// matched values in the bag.  Unknown parameters fail the parse.
	// Finalize() then reads the values out using the typed accessors
	// and emits the corresponding pJob.AddX call.
	struct ChunkDescriptor;

	class ParseStateBag : public IChunkParseState
	{
	public:
		ParseStateBag(const ChunkDescriptor* desc = nullptr) : mDescriptor(desc) {}

		bool Has( const std::string& key ) const
		{
			return mSingles.find( key ) != mSingles.end()
				|| mRepeatables.find( key ) != mRepeatables.end();
		}

		// Typed accessors.  When `key` is absent each returns the
		// default supplied by the caller — Finalize() typically passes
		// the same default the legacy ParseChunk used as its initial
		// value, so behaviour matches the pre-migration parser.
		void ValidateAccess(const std::string& key) const;

		std::string GetString( const std::string& key, const std::string& def = std::string() ) const
		{
			ValidateAccess(key);
			std::map<std::string, std::string>::const_iterator it = mSingles.find( key );
			return it == mSingles.end() ? def : it->second;
		}
		double GetDouble( const std::string& key, double def = 0.0 ) const
		{
			ValidateAccess(key);
			std::map<std::string, std::string>::const_iterator it = mSingles.find( key );
			if( it == mSingles.end() ) return def;
			return RISE::String( it->second.c_str() ).toDouble();
		}
		unsigned int GetUInt( const std::string& key, unsigned int def = 0u ) const
		{
			ValidateAccess(key);
			std::map<std::string, std::string>::const_iterator it = mSingles.find( key );
			if( it == mSingles.end() ) return def;
			return RISE::String( it->second.c_str() ).toUInt();
		}
		int GetInt( const std::string& key, int def = 0 ) const
		{
			ValidateAccess(key);
			std::map<std::string, std::string>::const_iterator it = mSingles.find( key );
			if( it == mSingles.end() ) return def;
			return atoi( it->second.c_str() );
		}
		bool GetBool( const std::string& key, bool def = false ) const
		{
			ValidateAccess(key);
			std::map<std::string, std::string>::const_iterator it = mSingles.find( key );
			if( it == mSingles.end() ) return def;
			return RISE::String( it->second.c_str() ).toBoolean();
		}
		// Reads three space-separated doubles into out[3].  Returns
		// true if the key was present (so callers can apply unit
		// conversions like DEG_TO_RAD only on explicit input).
		bool GetVec3( const std::string& key, double out[3] ) const
		{
			ValidateAccess(key);
			std::map<std::string, std::string>::const_iterator it = mSingles.find( key );
			if( it == mSingles.end() ) return false;
			out[0] = out[1] = out[2] = 0.0;
			sscanf( it->second.c_str(), "%lf %lf %lf", &out[0], &out[1], &out[2] );
			return true;
		}
		// Reads four space-separated doubles into out[4] (e.g. quaternion
		// xyzw).  Returns true if the key was present.
		bool GetVec4( const std::string& key, double out[4] ) const
		{
			ValidateAccess(key);
			std::map<std::string, std::string>::const_iterator it = mSingles.find( key );
			if( it == mSingles.end() ) return false;
			out[0] = out[1] = out[2] = out[3] = 0.0;
			sscanf( it->second.c_str(), "%lf %lf %lf %lf", &out[0], &out[1], &out[2], &out[3] );
			return true;
		}
		// Reads sixteen space-separated doubles into out[16], column-major
		// 4×4 (matches glTF and RISE's internal Matrix4 layout).  Returns
		// true if the key was present.
		bool GetMat4( const std::string& key, double out[16] ) const
		{
			ValidateAccess(key);
			std::map<std::string, std::string>::const_iterator it = mSingles.find( key );
			if( it == mSingles.end() ) return false;
			for( int i = 0; i < 16; ++i ) out[i] = 0.0;
			sscanf( it->second.c_str(),
				"%lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf",
				&out[0],  &out[1],  &out[2],  &out[3],
				&out[4],  &out[5],  &out[6],  &out[7],
				&out[8],  &out[9],  &out[10], &out[11],
				&out[12], &out[13], &out[14], &out[15] );
			return true;
		}
		// All values for a repeatable parameter, in input order.
		const std::vector<std::string>& GetRepeatable( const std::string& key ) const
		{
			ValidateAccess(key);
			std::map<std::string, std::vector<std::string> >::const_iterator it = mRepeatables.find( key );
			static const std::vector<std::string> kEmpty;
			return it == mRepeatables.end() ? kEmpty : it->second;
		}
		// Raw access — useful when Finalize needs to do its own custom
		// parsing on a value (e.g. composite tokens like "phase hg 0.5").
		const std::map<std::string, std::string>& Singles() const { return mSingles; }

		// Internal — only DispatchChunkParameters should call these.
		void SetSingle( const std::string& key, const std::string& value )      { mSingles[key] = value; }
		void AppendRepeatable( const std::string& key, const std::string& value ){ mRepeatables[key].push_back( value ); }

	private:
		std::map<std::string, std::string>              mSingles;
		std::map<std::string, std::vector<std::string> > mRepeatables;
		const ChunkDescriptor*                          mDescriptor;
	};

	// Applies a parameter value to a custom IChunkParseState subclass.
	// Retained for parsers that need custom dispatch beyond the
	// standard ParseStateBag — almost no parser should need this.
	typedef void (*ApplyParameterFn)( IChunkParseState& state, const RISE::String& value );

	// One numeric / string preset surfaced to the editor properties
	// panel as a quick-pick combo entry.  The user can still type a
	// custom value — the preset list is a convenience, not a constraint.
	// `value` is the literal string the parser would accept (so "36"
	// for a sensor-size mm number, not "Full-frame 35mm").
	struct ParameterPreset
	{
		std::string label;        // human-readable, e.g. "Full-frame 35mm"
		std::string value;        // parser-acceptable literal, e.g. "36"
	};

	struct ParameterDescriptor
	{
		std::string                  name;
		ValueKind                    kind       = ValueKind::String;
		bool                         required   = false;
		bool                         repeatable = false;
		std::vector<std::string>     enumValues;                            // populated when kind == Enum
		std::vector<ChunkCategory>   referenceCategories;                    // populated when kind == Reference; a param can accept references from multiple categories (e.g. any Painter or Function)
		std::vector<ValueKind>       tupleKinds;                             // populated when the value is a whitespace-separated tuple of typed tokens (e.g. "shaderop foo 0 5 +" on advanced_shader — each token is a Reference, UInt, UInt, Enum); empty means the whole value is kind-typed
		std::vector<ParameterPreset> presets;                                // optional named values for the editor's quick-pick combo box; the line edit stays usable for arbitrary input
		std::string                  description;
		std::string                  defaultValueHint;
		std::string                  unitLabel;                              // optional short unit suffix shown next to the editor field (e.g. "mm", "°", "scene units", ""). Pure presentation hint — the parser ignores it. Empty means dimensionless / no label.
		ApplyParameterFn             apply      = nullptr;
	};

	struct ChunkDescriptor
	{
		std::string                      keyword;
		ChunkCategory                    category = ChunkCategory::Painter;
		std::vector<ParameterDescriptor> parameters;
		std::string                      description;
	};

	inline void ParseStateBag::ValidateAccess(const std::string& key) const
	{
		if( !mDescriptor ) return;
		bool found = false;
		for( size_t i=0; i<mDescriptor->parameters.size(); ++i ) {
			if( mDescriptor->parameters[i].name == key ) {
				found = true;
				break;
			}
		}
		if( !found ) {
			fprintf(stderr, "ChunkParser Bug: Finalize() requested undeclared parameter `%s` in chunk `%s`\n", 
				key.c_str(), mDescriptor->keyword.empty() ? "(unknown)" : mDescriptor->keyword.c_str());
		}
	}
}

#endif
