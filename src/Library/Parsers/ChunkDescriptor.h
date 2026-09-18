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
#include "../Interfaces/ILog.h"
#include "../Managers/GenericManager.h"   // g_cstFinalizeDiagSink -- lets ReportVectorArity hand DeriveToJob a specific reason (see its use below)

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
		"a single scalar \xE2\x80\x94 use a wavelength-uniform painter (`value` / `sellmeier` / "
		"`polynomial` / etc.), or a spectral-curve painter (`file`, or a `scale`/`multiply`/`add` "
		"composite over one) \xE2\x80\x94 those resolve through a single-scalar view instead of this error.";
	inline constexpr const char* const kScalarBoundToIPainterFmt =
		"%s `%s`: parameter `%s` is bound to `IPainter` chunk `%s`; this slot now requires a "
		"`scalar_painter` (physical scalar, no JH spectral uplift).  See docs/ISCALARPAINTER_REFACTOR.md.";
	inline constexpr const char* const kScalarUnknownFmt =
		"%s `%s`: parameter `%s` value `%s` is neither a registered scalar_painter nor an inline "
		"numeric literal \xE2\x80\x94 see docs/ISCALARPAINTER_REFACTOR.md";
	//   - kVectorArityFmt : ParseStateBag::GetVec2/GetVec3/GetVec4/GetMat4 (below),
	//     on a fixed-arity vector/matrix parameter whose value does not
	//     carry EXACTLY the expected token count (DL-32, docs/DEBT_LEDGER.md
	//     -- the root cause was these accessors zero-filling missing
	//     components instead of failing, so `position -4 4` silently
	//     derived `(-4, 4, 0)`).  Named per-accessor here rather than
	//     inlined at each of the ~130 call sites in ChunkParserRegistry.cpp
	//     that read a DoubleVec3/DoubleVec4/DoubleMat4 field, so every one
	//     of them is protected by construction, not by each Finalize()
	//     remembering to check.  The lone sanctioned exception is the
	//     single-number uniform-scale broadcast on `standard_object` /
	//     `override_object`'s `scale` (`ResolveScaleVec3` in
	//     ChunkParserRegistry.cpp) -- that helper reads the raw string
	//     itself and only calls GetVec3 once it has already confirmed
	//     exactly three tokens, so it never reaches this diagnostic.
	inline constexpr const char* const kVectorArityFmt =
		"ChunkParser:: parameter `%s` in `%s` expects exactly %d space-separated number(s); got %d "
		"in `%s` \xE2\x80\x94 a short or long vector used to silently zero-fill or truncate (DL-32, "
		"docs/DEBT_LEDGER.md) instead of failing the parse";

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
		DoubleVec2,   // two space-separated doubles (DL-32 round 3, docs/DEBT_LEDGER.md: a genuinely 2-component field -- a UV/(theta,phi)/(width,height) pair -- gets its OWN kind rather than a DoubleVec3 declaration a Finalize only ever reads 2 components of; see ParseStateBag::GetVec2)
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

		//! Color pipe only, DL-16/DL-17: true when this parameter's real
		//! Job.cpp resolution ALSO accepts a Scalar-pipe binding (a named
		//! `scalar_painter`, single-valued) in addition to the Color-pipe
		//! one the `pipe` field names -- `ResolveRotationPainterDual`
		//! (Job.cpp)'s "prefer Scalar, keep Color working" resolution
		//! order, used by `ggx_material.tangent_rotation` and
		//! `pbr_metallic_roughness_material.anisotropy_rotation`.  Exists
		//! so ConnectionLegalityTest's corpus sweep (real parser vs. this
		//! descriptor) can see the true legal set for these two oddballs
		//! without turning `pipe` into a bitmask for two parameters.
		//! False (the default) for every ordinary Color-pipe parameter.
		bool colorAlsoAcceptsScalar = false;

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
		SceneVariant,
		//! AN AUTHORED GUIDE-STRAND SET (`hair_guides`) -- data a
		//! `hair_geometry` reads, NOT a renderable scene entity.  Its own
		//! category, following the `Medium` precedent exactly: guides live
		//! in a Job-side name table (`Job::hairGuidesMap`), never in the
		//! IGeometryManager, so they are not interchangeable with a real
		//! `*_geometry` chunk in EITHER direction.  Splitting them out of
		//! `ChunkCategory::Geometry` is what makes the reverse-direction
		//! wiring (`standard_object.geometry <- a hair_guides name`)
		//! structurally illegal at WIRE time -- a Geometry-typed port
		//! declares `referenceCategories = {Geometry}` and a guide set is
		//! simply not a candidate for it -- rather than only at derive
		//! time in `Job::AddHairGeometry`.  See IJob::AddHairGuides.
		//!
		//! APPENDED, NEVER REORDERED: this enum crosses the GUI ABI as a
		//! bare int (SceneEditController.h's `PainterGraphNodeCategory` /
		//! `AppearanceClosureEntry::category` "just append, never reorder"
		//! contract), so a new value goes on the END even when a tidier
		//! home would be next to `Geometry`.
		HairGuides
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
		// Reads two space-separated doubles into out[2] (a genuinely
		// 2-component field -- a UV pair, a (theta,phi)/(width,height)
		// pair -- NOT a DoubleVec3 a caller only reads two components of).
		// Same DL-32 arity hard-error contract as GetVec3 below: absent
		// zero-fills nothing and returns false; present-but-wrong-arity
		// zero-fills `out`, logs+latches via ReportVectorArity, and still
		// returns true (key was present) for the same "most callers never
		// checked the return value" reason GetVec3's own comment explains.
		//
		// DL-32 round 3 (docs/DEBT_LEDGER.md): added alongside GetVec3/
		// GetVec4/GetMat4 when a full-file audit found 15 Finalize() sites
		// reading a fixed-2-token value via a RAW `sscanf` on
		// `bag.GetString(key).c_str()` -- bypassing every one of these
		// accessors, and `DispatchChunkParameters`'s own finite-number gate,
		// entirely (round 2 protected only the sites that actually CALL
		// GetVec3/GetVec4/GetMat4; a raw sscanf site was exactly as
		// unprotected as pre-round-1 `scale` was).  Every 2-component
		// parameter this accessor now serves used to be mis-declared
		// `ValueKind::DoubleVec3` (`perlin2d_painter` scale/shift,
		// `controlled_smoothness2d_painter` center, `gerstnerwave_painter`
		// wind_dir, `polynomial_function2d_painter` center/scale,
		// `composite_function2d_painter`'s four uv_scale/uv_offset params,
		// camera `target_orientation`) or `ValueKind::Double`
		// (`orthographic_camera`'s `viewport_scale` -- a SCALAR kind read as
		// 2 components, one degree worse) -- now `ValueKind::DoubleVec2`,
		// read through here.
		bool GetVec2( const std::string& key, double out[2] ) const
		{
			ValidateAccess(key);
			std::map<std::string, std::string>::const_iterator it = mSingles.find( key );
			if( it == mSingles.end() ) return false;
			out[0] = out[1] = 0.0;
			const int actual = CountValueTokens( it->second );
			if( actual != 2 ) {
				ReportVectorArity( key, 2, actual, it->second );
				return true;
			}
			sscanf( it->second.c_str(), "%lf %lf", &out[0], &out[1] );
			return true;
		}
		// Reads three space-separated doubles into out[3].  Returns
		// true if the key was present (so callers can apply unit
		// conversions like DEG_TO_RAD only on explicit input).
		//
		// DL-32 (docs/DEBT_LEDGER.md): this used to zero-fill `out` and
		// `sscanf` into it unconditionally, so a value with FEWER tokens
		// than the arity "succeeded" with the missing components silently
		// 0 (`position -4 4` derived `(-4, 4, 0)`, no diagnostic anywhere).
		// The arity is now checked here, at the primitive every
		// DoubleVec3-kind parameter (~130 call sites in
		// ChunkParserRegistry.cpp: position/orientation on every object,
		// camera location/lookat/up, absorption/scattering/emission on
		// volumes, mesh corner points, bbox_min/max, radiance_orient,
		// painter scale/shift, ...) reads through, rather than at each
		// call site individually.  A mismatch is a HARD error: it is
		// logged (see kVectorArityFmt) and recorded via HadHardError(),
		// which the two live Finalize()-invoking surfaces
		// (IAsciiChunkParser::ParseChunk's default impl and Cst.cpp's
		// direct Finalize() calls) AND with the whole chunk failing
		// instead of silently deriving a degenerate scene.  The lone
		// sanctioned exception is `standard_object`/`override_object`'s
		// `scale`, whose single-number uniform-scale broadcast is resolved
		// by `ResolveScaleVec3` (ChunkParserRegistry.cpp) BEFORE this
		// accessor is ever called for that case -- see that helper's
		// comment.
		bool GetVec3( const std::string& key, double out[3] ) const
		{
			ValidateAccess(key);
			std::map<std::string, std::string>::const_iterator it = mSingles.find( key );
			if( it == mSingles.end() ) return false;
			out[0] = out[1] = out[2] = 0.0;
			const int actual = CountValueTokens( it->second );
			if( actual != 3 ) {
				ReportVectorArity( key, 3, actual, it->second );
				return true;
			}
			sscanf( it->second.c_str(), "%lf %lf %lf", &out[0], &out[1], &out[2] );
			return true;
		}
		// Reads four space-separated doubles into out[4] (e.g. quaternion
		// xyzw).  Returns true if the key was present.  Same DL-32 arity
		// hard-error as GetVec3 -- see its comment.
		bool GetVec4( const std::string& key, double out[4] ) const
		{
			ValidateAccess(key);
			std::map<std::string, std::string>::const_iterator it = mSingles.find( key );
			if( it == mSingles.end() ) return false;
			out[0] = out[1] = out[2] = out[3] = 0.0;
			const int actual = CountValueTokens( it->second );
			if( actual != 4 ) {
				ReportVectorArity( key, 4, actual, it->second );
				return true;
			}
			sscanf( it->second.c_str(), "%lf %lf %lf %lf", &out[0], &out[1], &out[2], &out[3] );
			return true;
		}
		// Reads sixteen space-separated doubles into out[16], column-major
		// 4×4 (matches glTF and RISE's internal Matrix4 layout).  Returns
		// true if the key was present.  Same DL-32 arity hard-error as
		// GetVec3 -- see its comment.
		bool GetMat4( const std::string& key, double out[16] ) const
		{
			ValidateAccess(key);
			std::map<std::string, std::string>::const_iterator it = mSingles.find( key );
			if( it == mSingles.end() ) return false;
			for( int i = 0; i < 16; ++i ) out[i] = 0.0;
			const int actual = CountValueTokens( it->second );
			if( actual != 16 ) {
				ReportVectorArity( key, 16, actual, it->second );
				return true;
			}
			sscanf( it->second.c_str(),
				"%lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf",
				&out[0],  &out[1],  &out[2],  &out[3],
				&out[4],  &out[5],  &out[6],  &out[7],
				&out[8],  &out[9],  &out[10], &out[11],
				&out[12], &out[13], &out[14], &out[15] );
			return true;
		}
		// True once any GetVec3/GetVec4/GetMat4 call on this bag has hit a
		// wrong-arity value (DL-32).  The two live call sites that invoke a
		// chunk parser's Finalize() -- IAsciiChunkParser::ParseChunk's
		// default implementation and Cst.cpp's direct Finalize() calls --
		// AND this into their success check so the whole chunk fails
		// instead of silently applying a partially zero-filled vector.
		bool HadHardError() const { return mHardError; }
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
		// Counts whitespace-separated tokens in `s`, stopping at an inline
		// `#` comment marker -- mirrors ChunkParserRegistry.cpp's
		// `AllTokensAreFiniteNumbers` tokenization (the DispatchChunkParameters
		// gate that already ran, for every declared numeric-kind parameter,
		// before Finalize() reaches this accessor) so a legitimate trailing
		// comment (`position 1 2 3 # meters`) is not miscounted as extra
		// arity.  A pure lexical count, deliberately not re-validating
		// numeric-ness a second time.
		static int CountValueTokens( const std::string& s )
		{
			int n = 0;
			const char* p = s.c_str();
			while( *p ) {
				while( *p == ' ' || *p == '\t' ) ++p;
				if( !*p || *p == '#' ) break;
				++n;
				while( *p && *p != ' ' && *p != '\t' && *p != '#' ) ++p;
			}
			return n;
		}
		// Logs the DL-32 arity diagnostic (kVectorArityFmt: names the
		// chunk, the parameter, expected vs. got) and latches mHardError so
		// the chunk's Finalize() is treated as failed by its caller even
		// though this accessor itself still returns `true` ("key was
		// present") for source compatibility with the ~130 existing call
		// sites that only branch on presence, not on validity.  Defined
		// out-of-line below (mirroring ValidateAccess) because
		// `ChunkDescriptor` is only forward-declared at this point in the
		// file -- `mDescriptor->keyword` needs the complete type.
		void ReportVectorArity( const std::string& key, int expected, int actual, const std::string& raw ) const;

		std::map<std::string, std::string>              mSingles;
		std::map<std::string, std::vector<std::string> > mRepeatables;
		const ChunkDescriptor*                          mDescriptor;
		mutable bool                                     mHardError = false;
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
		// DL-32 (docs/DEBT_LEDGER.md), additive metadata only -- does not
		// itself change parsing.  True for a DoubleVec2/DoubleVec3-kind
		// parameter that ALSO accepts a single finite number as an explicit
		// uniform broadcast: `scale` on `standard_object`/`override_object`
		// (DoubleVec3, resolved by `ResolveScaleVec3`) and `viewport_scale`
		// on `orthographic_camera` (DoubleVec2, DL-32 round 3, resolved by
		// `ResolveVec2UniformBroadcast`) -- both in ChunkParserRegistry.cpp,
		// both helpers pre-validate the 1-or-N arity themselves and only
		// call the shared `GetVec2`/`GetVec3` accessor once they have
		// already confirmed the full token count, so the broadcast case
		// never reaches, and is never rejected by, that accessor's own
		// arity hard-error.  Every OTHER DoubleVec2/DoubleVec3/DoubleVec4/
		// DoubleMat4 parameter requires the full, exact token count.
		// Surfaced so the editor's syntax highlighter / suggestion engine
		// can show the shorthand is legal here specifically, rather than a
		// reader having to know to special-case these two params by name.
		bool                         allowsUniformScalarBroadcast = false;
		// DL-164 (docs/DEBT_LEDGER.md): true for a ValueKind::String parameter
		// whose value is texture-expression-VM BODY TEXT that may embed a
		// `sample(name)` / `sample_scalar(name)` painter reference -- today
		// `expression_painter`'s `expr`/`def` and `scalar_painter`'s
		// `expression`/`def` (both routed through
		// BuildExpressionProgramFromChunkFields with context vars enabled;
		// `expression_function2d`'s `expr`/`def` run with context vars OFF,
		// so `sample()` is a compile error there and this stays false).  Such
		// a reference is invisible to the ordinary Reference/tuple scan below
		// it (the value is a whole expression, not a bare chunk name), so
        // Cst.cpp's BuildReferenceGraph scans a flagged param's text with
		// ExpressionProgram::Builder::ExtractSampleRefs (the SAME tokenizer
		// ParseSampleCall consumes) instead, and DocRename rewrites a
		// matched call's identifier in place with RewriteSampleCallRefs --
		// unlike the piecewise_linear_function2d `cp` precedent, a sample()
		// reference IS rewritable (its whole value is not a single chunk
		// name, so it cannot go through the ordinary Reference substitution
		// path, but the identifier's exact byte range inside the text is
		// well-defined and reusable). A descriptor FLAG rather than a
		// role/param name check keeps the scan sites in Cst.cpp honest by
		// construction if a future chunk gains its own expression-body field.
		bool                         carriesExpressionSampleRefs = false;
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

	inline void ParseStateBag::ReportVectorArity( const std::string& key, int expected, int actual, const std::string& raw ) const
	{
		mHardError = true;
		const char* keyword = mDescriptor && !mDescriptor->keyword.empty() ? mDescriptor->keyword.c_str() : "(unknown)";
		GlobalLog()->PrintEx( eLog_Error, kVectorArityFmt, key.c_str(), keyword, expected, actual, raw.c_str() );
		// Also hand DeriveToJob the SPECIFIC reason (GenericManager.h's
		// g_cstFinalizeDiagSink contract), the same channel SweepReject /
		// Job.cpp's other Finalize()-internal refusals use, so the agent
		// surface's per-chunk diagnostic names DL-32/the parameter/the
		// arity instead of the generic "apply failed (e.g. unresolved
		// reference); see log" fallback.  `empty()`-gated like the
		// GLTFSceneImporter precedent: the FIRST arity failure inside one
		// Finalize() call wins, matching "log the earliest concrete cause"
		// elsewhere in this codebase.
		if( g_cstFinalizeDiagSink && g_cstFinalizeDiagSink->empty() ) {
			char buf[1024];
			snprintf( buf, sizeof(buf), kVectorArityFmt, key.c_str(), keyword, expected, actual, raw.c_str() );
			*g_cstFinalizeDiagSink = buf;
		}
	}
}

#endif
