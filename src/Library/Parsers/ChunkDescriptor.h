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
#include <climits>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "../Utilities/OidnConfig.h"
#include "../Utilities/RString.h"

namespace RISE
{
	// ---- shared parser diagnostic strings --------------------------------
	//
	// These `printf`-style format strings are the SOLE definition of a
	// handful of diagnostics that two or three independent .cpp files need
	// to either emit (the real parser) or quote verbatim (a validator that
	// mirrors the parser's behaviour without re-running it).  Living here
	// keeps every consumer byte-identical by construction -- no consumer
	// may hold its own copy of the literal text.
	//
	//   - kUndeclaredParameterFmt : ChunkParserRegistry.cpp's
	//     DispatchChunkParameters, on an unrecognized parameter name.
	//   - kScalarBoundToPerChannelFmt / kScalarBoundToIPainterFmt /
	//     kScalarUnknownFmt : Job.cpp's ResolveOrDiagnoseScalar, the
	//     three-way Scalar-pipe failure diagnostic (branches a/b/c).
	//
	// Consumed verbatim (not copied) by
	// SceneEditor/ConnectionLegality.cpp, which has no other way to stay
	// in lockstep with the real parser's wording -- see that file's
	// header comment for why a validator quoting the parser's own
	// diagnostics, rather than re-deriving them, is load-bearing.
	// `inline constexpr` (C++17) gives every TU the same address-stable
	// definition with no companion .cpp and no ODR risk.
	inline constexpr const char* const kUndeclaredParameterFmt =
		"ChunkParser:: Failed to parse parameter name `%s` (not declared in `%s` descriptor)";
	inline constexpr const char* const kScalarBoundToPerChannelFmt =
		"%s `%s`: parameter `%s` is bound to per-channel scalar_painter `%s`, but this slot reads "
		"a single scalar \xE2\x80\x94 use a wavelength-uniform painter (`value` or `file` / `sellmeier` / etc.) instead.";
	inline constexpr const char* const kScalarBoundToIPainterFmt =
		"%s `%s`: parameter `%s` is bound to `IPainter` chunk `%s`; this slot now requires a "
		"`scalar_painter` (physical scalar, no JH spectral uplift).  See docs/ISCALARPAINTER_REFACTOR.md.";
	inline constexpr const char* const kScalarUnknownFmt =
		"%s `%s`: parameter `%s` value `%s` is neither a registered scalar_painter nor an inline "
		"numeric literal \xE2\x80\x94 see docs/ISCALARPAINTER_REFACTOR.md";

	//
	// to_hint() — formats a typed default value into the string form
	// used by ParameterDescriptor::defaultValueHint.  Centralised so
	// the descriptor's hint and the Finalize fallback always agree:
	// both read the same `*Defaults`-struct field, the parser passes
	// it directly to `bag.GetUInt(name, d.field)` and the descriptor
	// passes it through `to_hint(d.field)` to produce the GUI hint.
	//
	// Format rules match the historical defaultValueHint conventions:
	//   - bool   → "TRUE" / "FALSE" (caps, matching the ASCII-scene
	//              Boolean tokens RISE::String::toBoolean accepts).
	//   - UInt   → decimal, with UINT_MAX rendered as "unlimited"
	//              (the bounce-cap sentinel; RISE never advertises
	//              "-1" for a UInt-typed param).
	//   - double → %g-formatted, with 0.0 rendered as "0", integers
	//              as "1.0", and small/large magnitudes as "1e-5".
	//   - enums  → the lowercase token the parser accepts.
	//
	inline std::string to_hint( bool v )         { return v ? "TRUE" : "FALSE"; }
	inline std::string to_hint( int v )          { return std::to_string( v ); }
	inline std::string to_hint( unsigned int v )
	{
		if( v == UINT_MAX ) return "unlimited";
		return std::to_string( v );
	}
	inline std::string to_hint( double v )
	{
		if( v == 0.0 ) return "0";
		char buf[32];
		const double a = std::fabs( v );
		if( a < 1e-3 || a >= 1e6 ) {
			std::snprintf( buf, sizeof(buf), "%g", v );
		} else if( v == std::floor( v ) ) {
			std::snprintf( buf, sizeof(buf), "%.1f", v );
		} else {
			std::snprintf( buf, sizeof(buf), "%g", v );
		}
		return buf;
	}
	inline std::string to_hint( const std::string& v ) { return v; }
	inline std::string to_hint( const char* v )        { return v ? std::string( v ) : std::string(); }

	inline std::string to_hint( OidnQuality v )
	{
		switch( v ) {
			case OidnQuality::High:     return "high";
			case OidnQuality::Balanced: return "balanced";
			case OidnQuality::Fast:     return "fast";
			case OidnQuality::Auto:
			default:                    return "auto";
		}
	}
	inline std::string to_hint( OidnDevice v )
	{
		switch( v ) {
			case OidnDevice::CPU:  return "cpu";
			case OidnDevice::GPU:  return "gpu";
			case OidnDevice::Auto:
			default:               return "auto";
		}
	}
	inline std::string to_hint( OidnPrefilter v )
	{
		switch( v ) {
			case OidnPrefilter::Accurate: return "accurate";
			case OidnPrefilter::Fast:
			default:                       return "fast";
		}
	}

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

	// ParameterSemantics.pipe -- S17 (docs/gui/NODE_GRAPH_CANVAS.md sect.
	// 6; docs/gui/MATERIAL_EDITOR.md sect. 6.4; docs/GUI_ROADMAP.md:361).
	// A Reference-kind parameter's `referenceCategories` says which
	// CHUNK CATEGORIES may bind (e.g. {Painter} for `reflectance`) but
	// several of those categories are internally split across TWO OR
	// MORE runtime managers a chunk's own keyword doesn't disclose --
	// `ChunkCategory::Painter` alone covers 41 distinct keywords, of
	// which exactly one (`scalar_painter`) resolves through
	// IScalarPainterManager and every other one resolves through
	// IPainterManager.  `pipe` is the DOWNSTREAM manager a parameter's
	// value is actually resolved against in Job.cpp -- audited per
	// parameter against the real `Add*` resolution code (never guessed
	// from the parameter's name; see ChunkParserRegistry.cpp's
	// `p.semantics.pipe = ...` assignments and their audit comments).
	//
	// Function2D is its own case worth flagging up front: MOST colour
	// `IPainter` chunks dual-register into IFunction2DManager too (see
	// Job.cpp's `RegisterPainterDual` -- any painter usable in a
	// `function2d`-typed slot) with exactly one carve-out
	// (`expression_painter`, single-registered -- see its Job.cpp
	// comment "Deliberately SINGLE-manager registration").  A
	// Function2D-piped slot's legal candidate set is therefore NOT
	// "chunks whose own category is Function" -- ConnectionLegality
	// encodes the real rule (Function-category chunks, PLUS every
	// Painter-category chunk except `expression_painter` and
	// `scalar_painter`), not this enum alone.
	enum class ParameterPipe
	{
		Unspecified,  // not yet audited for this parameter (default) -- non-Reference kinds, and any chunk family outside this slice's Painter/Material audit scope, stay Unspecified rather than guessed
		Color,        // IPainterManager -- the colour/reflectance/emission pipe; spectral rasterizers JH-uplift an inline numeric value read through this pipe (docs/ISCALARPAINTER_REFACTOR.md)
		Scalar,       // IScalarPainterManager -- the physical-scalar pipe (IOR, roughness, scattering, absorption, phase asymmetry, ...); NEVER JH-uplifted
		Material,     // IMaterialManager
		Function1D,   // IFunction1DManager -- today only `piecewise_linear_function` registers here
		Function2D,   // IFunction2DManager -- see the dual-registration note above; ConnectionLegality, not this enum, carries the exact legal-candidate rule
		Geometry,     // IGeometryManager / IObjectManager
		Other         // resolves against something else again; `note` explains
	};

	//! Companion constraint fields for a `ParameterPipe`-bearing Reference
	//! parameter.  Deliberately a STRUCT with separate fields, not a
	//! richer `pipe` enum -- MATERIAL_EDITOR.md sect. 6.4's adopted shape
	//! (the review's correction: "a single `pipe` enum is too weak on its
	//! own"). This slice (S17) populates `pipe` + `requireSingle` +
	//! `keywordAllowlist` + `note`; the sibling fields MATERIAL_EDITOR.md
	//! sect. 6.4 lists for a LATER slice (cardinality beyond
	//! requireSingle, units, colour space, spatial-vs-spectral) are not
	//! added here -- this is the "minimal, additive" first cut the S17
	//! brief calls for, not the full structure.
	struct ParameterSemantics
	{
		ParameterPipe pipe = ParameterPipe::Unspecified;

		//! Scalar pipe only: mirrors `ResolveOrDiagnoseScalar`'s own
		//! `requireSingle` argument for THIS parameter -- true rejects a
		//! per-channel/triple scalar_painter binding (the slot reads
		//! `.v[0]` only and would silently drop G/B).  Meaningless
		//! (false) outside the Scalar pipe.
		bool requireSingle = false;

		//! Optional: narrows legality to a specific chunk KEYWORD set
		//! that is STRICTER than "every chunk of the matching pipe" --
		//! e.g. `scalar_painter`'s `texture` form only accepts a
		//! raster-image painter chunk (png_painter / jpg_painter /
		//! hdr_painter / exr_painter / tiff_painter), never any other
		//! Color-pipe chunk, because the resolution code
		//! `dynamic_cast<TexturePainter*>`s the resolved painter.  Empty
		//! means "every chunk whose pipe/category matches is legal" (the
		//! common case).
		std::vector<std::string> keywordAllowlist;

		//! Free-text audit annotation for an oddball binding -- pipe says
		//! one thing (e.g. Color) while the authored MEANING is something
		//! else (e.g. an angle, or a physical scalar bridged through the
		//! colour manager).  Empty for the ordinary case.  The two named
		//! oddballs this slice documents: GGX's `tangent_rotation` and
		//! PBRMetallicRoughness's `anisotropy_rotation` (both Color pipe,
		//! both semantically an angle in radians -- ISCALARPAINTER_-
		//! REFACTOR.md / MATERIAL_EDITOR.md sect. 1's documented
		//! oddball), and PBRMetallicRoughness's `metallic` / `roughness`
		//! / `specular_factor` / `specular_color` / `anisotropy_factor`
		//! (Color pipe by construction: `Job::AddPBRMetallicRoughnessMaterial`'s
		//! `resolveOrSynth` helper checks `pPntManager->GetItem` and, on a
		//! miss, falls back to `atof` + a synthesized uniform-colour
		//! painter -- a `scalar_painter` name here silently synthesizes
		//! ZERO rather than binding, since `atof` on a non-numeric name
		//! is 0.0 -- MATERIAL_EDITOR.md sect. 6.4 names this "pbr's
		//! colour-manager roughness").
		std::string note;
	};

	enum class ChunkCategory
	{
		Painter,
		Function,
		Material,
		Camera,
		Film,
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
		Animation,
		SceneVariant
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
		ParameterSemantics           semantics;                              // S17, additive: which manager/pipe a Reference-kind param's value actually resolves against (ChunkDescriptor.h's ParameterPipe doc comment). Default-constructed (Unspecified) for every non-Reference param and every family this slice didn't audit.
		ApplyParameterFn             apply      = nullptr;
	};

	struct ChunkDescriptor
	{
		std::string                      keyword;
		ChunkCategory                    category = ChunkCategory::Painter;
		std::vector<ParameterDescriptor> parameters;
		std::string                      description;
		bool                             unnamedRepeatable = false;              //!< true iff multiple UNNAMED chunks of this keyword may coexist -- the derive APPENDS rather than last-wins (e.g. timeline); consumed by the agent insert/remove verbs
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
