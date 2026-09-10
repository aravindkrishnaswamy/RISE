//////////////////////////////////////////////////////////////////////
//
//  ChunkParserRegistry.cpp - Definitions of every concrete
//    IAsciiChunkParser subclass, the CreateAllChunkParsers factory,
//    the default IAsciiChunkParser::ParseChunk dispatch, and the public
//    DispatchChunkParameters / ClearChunkParserState / LastAllocatedCameraName
//    wrappers.  This is the SHARED chunk-parser registry: it is consumed
//    by the CST derive path (Cst/Cst.cpp, ParseToCst + DeriveToJob -- the
//    ONLY scene-load path) and the scene-editor introspection panels
//    (SceneEditor/ChunkDescriptorRegistry.cpp).
//
//    Split out of AsciiSceneParser.cpp (Model-B P5 Slice 6b) precisely so
//    the legacy streaming loader could be retired as a clean unit without
//    disturbing this registry -- that retirement happened in Slice 6c/6d
//    (AsciiSceneParser.cpp and its ParseAndLoadScene are deleted).
//
//    The descriptor-driven architecture (every parser overrides only
//    Describe() + Finalize(); the default ParseChunk validates against the
//    descriptor) is documented in README.md in this directory.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include <vector>
#include <map>
#include <set>
#include <stack>
#include <string>
#include <sstream>
#include <algorithm>
#include <cstring>   // Phase 6.2: strstr for sentinel detection
#include <cstdio>    // Phase 6.2: sscanf in OnOverrideObjectFinalized
#include <cstdlib>   // strtod for the ar_layer numeric parse
#include <cstdarg>  // va_list / va_start -- SweepReject's formatted refusal channel
#include <cerrno>    // ERANGE overflow detection for ar_layer values
#include <cmath>     // std::isfinite/sqrt/atan2/fabs (AllFiniteD, DirectionToEulerDeg, etc.) --
                     // only transitively available via ChunkDescriptor.h today; include directly
#include "../Materials/DielectricSPF.h"   // DielectricSPF::kMaxARLayers (ar_layer cap)
#include "../Materials/FabricPresets.h"   // fabric_material: the `fabric` enum's preset table (multi-slot seeding)
#include <sys/types.h>
#include <sys/stat.h>
#include "AsciiCommandParser.h"
#include "IAsciiChunkParser.h"
#include "ChunkParserRegistry.h"
#include "StdOutProgress.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/OrthonormalBasis3D.h"
#include "../Utilities/MediaPathLocator.h"
#include "MathExpressionEvaluator.h"
#include "../Utilities/RasterizerDefaults.h"
#include "../Utilities/Transformable.h"
#include "../Rendering/Film.h"		// kDefaultFilm* constants for `film` chunk
#include "../RISE_API.h"				// IScalarPainter constructors (Phase 2)
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/IScalarPainterManager.h"
#include "../Interfaces/IFunction1DManager.h"
#include "../Interfaces/IFunction2DManager.h"
#include "../Interfaces/IModifierManager.h"	// modifier chunks that self-register via IJobPriv::GetModifiers
#include "../Painters/RGBScalarPainter.h"		// for ScalarTriple::IsUniform et al
#include "../Painters/TexturePainter.h"		// for resolving a named image painter -> raster accessor (scalar_painter texture form)
#include "../Painters/ExpressionPainter.h"		// BuildExpressionProgramFromChunkFields (expression_painter, scalar_painter{expression})
#include "../Interfaces/IJobPriv.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/IObjectPriv.h"
#include "../Managers/GenericManager.h"			// g_cstFinalizeDiagSink -- specific-reason channel for a Finalize failure (reserved camera name)
// Phase B: descriptor-driven introspection used by
// PopulateLoadedPropertySnapshot to capture loaded parameter values.
#include "../SceneEditor/CameraIntrospection.h"
#include "../SceneEditor/LightIntrospection.h"
#include "../SceneEditor/MaterialIntrospection.h"
#include "../SceneEditor/MediaIntrospection.h"
#include "../SceneEditor/ObjectIntrospection.h"
#include "../Interfaces/ICameraManager.h"
#include "../Interfaces/ILightManager.h"
#include "../Interfaces/IMaterialManager.h"

#ifdef WIN32
#include <malloc.h>
#else
#include <alloca.h>
#endif

using namespace RISE;
using namespace RISE::Implementation;

// Shared file-scope helpers used by the chunk-parser registry.  (The
// legacy hal() QMC sequence `mh` lived ONLY in the deleted streaming TU,
// AsciiSceneParser.cpp, and was deleted with it in Slice 6c -- this TU
// never touched MultiHalton.)

// Max scene-file line length for the file-backed data-curve readers below
// (carried from the deleted streaming TU, AsciiSceneParser.cpp).
#define MAX_CHARS_PER_LINE		8192

// Read a data file into its meaningful lines, tolerant of comments and
// blank lines.  Skips blank / whitespace-only lines and lines whose first
// non-blank character is '#'.  Returns false (the caller logs) if the file
// cannot be opened.  Each returned line still contains its trailing
// newline; callers sscanf their own column format from it and validate the
// conversion count, so a malformed or non-numeric line is skipped rather
// than — as the historical `while( !feof(f) ) { fscanf(...); push; }` loops
// did — spinning forever on a comment line (fscanf matches nothing and does
// not advance) or pushing uninitialized / duplicated values at EOF.
static bool ReadDataFileLines( const String& filename, std::vector<std::string>& outLines )
{
	FILE* f = fopen( GlobalMediaPathLocator().Find( filename ).c_str(), "r" );
	if( !f ) {
		return false;
	}
	char buf[MAX_CHARS_PER_LINE];
	while( fgets( buf, sizeof( buf ), f ) ) {
		const char* p = buf;
		while( *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ) {
			++p;
		}
		if( *p == '\0' || *p == '#' ) {
			continue;   // blank / whitespace-only / comment line
		}
		outLines.push_back( std::string( buf ) );
	}
	fclose( f );
	return true;
}

inline bool string_split( const String& s, String& first, String& second, const char ch )
{
	String::const_iterator it = std::find( s.begin(), s.end(), ch );
	if( it==s.end() ) {
		return false;
	}

	first = String( s.begin(), it );
	second = String( it+1, s.end() );
	return true;
}

// Phase 6.2 helper (docs/ROUND_TRIP_SAVE_PLAN.md §8.10): compose a
// column-major 4×4 from translation, glTF-style quaternion (xyzw),
// and per-axis stretch.  Extracted from the inline body of
// StandardObjectAsciiChunkParser::Finalize (originally line 5534) so
// the new OverrideObjectAsciiChunkParser can call it.  Behaviour-
// preserving refactor: the math is identical.
inline void ComposeTRS_QuaternionGltf(
	const double pos[3], const double q[4], const double scale[3],
	double outM[16] )
{
	// Quaternion → 3×3 rotation, then assemble column-major M = T·R·S.
	const double xx = q[0]*q[0], yy = q[1]*q[1], zz = q[2]*q[2];
	const double xy = q[0]*q[1], xz = q[0]*q[2], yz = q[1]*q[2];
	const double wx = q[3]*q[0], wy = q[3]*q[1], wz = q[3]*q[2];
	const double r00 = 1.0 - 2.0*(yy+zz);
	const double r01 = 2.0*(xy - wz);
	const double r02 = 2.0*(xz + wy);
	const double r10 = 2.0*(xy + wz);
	const double r11 = 1.0 - 2.0*(xx+zz);
	const double r12 = 2.0*(yz - wx);
	const double r20 = 2.0*(xz - wy);
	const double r21 = 2.0*(yz + wx);
	const double r22 = 1.0 - 2.0*(xx+yy);

	// Column 0: scale[0] * R column 0
	outM[ 0] = scale[0] * r00;  outM[ 1] = scale[0] * r10;  outM[ 2] = scale[0] * r20;  outM[ 3] = 0;
	// Column 1: scale[1] * R column 1
	outM[ 4] = scale[1] * r01;  outM[ 5] = scale[1] * r11;  outM[ 6] = scale[1] * r21;  outM[ 7] = 0;
	// Column 2: scale[2] * R column 2
	outM[ 8] = scale[2] * r02;  outM[ 9] = scale[2] * r12;  outM[10] = scale[2] * r22;  outM[11] = 0;
	// Column 3: translation, w=1
	outM[12] = pos[0];          outM[13] = pos[1];          outM[14] = pos[2];          outM[15] = 1;
}

// Phase 6.2 helper: build a RISE Matrix4 from a column-major 16-double
// array — the same layout as glTF and as the existing
// `Job::AddObjectMatrix` consumer (Job.cpp:5103-5107).  Inline here so
// `OverrideObjectAsciiChunkParser` doesn't need to introduce a new
// public Matrix4 constructor.
inline Matrix4 BuildMatrix4FromColumnMajor( const double m[16] )
{
	Matrix4 out;
	out._00 = m[ 0]; out._01 = m[ 1]; out._02 = m[ 2]; out._03 = m[ 3];
	out._10 = m[ 4]; out._11 = m[ 5]; out._12 = m[ 6]; out._13 = m[ 7];
	out._20 = m[ 8]; out._21 = m[ 9]; out._22 = m[10]; out._23 = m[11];
	out._30 = m[12]; out._31 = m[13]; out._32 = m[14]; out._33 = m[15];
	return out;
}

//////////////////////////////////////////////////
// IAsciiChunkParser moved to IAsciiChunkParser.h
// Descriptor types in ChunkDescriptor.h
// Factory declared in ChunkParserRegistry.h
//////////////////////////////////////////////////

//////////////////////////////////////////////////
// Implementation of the different kinds of
//   chunk parsers
//////////////////////////////////////////////////

//////////////////////////////////////////////////
// Chunk breakdown:
//  Chunks are identified by name.  There are
//  two levels to chunks.  At the root level,
//  there are 8 primary chunks:
//    Geometry, Painter, Material, Object, Camera
//    PhotonMap, Rasterizer and Shader
//
//  Most of these primary chunks have subchunks.
//  For example each type of painter is a subchunk
//  The main painter chunk itself doesn't mean
//  anything without a subchunk
//
//  A note about parsing:  Each chunk MUST begin
//    with a '{' on its own line and MUST end
//    with a '}' on its own line.  The braces
//    and all comments will be automatically removed
//    by the primary parser before being passed
//    to the chunk parser
//////////////////////////////////////////////////

namespace RISE
{
	namespace Implementation
	{
		namespace ChunkParsers
		{
			// Adds the optional `variant` tag to a chunk descriptor (doc 63): when set, the chunk OVERRIDES its base
			// same-named counterpart while that scene_variant is active.  The chunk's own Finalize ignores `variant`
			// -- it is a marker the CST derive reads (bake-at-derive); the legacy reader skips a tagged material.
			static void AddVariantTagParam( ChunkDescriptor& cd )
			{
				cd.parameters.emplace_back();
				ParameterDescriptor& p = cd.parameters.back();
				p.name = "variant"; p.kind = ValueKind::String;
				p.description = "If set, this chunk overrides the base same-named chunk when scene_variant <name> is active.";
			}

			// Tracks uniform color painter values so that material parsers
			// can validate energy conservation at scene-definition time.
			struct PainterColor { double c[3]; };
			static thread_local std::map<std::string, PainterColor> s_painterColors;

			//! Resolve a scalar painter from a chunk parameter value.
			//!
			//! Accepts three input shapes:
			//!   - Inline scalar literal: `1.5` → UniformScalarPainter(1.5).
			//!   - Inline RGB-triple literal: `1.3 1.5 2.0` → RGBScalarPainter.
			//!   - Name of a registered scalar_painter: looked up in
			//!     `pJob.GetScalarPainters()`.
			//!
			//! Returns nullptr on lookup failure / unparseable value.  The
			//! returned painter is owned by the caller (refcount 1, must
			//! be released).  `requireSingle = true` causes a per-channel
			//! painter to be rejected with a clear error — used by single-
			//! scalar material slots (roughness, exponent, etc.).
			//!
			//! `paramName` is just for diagnostic messages.
			//!
			//! DEAD CODE (no callers; the live resolver is Job.cpp's
			//! ResolveScalarPainterArg).  If ever revived, its inline strtod
			//! path must gain the same non-finite rejection (nan/inf spellings +
			//! ERANGE overflow) the live path has, or it reintroduces the
			//! non-finite-scalar bug.  Kept only as a reference.
			[[maybe_unused]] static IScalarPainter* ResolveScalarPainter(
				IJob& pJob,
				const std::string& value,
				const char* paramName,
				bool requireSingle = false )
			{
				IJobPriv* pPriv = dynamic_cast<IJobPriv*>( &pJob );
				if( !pPriv ) return nullptr;

				// Try named-lookup first — the most common case in scenes
				// that already define a `scalar_painter` block.
				IScalarPainter* named =
					pPriv->GetScalarPainters()->GetItem( value.c_str() );
				if( named ) {
					if( requireSingle && named->HasPerChannelVariation() ) {
						GlobalLog()->PrintEx( eLog_Error,
							"scalar parameter `%s`: bound to per-channel painter `%s`, but slot requires a wavelength-uniform scalar",
							paramName, value.c_str() );
						return nullptr;
					}
					named->addref();
					return named;
				}

				// Inline numeric forms.  Use `strtod` end-pointer to
				// reject trailing non-whitespace — silent truncation
				// would let `1.5 garbage_name` parse as `1.5`, masking
				// typos in named-painter references.
				auto onlyTrailingWhitespace = []( const char* p ) -> bool {
					while( *p ) {
						if( ! std::isspace( static_cast<unsigned char>( *p ) ) ) return false;
						++p;
					}
					return true;
				};

				// 3-double first (more specific).
				{
					const char* s = value.c_str();
					char* end1 = nullptr;
					const double r = std::strtod( s, &end1 );
					if( end1 != s ) {
						char* end2 = nullptr;
						const double g = std::strtod( end1, &end2 );
						if( end2 != end1 ) {
							char* end3 = nullptr;
							const double b = std::strtod( end2, &end3 );
							if( end3 != end2 && onlyTrailingWhitespace( end3 ) ) {
								if( requireSingle ) {
									if( r == g && g == b ) {
										IScalarPainter* p = nullptr;
										RISE_API_CreateUniformScalarPainter( &p, Scalar( r ) );
										return p;
									}
									GlobalLog()->PrintEx( eLog_Error,
										"scalar parameter `%s`: inline triple (%g %g %g) supplied where a wavelength-uniform scalar is required",
										paramName, r, g, b );
									return nullptr;
								}
								IScalarPainter* p = nullptr;
								RISE_API_CreateRGBScalarPainter( &p,
									Scalar( r ), Scalar( g ), Scalar( b ) );
								return p;
							}
						}
					}
				}
				// 1-double next.
				{
					const char* s = value.c_str();
					char* end = nullptr;
					const double v = std::strtod( s, &end );
					if( end != s && onlyTrailingWhitespace( end ) ) {
						IScalarPainter* p = nullptr;
						RISE_API_CreateUniformScalarPainter( &p, Scalar( v ) );
						return p;
					}
				}

				GlobalLog()->PrintEx( eLog_Error,
					"scalar parameter `%s`: value `%s` is neither a named scalar_painter nor a numeric literal",
					paramName, value.c_str() );
				return nullptr;
			}

			// Scene-level camera defaults (set by `camera_defaults`
			// chunk).  thinlens_camera consults this as fallback when
			// a per-camera value is omitted.  Reset at the top of
			// every parse via ClearParseState; declaration order
			// inside the .RISEscene file matters — `camera_defaults`
			// must precede the camera chunks that consume it (the
			// natural reading order; same as `standard_shader` and
			// other scene-level config blocks).
			struct CameraDefaultsState {
				bool has_sensor_size  = false;  double sensor_size  = 0;
				bool has_focal_length = false;  double focal_length = 0;
				bool has_fstop        = false;  double fstop        = 0;
			};
			static thread_local CameraDefaultsState s_cameraDefaults;

			// Scene-level unit scale, set by `scene_options`.  Tells the
			// parser what one "scene unit" (geometry coordinates,
			// focus_distance, etc.) corresponds to in real-world
			// meters.  Default 1.0 = scenes are in meters.  Camera
			// lens-spec params (sensor_size, focal_length, shift_x/y)
			// are entered in MM in the scene file regardless of the
			// scene's unit scale; the camera reconciles mm-to-scene
			// internally via this factor.  This matches Cycles /
			// Arnold / V-Ray conventions where the user types
			// photographic numbers (35mm, f/2.8) and the renderer
			// scales them per scene-unit settings.
			struct SceneOptionsState {
				double scene_unit_meters = 1.0;   // 1 scene unit = N meters; default meters.
				// Set by `thinlens_camera::Finalize` once it commits a
				// camera using whatever scene_unit was current at that
				// moment.  If `scene_options` is then declared AFTER
				// the camera, the camera's scale is already locked in
				// and the new value is silently ignored — we warn at
				// parse time so a misplaced block is caught loudly
				// rather than producing a 1000×-misscaled render.
				bool   camera_committed  = false;
			};
			static thread_local SceneOptionsState s_sceneOptions;

			// Tracks every camera name issued across the whole top-level load (incl. recursive `> load`/`> run`
			// includes; reset only at top-level) so the
			// auto-name helper below doesn't collide with names the
			// user explicitly supplied.  Insertion is the parser's
			// responsibility — happens inside `AllocateCameraName`,
			// once per camera chunk, before the Add*Camera call.
			static thread_local std::set<std::string> s_cameraNamesUsed;

			// The runtime name AllocateCameraName issued for the MOST
			// RECENTLY finalized camera chunk.  HISTORICAL: the Phase-B
			// entity-index hook (AsciiSceneParser::OnEntityChunkFinalized,
			// deleted with the streaming loader in Slice 6c) read this to key
			// a NAME-OMITTED camera's SourceSpan under the SAME runtime name
			// the editor enumerates ("default", auto-suffixed) instead of
			// ExtractObjectName's "noname".  The SourceSpan entity index
			// itself went in Slice 6d; today's save (SaveEngine's whole-
			// Document SerializeCst) needs no such keying.  Still set on
			// every AllocateCameraName call; the only remaining reader is
			// the currently-unused public LastAllocatedCameraName accessor
			// (retained pending deletion).  thread_local for the same reason
			// as s_cameraNamesUsed.
			static thread_local std::string s_lastAllocatedCameraName;

			// Default name issued to an unnamed camera chunk.  The
			// design treats `name` as optional; first unnamed camera
			// gets "default", second "default_1", and so on.  An
			// explicit `name "default"` followed by an unnamed camera
			// also auto-suffixes — that's what the s_cameraNamesUsed
			// set is for.
			static std::string AllocateCameraNameImpl( const std::string& requested ) {
				if( !requested.empty() ) {
					s_cameraNamesUsed.insert( requested );
					return requested;
				}
				if( s_cameraNamesUsed.find( "default" ) == s_cameraNamesUsed.end() ) {
					s_cameraNamesUsed.insert( "default" );
					return "default";
				}
				for( int i = 1; i < 1000; ++i ) {
					char buf[64];
					std::snprintf( buf, sizeof(buf), "default_%d", i );
					std::string candidate = buf;
					if( s_cameraNamesUsed.find( candidate ) == s_cameraNamesUsed.end() ) {
						s_cameraNamesUsed.insert( candidate );
						return candidate;
					}
				}
				return std::string( "default_overflow" );
			}

			// Capturing wrapper around the (unchanged) allocator: every
			// camera Finalize calls this, so s_lastAllocatedCameraName always
			// holds the runtime name of the most recently finalized camera.
			static std::string AllocateCameraName( const std::string& requested ) {
				s_lastAllocatedCameraName = AllocateCameraNameImpl( requested );
				return s_lastAllocatedCameraName;
			}

			// Read-only accessor: the runtime name issued to the most
			// recently finalized camera chunk.  Its consumer -- the Phase-B
			// entity-index hook (AsciiSceneParser::OnEntityChunkFinalized) --
			// was deleted with the streaming loader in Slice 6c; today the
			// only caller is the public LastAllocatedCameraName wrapper,
			// which itself has no callers.  Retained pending deletion.
			static const std::string& LastAllocatedCameraName() {
				return s_lastAllocatedCameraName;
			}

			// `none` is the scene language's universal unbind sentinel, and
			// Job::AddKeyframeToAnimation's camera branch specifically treats
			// element=="none" (or an empty element) as "target the ACTIVE
			// camera" -- see that function's comment in Job.cpp.  A
			// hand-authored scene naming a camera `name "none"` would create
			// a camera that a `timeline`/`keyframe` chunk could never target
			// by name (it would always silently fall back to the active
			// camera instead).  Job::ApplyCstInsertChunk already refuses
			// `none` as a chunk name for AGENT-driven inserts, but that gate
			// sits above the parser and never runs for a scene file parsed/
			// derived directly -- so the refusal has to be repeated here, at
			// the point every camera chunk's Finalize allocates its runtime
			// name, to close the hand-authored-scene gap.  Every camera
			// Finalize must call this BEFORE AllocateCameraName and bail out
			// (return false) if it returns true.  Wording matches
			// Job::ApplyCstInsertChunk's "reserved name" diagnostic so the
			// two refusal paths read as one policy to scene authors.
			static bool RejectReservedCameraName( const std::string& keyword, const std::string& requestedName ) {
				if( requestedName != "none" ) {
					return false;
				}
				if( RISE::g_cstFinalizeDiagSink ) {
					*RISE::g_cstFinalizeDiagSink =
						"reserved name: `none` is the built-in unbind sentinel -- pick a different camera name";
				}
				GlobalLog()->PrintEx( eLog_Error,
					"%s:: `none` is a reserved name (the active-camera / unbind sentinel used by timeline `element`) -- pick a different camera name",
					keyword.c_str() );
				return true;
			}

			// Per-parse reset.  Sole live caller: Cst::DeriveToJob (Cst/Cst.cpp), via the public
			// ClearChunkParserState wrapper, always with the default resetTopLevelState=true.
			// resetTopLevelState gates the reset of state that accumulates ACROSS a top-level build to TOP-LEVEL
			// parses only.  HISTORICAL: the deleted streaming loader (ParseAndLoadScene, Slice 6c) passed false
			// on a nested `> load`/`> run` parse -- camera auto-naming had to stay unique manager-wide ACROSS
			// includes, so a nested parse must not wipe the outer scene's allocated names (else a child's unnamed
			// camera re-allocates "default" and Scene::AddCamera rejects the duplicate).  No caller passes false
			// today; the parameter is retained pending deletion.  The other caches reset on every call.  (The
			// legacy hal() QMC sequence lived in the deleted streaming TU and went with it -- never this TU's
			// concern.)
			static void ClearParseState( bool resetTopLevelState = true ) {
				s_painterColors.clear();
				s_cameraDefaults = CameraDefaultsState();
				s_sceneOptions   = SceneOptionsState();
				if( resetTopLevelState ) {
					s_cameraNamesUsed.clear();
					s_lastAllocatedCameraName.clear();
				}
			}

			// Generic dispatch used by migrated chunk parsers to replace the
			// Generic registry-driven dispatcher.  Walks the input
			// parameter lines, validates each name against the chunk's
			// ChunkDescriptor::parameters, and stores matched values
			// in the bag (single-valued or repeatable depending on
			// the descriptor's `repeatable` flag).  Unknown parameter
			// names fail the parse — exactly the same behaviour as
			// the legacy hand-rolled if/else chain's else branch.
			//
			// Because the bag is keyed by parameter name and the only
			// gate on what gets stored is the descriptor, the
			// descriptor IS the parser's accepted-parameter set.
			// Drift between "what the parser parses" and "what the
			// descriptor advertises" is structurally impossible: if
			// the descriptor lists a parameter, the parser accepts it
			// and Finalize sees it; if it doesn't, the parser rejects
			// it.  Each parser's Finalize then reads typed values out
			// of the bag and emits the corresponding pJob.AddX call.
			// TEXT-domain numeric validation for Double/UInt-kind parameter
			// values: every whitespace-separated token must fully consume as
			// a number, and `nan` / `inf` spellings are rejected by TEXT
			// before strtod ever runs.  This is deliberately not a value
			// check (std::isfinite / !(x>0) nets) -- the build uses
			// -ffast-math (finite-math-only), under which inf/NaN VALUE
			// comparisons are undefined and really do get folded away (see
			// the guilloché dial inf-seed miscompile, 2026-06-11).  Scene
			// text is the one layer where the rejection cannot be optimized
			// out.  Also closes String::toDouble's uninitialized return on
			// entirely non-numeric tokens.
			// On an ERANGE strtod result, classify the consumed token [tok,end) as
			// UNDERFLOW (-> finite 0/subnormal) vs OVERFLOW (-> non-finite HUGE_VAL)
			// at the STRING layer.  An exponent-SIGN heuristic alone is spoofable --
			// a 320-digit mantissa with `e-1` still overflows to +inf yet carries
			// `e-` in the token -- so compute the SIGN of the token's net magnitude:
			// the order of the leading significant digit plus the explicit exponent
			// (decimal digits count in powers of 10; C99 hex-float mantissa digits
			// count 4 bits each against the BINARY `p` exponent).  ERANGE guarantees
			// the value is hundreds of orders outside [DBL_MIN, DBL_MAX], so the +-3
			// bit slop of the leading hex digit can never flip the sign.
			// KEEP IN LOCKSTEP with the identical helper in Job.cpp.
			static bool ERangeTokenIsUnderflow( const char* tok, const char* end )
			{
				const char* p = ( tok < end && ( *tok == '+' || *tok == '-' ) ) ? tok + 1 : tok;
				bool isHex = false;
				if( end - p > 1 && p[0] == '0' && ( p[1] == 'x' || p[1] == 'X' ) ) { isHex = true; p += 2; }
				long order = 0;                    // order of the leading significant digit
				bool seenSig = false, seenPoint = false;
				long intDigits = 0, fracZeros = 0;
				for( ; p < end; ++p ) {
					const char c = *p;
					if( c == '.' && !seenPoint ) { seenPoint = true; continue; }
					const bool dig = isHex ? !!isxdigit( (unsigned char)c ) : !!isdigit( (unsigned char)c );
					if( !dig ) break;                       // exponent marker
					if( !seenSig ) {
						if( c == '0' ) { if( seenPoint ) ++fracZeros; continue; }
						seenSig = true;
						if( !seenPoint ) intDigits = 1; else order = -( fracZeros + 1 );
					} else if( !seenPoint ) {
						++intDigits;
					}
				}
				if( !seenSig ) return true;        // literal zero never ERANGEs; harmless
				if( intDigits > 0 ) order = intDigits - 1;
				long expVal = 0;
				if( p < end && ( ( !isHex && ( *p == 'e' || *p == 'E' ) ) || ( isHex && ( *p == 'p' || *p == 'P' ) ) ) ) {
					++p;
					long sign = 1;
					if( p < end && ( *p == '+' || *p == '-' ) ) { if( *p == '-' ) sign = -1; ++p; }
					long v = 0;
					for( ; p < end && isdigit( (unsigned char)*p ); ++p ) { if( v < 100000000L ) v = v * 10 + ( *p - '0' ); }
					expVal = sign * v;
				}
				const long net = isHex ? ( 4 * order + expVal ) : ( order + expVal );
				return net < 0;
			}

			//! EVERY sweep_geometry refusal goes through this ONE helper so
			//! the author's specific reason reaches BOTH the log and the CST
			//! Finalize diagnostic sink -- the same channel lathe_geometry's
			//! `Reject` and skeleton_geometry's already use.  Without it a
			//! chunk-level refusal (a malformed point_scale arity, a
			//! point_morph with no second profile, the profile2 trio's mutual
			//! exclusion) surfaced to the author, and to the agent surface
			//! reading those diagnostics, as the generic "sweep_geometry:
			//! apply failed (e.g. unresolved reference); see log" -- actively
			//! misleading, since no reference is involved.  Call sites keep
			//! their printf form, so the message texts are unchanged.
			inline bool SweepReject( const char* fmt, ... )
			{
				char buf[1024];
				va_list ap;
				va_start( ap, fmt );
				vsnprintf( buf, sizeof(buf), fmt, ap );
				va_end( ap );
				if( RISE::g_cstFinalizeDiagSink ) *RISE::g_cstFinalizeDiagSink = buf;
				GlobalLog()->Print( eLog_Error, buf );
				return false;
			}

			inline bool AllTokensAreFiniteNumbers( const char* sz, int* outTokenCount = 0 )
			{
				if( outTokenCount ) *outTokenCount = 0;
				if( !sz ) {
					return false;
				}
				int tokens = 0;
				const char* p = sz;
				while( *p ) {
					while( *p == ' ' || *p == '\t' ) {
						++p;
					}
					if( !*p ) {
						break;
					}
					const char* tok = p;
					if( *tok == '#' ) {
						break;	// inline trailing comment ends the value (legacy-tolerated idiom: `sensor_size 36 # mm`)
					}
					const char* t = ( *tok == '+' || *tok == '-' ) ? tok + 1 : tok;
					if( t[0] == 'n' || t[0] == 'N' || t[0] == 'i' || t[0] == 'I' ) {
						return false;	// nan / inf / infinity spellings
					}
					char* end = 0;
					errno = 0;
					strtod( tok, &end );
					if( errno == ERANGE ) {
						// ERANGE covers overflow (-> non-finite HUGE_VAL) AND underflow
						// (-> finite 0/subnormal, e.g. 5e-400).  Only overflow is
						// non-finite; classify at the STRING layer by net-magnitude
						// sign (exponent-sign alone is spoofable -- see the helper).
						if( !ERangeTokenIsUnderflow( tok, end ) ) {
							return false;	// overflow (e.g. 1e999 -> HUGE_VAL); reject at the STRING layer (value-level isfinite was unreliable under bare -ffast-math (fixed 2026-07-29; the string layer is still preferred)), mirroring the ar_layer parser
						}
					}
					if( end == tok ) {
						return false;	// token does not start as a number
					}
					if( *end != '\0' && *end != ' ' && *end != '\t' && *end != '#' ) {
						return false;	// trailing garbage glued to the number
					}
					++tokens;
					p = end;
					if( *p == '#' ) {
						break;	// number glued to a comment (`36#mm`) -- legacy sscanf accepted it
					}
				}
				if( outTokenCount ) *outTokenCount = tokens;
				return tokens > 0;
			}

			inline bool HasExactNumericArity(
				const ParseStateBag& bag, const char* key, const int expected )
			{
				if( !bag.Has( key ) ) return true;
				int actual = 0;
				return AllTokensAreFiniteNumbers( bag.GetString( key ).c_str(), &actual )
					&& actual == expected;
			}

			inline bool DispatchChunkParameters(
				const ChunkDescriptor& desc,
				ParseStateBag&         bag,
				const IAsciiChunkParser::ParamsList& params )
			{
				for( IAsciiChunkParser::ParamsList::const_iterator i = params.begin(); i != params.end(); ++i ) {
					String pname;
					String pvalue;
					if( !string_split( *i, pname, pvalue, ' ' ) ) {
						return false;
					}

					const ParameterDescriptor* found = 0;
					for( std::vector<ParameterDescriptor>::const_iterator p = desc.parameters.begin(); p != desc.parameters.end(); ++p ) {
						if( p->name == std::string(pname.c_str()) ) {
							found = &(*p);
							break;
						}
					}

					if( !found ) {
						GlobalLog()->PrintEx( eLog_Error,
							kUndeclaredParameterFmt,
							pname.c_str(),
							desc.keyword.empty() ? "(unknown)" : desc.keyword.c_str() );
						return false;
					}

					switch( found->kind ) {
					case ValueKind::Double:
					case ValueKind::DoubleVec3:
					case ValueKind::DoubleVec4:
					case ValueKind::DoubleMat4:
					case ValueKind::UInt:
						if( !AllTokensAreFiniteNumbers( pvalue.c_str() ) ) {
							GlobalLog()->PrintEx( eLog_Error,
								"ChunkParser:: parameter `%s` in `%s` expects finite numeric value(s); got `%s` (nan/inf and non-numeric tokens are rejected)",
								pname.c_str(),
								desc.keyword.empty() ? "(unknown)" : desc.keyword.c_str(),
								pvalue.c_str() );
							return false;
						}
						break;
					default:
						break;
					}

					if( found->repeatable ) {
						bag.AppendRepeatable( found->name, std::string(pvalue.c_str()) );
					} else {
						bag.SetSingle( found->name, std::string(pvalue.c_str()) );
					}
				}
				return true;
			}

			//////////////////////////////////////////
			// Descriptor helpers — shared parameter groups used by
			// chunk-parser Describe() implementations below.  Defined
			// here so every chunk parser that uses them sees a complete
			// declaration before the call site.
			//////////////////////////////////////////

			// Per-enum to_hint specialisations for the parser-internal
			// enums whose definitions live in headers ChunkDescriptor.h
			// shouldn't pull in.  Match the lowercase string the parser
			// actually accepts on the input side.  The using-declaration
			// re-introduces the namespace-RISE to_hint overloads (bool,
			// double, unsigned int, OidnQuality, ...) so they participate
			// in overload resolution alongside these local additions —
			// without the using, name lookup would stop at the first
			// match in this nested namespace and the namespace-level
			// overloads would be hidden.
			using RISE::to_hint;
			static inline std::string to_hint( GuidingSamplingType v ) {
				return v == eGuidingRIS ? "RIS" : "OneSampleMIS";
			}
			static inline std::string to_hint( SMSSeedingMode v ) {
				return v == SMSSeedingMode::Uniform ? "uniform" : "snell";
			}
			static inline std::string to_hint( AutoIntegratorChoice v ) {
				switch( v ) {
					case AutoIntegratorChoice::PT:   return "pt";
					case AutoIntegratorChoice::BDPT: return "bdpt";
					case AutoIntegratorChoice::VCM:  return "vcm";
					case AutoIntegratorChoice::Auto:
					default:                         return "auto";
				}
			}

			// Optional rasterizer params accepted only by a subset.  Hints
			// derive from StabilityConfig defaults so this helper has one
			// source of truth shared with the parser-side `bag.GetX()`
			// fallbacks (which all use `if(bag.Has)` against a default-
			// constructed StabilityConfig).
			template<typename PushFn>
			static void AddOptimalMISParams( PushFn P ) {
				StabilityConfig d;
				{ auto& p = P(); p.name = "optimal_mis";                     p.kind = ValueKind::Bool; p.description = "Enable optimal MIS";                  p.defaultValueHint = to_hint(d.optimalMIS); }
				{ auto& p = P(); p.name = "optimal_mis_training_iterations"; p.kind = ValueKind::UInt; p.description = "Optimal-MIS training iterations";    p.defaultValueHint = to_hint(d.optimalMISTrainingIterations); }
				{ auto& p = P(); p.name = "optimal_mis_tile_size";           p.kind = ValueKind::UInt; p.description = "Optimal-MIS tile size";              p.defaultValueHint = to_hint(d.optimalMISTileSize); }
			}

			// StabilityConfig params accepted by all non-MLT rasterizers.
			template<typename PushFn>
			static void AddStabilityConfigParams( PushFn P ) {
				StabilityConfig d;
				{ auto& p = P(); p.name = "direct_clamp";                           p.kind = ValueKind::Double; p.description = "Clamp on direct-lighting contribution (0 disables)"; p.defaultValueHint = to_hint(d.directClamp); }
				{ auto& p = P(); p.name = "indirect_clamp";                         p.kind = ValueKind::Double; p.description = "Clamp on indirect contribution (0 disables)";         p.defaultValueHint = to_hint(d.indirectClamp); }
				{ auto& p = P(); p.name = "rr_min_depth";                           p.kind = ValueKind::UInt;   p.description = "Min depth before Russian roulette";     p.defaultValueHint = to_hint(d.rrMinDepth); }
				{ auto& p = P(); p.name = "rr_threshold";                           p.kind = ValueKind::Double; p.description = "Throughput threshold for RR";           p.defaultValueHint = to_hint(d.rrThreshold); }
				{ auto& p = P(); p.name = "max_diffuse_bounce";                     p.kind = ValueKind::UInt;   p.description = "Max diffuse bounce depth (UINT_MAX = unlimited)";              p.defaultValueHint = to_hint(d.maxDiffuseBounce); }
				{ auto& p = P(); p.name = "max_glossy_bounce";                      p.kind = ValueKind::UInt;   p.description = "Max glossy bounce depth (UINT_MAX = unlimited)";               p.defaultValueHint = to_hint(d.maxGlossyBounce); }
				{ auto& p = P(); p.name = "max_transmission_bounce";                p.kind = ValueKind::UInt;   p.description = "Max transmission bounce depth (UINT_MAX = unlimited)";         p.defaultValueHint = to_hint(d.maxTransmissionBounce); }
				{ auto& p = P(); p.name = "max_translucent_bounce";                 p.kind = ValueKind::UInt;   p.description = "Max translucent bounce depth (UINT_MAX = unlimited)";          p.defaultValueHint = to_hint(d.maxTranslucentBounce); }
				{ auto& p = P(); p.name = "max_volume_bounce";                      p.kind = ValueKind::UInt;   p.description = "Max volume bounce depth";               p.defaultValueHint = to_hint(d.maxVolumeBounce); }
				{ auto& p = P(); p.name = "light_bvh";                              p.kind = ValueKind::Bool;   p.description = "Use a BVH over lights for NEE";         p.defaultValueHint = to_hint(d.useLightBVH); }
			}
			template<typename PushFn>
			static void AddPathGuidingParams( PushFn P ) {
				PathGuidingConfig d;
				{ auto& p = P(); p.name = "pathguiding";                            p.kind = ValueKind::Bool;   p.description = "Enable path guiding";                   p.defaultValueHint = to_hint(d.enabled); }
				{ auto& p = P(); p.name = "pathguiding_iterations";                 p.kind = ValueKind::UInt;   p.description = "Training iterations";                   p.defaultValueHint = to_hint(d.trainingIterations); }
				{ auto& p = P(); p.name = "pathguiding_spp";                        p.kind = ValueKind::UInt;   p.description = "Samples per pixel during training";     p.defaultValueHint = to_hint(d.trainingSPP); }
				{ auto& p = P(); p.name = "pathguiding_combine_training";           p.kind = ValueKind::Bool;   p.description = "Accumulate training-iteration pixels into the final image weighted by SPP (Müller 2017 §5).  Off = legacy discard behaviour."; p.defaultValueHint = to_hint(d.combineTrainingIterations); }
				{ auto& p = P(); p.name = "pathguiding_online";                     p.kind = ValueKind::Bool;   p.description = "Training-iteration loop is the entire render; no separate final pass.  Best for low-SPP regimes (Vorba/NASG style)."; p.defaultValueHint = to_hint(d.online); }
				{ auto& p = P(); p.name = "pathguiding_warmup_iterations";          p.kind = ValueKind::UInt;   p.description = "First N training iterations render with alpha=0 (unguided) so their pixels are unbiased even when combine/online is on.  Samples still feed the field.  Default 1 keeps the empty-field iteration's pixels clean; raise to 2 in online mode."; p.defaultValueHint = to_hint(d.warmupIterations); }
				{ auto& p = P(); p.name = "pathguiding_alpha";                      p.kind = ValueKind::Double; p.description = "Mixing factor with BSDF sampling";      p.defaultValueHint = to_hint(d.alpha); }
				{ auto& p = P(); p.name = "pathguiding_learned_alpha";              p.kind = ValueKind::Bool;   p.description = "Per-cell Adam-learned mixing alpha (Müller 2017 v2); modest win at SPP >= 256, neutral at low SPP";  p.defaultValueHint = to_hint(d.learnedAlpha); }
				{ auto& p = P(); p.name = "pathguiding_max_depth";                  p.kind = ValueKind::UInt;   p.description = "Max eye-subpath depth to apply guiding (matches typical scene max_eye_depth)"; p.defaultValueHint = to_hint(d.maxGuidingDepth); }
				{ auto& p = P(); p.name = "pathguiding_light_max_depth";            p.kind = ValueKind::UInt;   p.description = "Max light-subpath depth for guiding (BDPT only); 0 disables (separate field, additional training cost)"; p.defaultValueHint = to_hint(d.maxLightGuidingDepth); }
				{ auto& p = P(); p.name = "pathguiding_sampling_type";              p.kind = ValueKind::Enum;   p.enumValues = {"ris","RIS","OneSampleMIS"}; p.description = "Sampling strategy (any string other than ris/RIS selects OneSampleMIS)";  p.defaultValueHint = to_hint(d.samplingType); }
				{ auto& p = P(); p.name = "pathguiding_ris_candidates";             p.kind = ValueKind::UInt;   p.description = "RIS candidate count (only N=2 currently implemented; values >2 reserved for future)"; p.defaultValueHint = to_hint(d.risCandidates); }
				{ auto& p = P(); p.name = "pathguiding_complete_paths";             p.kind = ValueKind::Bool;   p.description = "Enable complete-path guiding (experimental, BDPT)"; p.defaultValueHint = to_hint(d.completePathGuiding); }
				{ auto& p = P(); p.name = "pathguiding_complete_path_strategy_selection"; p.kind = ValueKind::Bool; p.description = "Enable complete-path strategy selection (experimental)"; p.defaultValueHint = to_hint(d.completePathStrategySelection); }
				{ auto& p = P(); p.name = "pathguiding_complete_path_strategy_samples";   p.kind = ValueKind::UInt; p.description = "Techniques to evaluate per path when strategy selection is on"; p.defaultValueHint = to_hint(d.completePathStrategySamples); }
			}
			template<typename PushFn>
			static void AddAdaptiveSamplingParams( PushFn P ) {
				AdaptiveSamplingConfig d;
				{ auto& p = P(); p.name = "adaptive_max_samples";                   p.kind = ValueKind::UInt;   p.description = "Max adaptive samples per pixel (0 disables)";        p.defaultValueHint = to_hint(d.maxSamples); }
				{ auto& p = P(); p.name = "adaptive_threshold";                     p.kind = ValueKind::Double; p.description = "Relative-error threshold";              p.defaultValueHint = to_hint(d.threshold); }
				{ auto& p = P(); p.name = "show_adaptive_map";                      p.kind = ValueKind::Bool;   p.description = "Visualize the adaptive sample map";     p.defaultValueHint = to_hint(d.showMap); }
			}
			template<typename PushFn>
			static void AddPixelFilterParams( PushFn P ) {
				PixelFilterConfig d;
				{ auto& p = P(); p.name = "pixel_sampler";                          p.kind = ValueKind::String; p.description = "Pixel sampler strategy";                p.defaultValueHint = to_hint(std::string(d.pixelSampler.c_str())); }
				{ auto& p = P(); p.name = "pixel_sampler_param";                    p.kind = ValueKind::Double; p.description = "Sampler-specific parameter";            p.defaultValueHint = to_hint(d.pixelSamplerParam); }
				{ auto& p = P(); p.name = "pixel_filter";                           p.kind = ValueKind::String; p.description = "Reconstruction filter";                 p.defaultValueHint = to_hint(std::string(d.filter.c_str())); }
				{ auto& p = P(); p.name = "pixel_filter_width";                     p.kind = ValueKind::Double; p.description = "Filter width";                          p.defaultValueHint = to_hint(d.width); }
				{ auto& p = P(); p.name = "pixel_filter_height";                    p.kind = ValueKind::Double; p.description = "Filter height";                         p.defaultValueHint = to_hint(d.height); }
				{ auto& p = P(); p.name = "pixel_filter_paramA";                    p.kind = ValueKind::Double; p.description = "Filter paramA";                         p.defaultValueHint = to_hint(d.paramA); }
				{ auto& p = P(); p.name = "pixel_filter_paramB";                    p.kind = ValueKind::Double; p.description = "Filter paramB";                         p.defaultValueHint = to_hint(d.paramB); }
				{ auto& p = P(); p.name = "blue_noise_sampler";                     p.kind = ValueKind::Bool;   p.description = "Use blue-noise sampler";                p.defaultValueHint = to_hint(d.blueNoiseSampler); }
			}
			template<typename PushFn>
			static void AddRadianceMapParams( PushFn P ) {
				RadianceMapConfig d;
				{ auto& p = P(); p.name = "radiance_map";                           p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Environment radiance painter"; }
				{ auto& p = P(); p.name = "radiance_scale";                         p.kind = ValueKind::Double; p.description = "Scale applied to radiance map";         p.defaultValueHint = to_hint(d.scale); }
				{ auto& p = P(); p.name = "radiance_background";                    p.kind = ValueKind::Bool;   p.description = "Also use as camera background";         p.defaultValueHint = to_hint(d.isBackground); }
				{ auto& p = P(); p.name = "radiance_orient";                        p.kind = ValueKind::DoubleVec3; p.description = "Rotation (degrees) X Y Z";           p.defaultValueHint = "0 0 0"; }
			}
			template<typename PushFn>
			static void AddProgressiveParams( PushFn P ) {
				ProgressiveConfig d;
				{ auto& p = P(); p.name = "progressive_rendering";                  p.kind = ValueKind::Bool;   p.description = "Enable progressive rendering";          p.defaultValueHint = to_hint(d.enabled); }
				{ auto& p = P(); p.name = "progressive_samples_per_pass";           p.kind = ValueKind::UInt;   p.description = "Samples per progressive pass";          p.defaultValueHint = to_hint(d.samplesPerPass); }
			}
			// Core spectral params — exactly the fields of SpectralConfig.
			// Used by every spectral rasterizer (pixelintegratingspectral,
			// PT/BDPT/VCM spectral, MLT spectral).
			template<typename PushFn>
			static void AddSpectralCoreParams( PushFn P ) {
				SpectralConfig d;
				{ auto& p = P(); p.name = "spectral_samples";  p.kind = ValueKind::UInt;   p.description = "Number of spectral samples per pixel";       p.defaultValueHint = to_hint(d.spectralSamples); }
				{ auto& p = P(); p.name = "nmbegin";           p.kind = ValueKind::Double; p.description = "Start wavelength (nm)";                      p.defaultValueHint = to_hint(d.nmBegin); }
				{ auto& p = P(); p.name = "nmend";             p.kind = ValueKind::Double; p.description = "End wavelength (nm)";                        p.defaultValueHint = to_hint(d.nmEnd); }
				{ auto& p = P(); p.name = "num_wavelengths";   p.kind = ValueKind::UInt;   p.description = "Discrete wavelengths sampled";               p.defaultValueHint = to_hint(d.numWavelengths); }
				{ auto& p = P(); p.name = "hwss";              p.kind = ValueKind::Bool;   p.description = "Enable hero-wavelength stratified sampling"; p.defaultValueHint = to_hint(d.useHWSS); }
			}

			// RGB-to-SPD conversion params — only the legacy
			// pixelintegratingspectral_rasterizer accepts these.  Modern
			// PT/BDPT/VCM/MLT spectral integrators do RGB-to-SPD via
			// the painters pipeline.
			template<typename PushFn>
			static void AddSpectralRGBSpdParams( PushFn P ) {
				{ auto& p = P(); p.name = "integrate_rgb";       p.kind = ValueKind::Bool;   p.description = "Integrate directly to RGB (skip spectral storage)"; p.defaultValueHint = "FALSE"; }
				{ auto& p = P(); p.name = "rgb_spd";             p.kind = ValueKind::String; p.description = "RGB-to-SPD conversion type";            p.defaultValueHint = "smits"; }
				{ auto& p = P(); p.name = "rgb_spd_wavelengths"; p.kind = ValueKind::String; p.description = "Wavelengths for custom RGB-SPD tables";  p.defaultValueHint = ""; }
				{ auto& p = P(); p.name = "rgb_spd_r";           p.kind = ValueKind::String; p.description = "Red channel SPD samples";               p.defaultValueHint = ""; }
				{ auto& p = P(); p.name = "rgb_spd_g";           p.kind = ValueKind::String; p.description = "Green channel SPD samples";             p.defaultValueHint = ""; }
				{ auto& p = P(); p.name = "rgb_spd_b";           p.kind = ValueKind::String; p.description = "Blue channel SPD samples";              p.defaultValueHint = ""; }
			}

			// Backwards-compat alias — every existing call site that
			// said AddSpectralConfigParams meant "everything spectral",
			// which was the over-broad bundle.  Now resolves to core +
			// RGB-SPD.  New code should use AddSpectralCoreParams /
			// AddSpectralRGBSpdParams directly so the descriptor
			// matches what the parser actually consumes.
			template<typename PushFn>
			static void AddSpectralConfigParams( PushFn P ) {
				AddSpectralCoreParams( P );
				AddSpectralRGBSpdParams( P );
			}
			// Strip surrounding ASCII double-quotes from a string value
			// produced by `ParseStateBag::GetString`.  RISE's scene-language
			// convention is bare identifiers for these enum-style string
			// parameters (`pixel_filter box`, no quotes), but users
			// frequently write the quoted form (`sms_seeding "uniform"`)
			// because it looks more like normal config syntax — and the
			// bag preserves quotes verbatim.  Without this normalisation,
			// the raw `GetString` return for `sms_seeding "uniform"` is
			// the 10-character sequence `"uniform"` (the quotes are part
			// of the string), which never compares equal to the literal
			// `"uniform"` (8 chars) and silently falls through to the
			// "snell" fallback.  Found while debugging an abandoned
			// prototype; the underlying bug is older and applies wherever
			// an enum-as-string is parsed, but only `sms_seeding` is
			// fixed here — extend to other sites as they're discovered
			// or as part of a broader parser audit.  Idempotent: bare
			// values are returned unchanged.
			static std::string StripSurroundingQuotes( const std::string& s ) {
				if( s.size() >= 2 && s.front() == '"' && s.back() == '"' ) {
					return s.substr( 1, s.size() - 2 );
				}
				return s;
			}

			template<typename PushFn>
			static void AddSMSConfigParams( PushFn P ) {
				SMSConfig d;
				{ auto& p = P(); p.name = "sms_enabled";                            p.kind = ValueKind::Bool;   p.description = "Enable Specular Manifold Sampling";     p.defaultValueHint = to_hint(d.enabled); }
				{ auto& p = P(); p.name = "sms_max_iterations";                     p.kind = ValueKind::UInt;   p.description = "Max SMS Newton iterations";             p.defaultValueHint = to_hint(d.maxIterations); }
				{ auto& p = P(); p.name = "sms_threshold";                          p.kind = ValueKind::Double; p.description = "SMS convergence threshold";             p.defaultValueHint = to_hint(d.threshold); }
				{ auto& p = P(); p.name = "sms_max_chain_depth";                    p.kind = ValueKind::UInt;   p.description = "Max SMS manifold-chain depth (recommend setting to natural caustic K, typically 2)"; p.defaultValueHint = to_hint(d.maxChainDepth); }
				{ auto& p = P(); p.name = "sms_biased";                             p.kind = ValueKind::Bool;   p.description = "Use biased SMS estimator";              p.defaultValueHint = to_hint(d.biased); }
				{ auto& p = P(); p.name = "sms_bernoulli_trials";                   p.kind = ValueKind::UInt;   p.description = "Bernoulli trials per vertex (only used when sms_biased=FALSE)"; p.defaultValueHint = to_hint(d.bernoulliTrials); }
				{ auto& p = P(); p.name = "sms_multi_trials";                       p.kind = ValueKind::UInt;   p.description = "Multi-trials per vertex";               p.defaultValueHint = to_hint(d.multiTrials); }
				{ auto& p = P(); p.name = "sms_photon_count";                       p.kind = ValueKind::UInt;   p.description = "SMS photon budget (0 disables; recommend 10000 for diacaustic / mirror-chain scenes)"; p.defaultValueHint = to_hint(d.photonCount); }
				{ auto& p = P(); p.name = "sms_two_stage";                          p.kind = ValueKind::Bool;   p.description = "Two-stage solver: smooth seed then refine on actual surface (Zeltner 2020 §5)"; p.defaultValueHint = to_hint(d.twoStage); }
				{ auto& p = P(); p.name = "sms_seeding";                            p.kind = ValueKind::String; p.description = "SMS seeding strategy: \"snell\" (legacy Snell-trace) or \"uniform\" (Mitsuba-faithful uniform-on-shape)"; p.defaultValueHint = to_hint(d.seedingMode); }
				{ auto& p = P(); p.name = "sms_target_bounces";                     p.kind = ValueKind::UInt;   p.description = "REQUIRED specular-vertex count per seed chain (Mitsuba `m_config.bounces` analogue).  0 = no target.  Set to natural caustic K (typically 2 for glass shells / interior lights).  Active in BOTH snell and uniform modes; recommended for uniform mode."; p.defaultValueHint = to_hint(d.targetBounces); }
			}
			template<typename PushFn>
			static void AddPhotonMapGenerateCommonParams( PushFn P ) {
				{ auto& p = P(); p.name = "num";                      p.kind = ValueKind::UInt;   p.description = "Photon count to shoot";                     p.defaultValueHint = "10000"; }
				{ auto& p = P(); p.name = "power_scale";              p.kind = ValueKind::Double; p.description = "Photon power multiplier";                   p.defaultValueHint = "1.0"; }
				{ auto& p = P(); p.name = "max_recursion";            p.kind = ValueKind::UInt;   p.description = "Max photon scattering depth";               p.defaultValueHint = "10"; }
				{ auto& p = P(); p.name = "min_importance";           p.kind = ValueKind::Double; p.description = "Photon-throughput cutoff";                  p.defaultValueHint = "0.01"; }
				{ auto& p = P(); p.name = "branch";                   p.kind = ValueKind::Bool;   p.description = "Branch at dielectric splits";               p.defaultValueHint = "TRUE"; }
				{ auto& p = P(); p.name = "reflect";                  p.kind = ValueKind::Bool;   p.description = "Trace reflected photons";                   p.defaultValueHint = "TRUE"; }
				{ auto& p = P(); p.name = "refract";                  p.kind = ValueKind::Bool;   p.description = "Trace refracted photons";                   p.defaultValueHint = "TRUE"; }
				{ auto& p = P(); p.name = "shootFromNonMeshLights";   p.kind = ValueKind::Bool;   p.description = "Shoot from point / directional lights";     p.defaultValueHint = "TRUE"; }
				{ auto& p = P(); p.name = "shootFromMeshLights";      p.kind = ValueKind::Bool;   p.description = "Shoot from area / mesh luminaires";         p.defaultValueHint = "FALSE"; }
				{ auto& p = P(); p.name = "temporal_samples";         p.kind = ValueKind::UInt;   p.description = "Temporal samples for animated lights";      p.defaultValueHint = "1"; }
				{ auto& p = P(); p.name = "regenerate";               p.kind = ValueKind::Bool;   p.description = "Regenerate per frame";                      p.defaultValueHint = "FALSE"; }
			}
			template<typename PushFn>
			static void AddPhotonMapGatherCommonParams( PushFn P ) {
				{ auto& p = P(); p.name = "radius";        p.kind = ValueKind::Double; p.description = "Max gather radius"; p.defaultValueHint = "0"; }
				{ auto& p = P(); p.name = "ellipse_ratio"; p.kind = ValueKind::Double; p.description = "Flattening ratio for the gather ellipsoid"; p.defaultValueHint = "0.05"; }
				{ auto& p = P(); p.name = "min_photons";   p.kind = ValueKind::UInt;   p.description = "Minimum photons to gather"; p.defaultValueHint = "8"; }
				{ auto& p = P(); p.name = "max_photons";   p.kind = ValueKind::UInt;   p.description = "Maximum photons to gather"; p.defaultValueHint = "150"; }
			}
			template<typename PushFn>
			static void AddCameraCommonParams( PushFn P ) {
				{ auto& p = P(); p.name = "name";               p.kind = ValueKind::String;     p.description = "Optional identifier; defaults to \"default\" with auto-suffix on collision."; p.defaultValueHint = "default"; }
				{ auto& p = P(); p.name = "location";           p.kind = ValueKind::DoubleVec3; p.description = "World-space position"; }
				{ auto& p = P(); p.name = "lookat";             p.kind = ValueKind::DoubleVec3; p.description = "Look-at target point"; }
				{ auto& p = P(); p.name = "up";                 p.kind = ValueKind::DoubleVec3; p.description = "Up vector"; p.defaultValueHint = "0 1 0"; }
				// width / height / pixelAR moved to the `film` chunk
				// in scene format v6 (Phase B2 of the Camera/Film/Output
				// split).  Camera chunks are now imaging-only; the
				// rasterizer reads grid dims from `Scene::GetFilm()`.
				{ auto& p = P(); p.name = "exposure";           p.kind = ValueKind::Double;     p.description = "Shutter exposure time"; p.defaultValueHint = "0"; }
				{ auto& p = P(); p.name = "scanning_rate";      p.kind = ValueKind::Double;     p.description = "Rolling-shutter rate"; p.defaultValueHint = "0"; }
				{ auto& p = P(); p.name = "pixel_rate";         p.kind = ValueKind::Double;     p.description = "Per-pixel time offset"; p.defaultValueHint = "0"; }
				{ auto& p = P(); p.name = "pitch";              p.kind = ValueKind::Double;     p.description = "Pitch rotation (degrees)"; }
				{ auto& p = P(); p.name = "roll";               p.kind = ValueKind::Double;     p.description = "Roll rotation (degrees)"; }
				{ auto& p = P(); p.name = "yaw";                p.kind = ValueKind::Double;     p.description = "Yaw rotation (degrees)"; }
				{ auto& p = P(); p.name = "orientation";        p.kind = ValueKind::DoubleVec3; p.description = "Euler orientation (degrees)"; }
				{ auto& p = P(); p.name = "theta";              p.kind = ValueKind::Double;     p.description = "Polar angle (radians)"; }
				{ auto& p = P(); p.name = "phi";                p.kind = ValueKind::Double;     p.description = "Azimuthal angle (radians)"; }
				{ auto& p = P(); p.name = "target_orientation"; p.kind = ValueKind::DoubleVec3; p.description = "Target Euler orientation"; }
			}
			//! `persistenceDefault` is passed per chunk because the eight
			//! chunks sharing this helper do NOT share one Finalize default
			//! (perlin2d 0.5; perlin3d + turbulence3d 1.0; simplex3d,
			//! wavelet3d, curlnoise3d, domainwarp3d, perlinworley3d 0.65) --
			//! a single hardcoded hint here was wrong for seven of them, and
			//! `defaultValueHint` is MODEL-FACING (read_schema serves it as
			//! the parameter's `default`).
			template<typename PushFn>
			static void AddNoisePainterCommonParams( PushFn P, const char* persistenceDefault = "0.5" ) {
				{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;     p.description = "Unique name";                p.defaultValueHint = "noname"; }
				{ auto& p = P(); p.name = "colora";      p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "Painter used where the noise field is at its LOW end"; }
				{ auto& p = P(); p.name = "colorb";      p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "Painter used where the noise field is at its HIGH end"; }
				{ auto& p = P(); p.name = "persistence"; p.kind = ValueKind::Double;     p.description = "Amplitude falloff per octave (lower = smoother, higher = grittier)"; p.defaultValueHint = persistenceDefault; }
				{ auto& p = P(); p.name = "octaves";     p.kind = ValueKind::UInt;       p.description = "Number of noise octaves (more = finer detail, more cost)"; p.defaultValueHint = "4"; }
				{ auto& p = P(); p.name = "scale";       p.kind = ValueKind::DoubleVec3; p.description = "Per-axis FREQUENCY multiplier on the sample coordinate -- LARGER = tighter/finer features.  Unequal components stretch the pattern along an axis (that is how you get grain or banding rather than blobs)"; p.defaultValueHint = "1 1 1"; }
				{ auto& p = P(); p.name = "shift";       p.kind = ValueKind::DoubleVec3; p.description = "Per-axis offset added to the sample coordinate -- slides the pattern without rescaling it (use it to de-register two objects that share one painter)"; p.defaultValueHint = "0 0 0"; }
			}
			//
			// AddBaseRasterizerParams — the 7 fields every production
			// rasterizer accepts.  Takes a `BaseRasterizerDefaults` so
			// the per-rasterizer parser can pass its own *Defaults
			// struct (PixelPel overrides numPixelSamples=1, MLT
			// overrides oidnDenoise=false, etc.) and the GUI hint
			// matches the actual `Finalize` fallback.
			//
			template<typename PushFn>
			static void AddBaseRasterizerParams( PushFn P, const BaseRasterizerDefaults& d ) {
				{ auto& p = P(); p.name = "defaultshader";                          p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Shader}; p.description = "Default shader chain for hit points"; p.defaultValueHint = to_hint(d.defaultShader); }
				{ auto& p = P(); p.name = "samples";                                p.kind = ValueKind::UInt;   p.description = "Samples per pixel";                     p.defaultValueHint = to_hint(d.numPixelSamples); }
				{ auto& p = P(); p.name = "show_luminaires";                        p.kind = ValueKind::Bool;   p.description = "Show direct-visible luminaires";        p.defaultValueHint = to_hint(d.showLuminaires); }
				{ auto& p = P(); p.name = "oidn_denoise";                           p.kind = ValueKind::Bool;   p.description = "Enable OIDN denoiser";                  p.defaultValueHint = to_hint(d.oidnDenoise); }
				{ auto& p = P(); p.name = "oidn_quality";                           p.kind = ValueKind::Enum;   p.enumValues = {"auto","high","balanced","fast"}; p.description = "OIDN quality preset (auto picks from render-time / megapixels)"; p.defaultValueHint = to_hint(d.oidnQuality); }
				{ auto& p = P(); p.name = "oidn_device";                            p.kind = ValueKind::Enum;   p.enumValues = {"auto","cpu","gpu"};              p.description = "OIDN device backend (auto = prefer GPU, fall back to CPU)";       p.defaultValueHint = to_hint(d.oidnDevice); }
				{ auto& p = P(); p.name = "oidn_prefilter";                         p.kind = ValueKind::Enum;   p.enumValues = {"fast","accurate"};               p.description = "OIDN aux source mode (fast = retrace/first-hit, accurate = inline first-non-delta + prefilter)"; p.defaultValueHint = to_hint(d.oidnPrefilter); }
			}

			// Parse the `oidn_quality` enum string from a parser bag.  Unknown
			// strings fall through to Auto with a warning so a typo doesn't
			// silently freeze the user at HIGH on every render.
			static inline OidnQuality ParseOidnQuality( const std::string& s )
			{
				if( s == "high"     ) return OidnQuality::High;
				if( s == "balanced" ) return OidnQuality::Balanced;
				if( s == "fast"     ) return OidnQuality::Fast;
				if( s == "auto"     ) return OidnQuality::Auto;
				GlobalLog()->PrintEx( eLog_Warning,
					"Parser: unknown oidn_quality value \"%s\"; defaulting to auto",
					s.c_str() );
				return OidnQuality::Auto;
			}

			// Parse the `oidn_device` enum string from a parser bag.
			// Same fallback discipline as ParseOidnQuality — typos
			// fall through to Auto with a warning rather than silently
			// pinning the user to one backend.
			static inline OidnDevice ParseOidnDevice( const std::string& s )
			{
				if( s == "cpu"  ) return OidnDevice::CPU;
				if( s == "gpu"  ) return OidnDevice::GPU;
				if( s == "auto" ) return OidnDevice::Auto;
				GlobalLog()->PrintEx( eLog_Warning,
					"Parser: unknown oidn_device value \"%s\"; defaulting to auto",
					s.c_str() );
				return OidnDevice::Auto;
			}

			// Parse the `oidn_prefilter` enum string.  Default-falls
			// through to Fast (current default behaviour) on unknown
			// values; explicit `accurate` opts into the inline
			// first-non-delta + 3-filter prefilter pipeline.
			static inline OidnPrefilter ParseOidnPrefilter( const std::string& s )
			{
				if( s == "fast"     ) return OidnPrefilter::Fast;
				if( s == "accurate" ) return OidnPrefilter::Accurate;
				GlobalLog()->PrintEx( eLog_Warning,
					"Parser: unknown oidn_prefilter value \"%s\"; defaulting to fast",
					s.c_str() );
				return OidnPrefilter::Fast;
			}

			//////////////////////////////////////////
			// Painters
			//////////////////////////////////////////

			struct UniformColorPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",       "noname" );
					double color[3] = {0,0,0};
					bag.GetVec3( "color", color );
					// Default colorspace is `Rec709RGB_Linear` since 2026-05.
					// Hand-typed scalar / RGB values in scene files are
					// physical numbers (reflectance, light tint, intensity
					// multiplier), NOT display-referred.  Earlier default
					// `sRGB` silently decoded `color 0.5 0.5 0.5` to linear
					// 0.214 — a 2.4x dimming of every uniform-painter-driven
					// albedo.  Pre-2026-05 scenes that intended sRGB-encoded
					// display values must now declare `colorspace sRGB`
					// explicitly; image-texture painters
					// (png/jpeg/in-memory) keep their per-role auto colour-
					// space (sRGB for basecolor / emissive, linear for normal
					// / metallic-roughness / occlusion).
					std::string color_space = bag.GetString( "colorspace", "Rec709RGB_Linear" );

					PainterColor pc = { {color[0], color[1], color[2]} };
					s_painterColors[name] = pc;

					return pJob.AddUniformColorPainter( name.c_str(), color, color_space.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "uniformcolor_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Constant RGB painter -- ZERO spatial variation.  Right for small parts, test scenes, and as an operand of blend_painter; on a LARGE hero surface (table top, wall, floor, ground) one flat colour is the single biggest reason a render reads as amateurish.  Reach for a procedural painter instead -- read_skill {name:\"procedural-textures\"}.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";      p.kind = ValueKind::String;     p.description = "Unique name";                          p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "color";     p.kind = ValueKind::DoubleVec3; p.description = "R G B values";                         p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "colorspace";p.kind = ValueKind::String;    p.description = "Interpretation of R G B (linear default)"; p.defaultValueHint = "Rec709RGB_Linear"; }
						return cd;
					}();
					return d;
				}
			};

			struct VertexColorPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",       "noname" );
					double fallback[3]      = { 1.0, 1.0, 1.0 };
					bag.GetVec3( "fallback", fallback );
					// Default linear, matching uniformcolor_painter (2026-05).
					std::string color_space = bag.GetString( "colorspace", "Rec709RGB_Linear" );

					return pJob.AddVertexColorPainter( name.c_str(), fallback, color_space.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "vertex_color_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Painter that returns the per-vertex color "
							"interpolated by the geometry at the hit point.  "
							"Falls back to the configured color when the hit "
							"surface has no per-vertex color data.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";       p.kind = ValueKind::String;     p.description = "Unique name";                                p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "fallback";   p.kind = ValueKind::DoubleVec3; p.description = "RGB used when no vertex color is present";   p.defaultValueHint = "1 1 1"; }
						{ auto& p = P(); p.name = "colorspace"; p.kind = ValueKind::String;     p.description = "Interpretation of the fallback RGB (linear default)"; p.defaultValueHint = "Rec709RGB_Linear"; }
						return cd;
					}();
					return d;
				}
			};

			struct SpectralPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name    = bag.GetString( "name",   "noname" );
					double nmbegin      = bag.GetDouble( "nmbegin", 400.0 );
					double nmend        = bag.GetDouble( "nmend",   700.0 );
					double scale        = bag.GetDouble( "scale",   1.0 );

					std::vector<double> wavelengths;
					std::vector<double> amplitudes;

					// Repeatable per-sample control points: "cp <nm> <amp>"
					const std::vector<std::string>& cps = bag.GetRepeatable( "cp" );
					for( size_t k = 0; k < cps.size(); ++k ) {
						double nm = 0.0, amp = 0.0;
						sscanf( cps[k].c_str(), "%lf %lf", &nm, &amp );
						wavelengths.push_back( nm );
						amplitudes.push_back( amp );
					}

					// Optional file-loaded spectrum (pairs)
					if( bag.Has( "file" ) ) {
						std::string fname = bag.GetString( "file" );
						std::vector<std::string> fileLines;
						if( !ReadDataFileLines( String( fname.c_str() ), fileLines ) ) {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to open file `%s`", fname.c_str() );
							return false;
						}
						for( const std::string& ln : fileLines ) {
							double nm = 0.0, amp = 0.0;
							if( sscanf( ln.c_str(), "%lf %lf", &nm, &amp ) == 2 ) {
								wavelengths.push_back( nm );
								amplitudes.push_back( amp );
							} else {
								GlobalLog()->PrintEx( eLog_Warning, "spectral file:: skipping malformed line in `%s`", fname.c_str() );
							}
						}
					}

					// Optional file-loaded wavelengths
					if( bag.Has( "nmfile" ) ) {
						std::string fname = bag.GetString( "nmfile" );
						FILE* f = fopen( GlobalMediaPathLocator().Find( String( fname.c_str() ) ).c_str(), "r" );
						if( f ) {
							char nmbuf[MAX_CHARS_PER_LINE];
							while( fgets( nmbuf, sizeof( nmbuf ), f ) ) {
								const char* q = nmbuf;
								while( *q == ' ' || *q == '\t' || *q == '\r' || *q == '\n' ) ++q;
								if( *q == '\0' || *q == '#' ) continue;
								double nm = 0.0;
								if( sscanf( nmbuf, "%lf", &nm ) == 1 ) wavelengths.push_back( nm );
							}
							fclose( f );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to open file `%s`", fname.c_str() );
							return false;
						}
					}

					// Optional file-loaded amplitudes
					if( bag.Has( "ampfile" ) ) {
						std::string fname = bag.GetString( "ampfile" );
						FILE* f = fopen( GlobalMediaPathLocator().Find( String( fname.c_str() ) ).c_str(), "r" );
						if( f ) {
							char ampbuf[MAX_CHARS_PER_LINE];
							while( fgets( ampbuf, sizeof( ampbuf ), f ) ) {
								const char* q = ampbuf;
								while( *q == ' ' || *q == '\t' || *q == '\r' || *q == '\n' ) ++q;
								if( *q == '\0' || *q == '#' ) continue;
								double amp = 0.0;
								if( sscanf( ampbuf, "%lf", &amp ) == 1 ) amplitudes.push_back( amp );
							}
							fclose( f );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to open file `%s`", fname.c_str() );
							return false;
						}
					}

					if( amplitudes.empty() || wavelengths.empty() ) {
						GlobalLog()->PrintEx( eLog_Error, "spectral_painter `%s`: no samples (empty / all-comment file, no inline cp)", name.c_str() );
						return false;
					}
					return pJob.AddSpectralColorPainter( name.c_str(), &amplitudes[0], &wavelengths[0], nmbegin, nmend, static_cast<unsigned int>(amplitudes.size()), scale );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "spectral_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Spectral painter defined by wavelength/amplitude samples.  This is the COLOUR pipe (spectral reflectance / emission, colorspace-converted); a wavelength-dependent PHYSICAL SCALAR (dispersive IOR, absorption) belongs in scalar_painter { file ... } instead.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";    p.kind = ValueKind::String;   p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "nmbegin"; p.kind = ValueKind::Double;   p.description = "Start wavelength (nm)"; p.defaultValueHint = "400"; }
						{ auto& p = P(); p.name = "nmend";   p.kind = ValueKind::Double;   p.description = "End wavelength (nm)"; p.defaultValueHint = "700"; }
						{ auto& p = P(); p.name = "scale";   p.kind = ValueKind::Double;   p.description = "Overall amplitude scale"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "cp";      p.kind = ValueKind::String;   p.repeatable = true; p.description = "Wavelength,amplitude sample (repeatable)"; }
						{ auto& p = P(); p.name = "file";    p.kind = ValueKind::Filename; p.description = "Spectrum text file (pairs)"; }
						{ auto& p = P(); p.name = "nmfile";  p.kind = ValueKind::Filename; p.description = "Wavelength list file"; }
						{ auto& p = P(); p.name = "ampfile"; p.kind = ValueKind::Filename; p.description = "Amplitude list file"; }
						return cd;
					}();
					return d;
				}
			};

			//////////////////////////////////////////
			// scalar_painter — IScalarPainter chunk (Phase 2 of
			// IScalarPainter refactor, see docs/ISCALARPAINTER_REFACTOR.md).
			//
			// Dispatches on which optional fields are present:
			//   value <s>                              → UniformScalarPainter
			//   values <r> <g> <b>                     → RGBScalarPainter
			//   file <path>                            → PiecewiseLinearScalarPainter (2-col file)
			//   sellmeier <B1> <B2> <B3> <C1> <C2> <C3>→ SellmeierScalarPainter
			//   polynomial <c0> <c1> ...               → PolynomialScalarPainter
			//   function1d <name>                      → Function1DScalarPainter wrapping a named IFunction1D
			//   function2d <name>                      → Function2DScalarPainter wrapping a named IFunction2D
			//   base <name> [scale <s>]                → ScaledScalarPainter (default scale = 1.0)
			//   multiply <a> <b>                       → MultiplyScalarPainter
			//   add <a> <b> [weight_a <w>] [weight_b <w>]
			//                                          → AddScalarPainter (multiply's
			//                                            additive sibling; out = weight_a*a
			//                                            + weight_b*b, weights default 1.0)
			//   texture <name> [channel R|G|B] [scale <s>] [bias <b>]
			//                                          → TextureScalarPainter (image map
			//                                            sampled at surface UV; out = bias +
			//                                            scale * rawTexel; no JH-uplift)
			//   expression <body> [param ...] [def ...] [seed <s>]
			//                                          → ExpressionScalarPainter (doc 88 P1 S2)
			//   painter <name> [channel R|G|B|A] [scale <s>] [bias <b>]
			//                                          → PainterChannelScalarPainter (doc 88 P2.1 S3;
			//                                            ANY colour painter, not just raster images;
			//                                            channel A reads GetAlpha)
			//
			// At most one of {value, values, file, sellmeier, polynomial,
			// function1d, function2d, base, multiply, add, texture,
			// expression, painter} may be present.  Mutually exclusive —
			// the parser raises an error otherwise.
			//////////////////////////////////////////
			struct ScalarPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );

					// Tally which form is present.
					const bool hasValue       = bag.Has( "value" );
					const bool hasValues      = bag.Has( "values" );
					const bool hasFile        = bag.Has( "file" );
					const bool hasSellmeier   = bag.Has( "sellmeier" );
					const bool hasPolynomial  = bag.Has( "polynomial" );
					const bool hasFunction1d  = bag.Has( "function1d" );
					const bool hasFunction2d  = bag.Has( "function2d" );
					const bool hasBase        = bag.Has( "base" );
					const bool hasMultiply    = bag.Has( "multiply" );
					const bool hasAdd         = bag.Has( "add" );
					const bool hasTexture     = bag.Has( "texture" );
					const bool hasExpression  = bag.Has( "expression" );
					const bool hasPainter     = bag.Has( "painter" );

					const int formCount = (int)hasValue + (int)hasValues + (int)hasFile +
						(int)hasSellmeier + (int)hasPolynomial + (int)hasFunction1d +
						(int)hasFunction2d + (int)hasBase + (int)hasMultiply + (int)hasAdd + (int)hasTexture +
						(int)hasExpression + (int)hasPainter;
					if( formCount == 0 ) {
						GlobalLog()->PrintEx( eLog_Error,
							"scalar_painter `%s`: missing form (one of value, values, file, sellmeier, polynomial, function1d, function2d, base, multiply, add, texture, expression, painter)",
							name.c_str() );
						return false;
					}
					if( formCount > 1 ) {
						GlobalLog()->PrintEx( eLog_Error,
							"scalar_painter `%s`: multiple forms specified (mutually exclusive)",
							name.c_str() );
						return false;
					}

					IJobPriv* pPriv = dynamic_cast<IJobPriv*>( &pJob );
					if( !pPriv ) {
						GlobalLog()->PrintEx( eLog_Error,
							"scalar_painter `%s`: IJobPriv unavailable", name.c_str() );
						return false;
					}

					IScalarPainter* painter = nullptr;

					if( hasValue ) {
						const double v = bag.GetDouble( "value" );
						RISE_API_CreateUniformScalarPainter( &painter, Scalar( v ) );
					}
					else if( hasValues ) {
						const std::string raw = bag.GetString( "values" );
						double rgb[3] = { 0, 0, 0 };
						const int matched = sscanf( raw.c_str(), "%lf %lf %lf",
							&rgb[0], &rgb[1], &rgb[2] );
						if( matched != 3 ) {
							GlobalLog()->PrintEx( eLog_Error,
								"scalar_painter `%s`: `values` requires three numeric components (got %d in `%s`)",
								name.c_str(), matched, raw.c_str() );
							return false;
						}
						RISE_API_CreateRGBScalarPainter( &painter,
							Scalar( rgb[0] ), Scalar( rgb[1] ), Scalar( rgb[2] ) );
					}
					else if( hasFile ) {
						std::string fname = bag.GetString( "file" );
						FILE* f = fopen( GlobalMediaPathLocator().Find( String( fname.c_str() ) ).c_str(), "r" );
						if( !f ) {
							GlobalLog()->PrintEx( eLog_Error,
								"scalar_painter `%s`: failed to open file `%s`",
								name.c_str(), fname.c_str() );
							return false;
						}
						std::vector<std::pair<Scalar, Scalar>> samples;
						char spbuf[MAX_CHARS_PER_LINE];
						while( fgets( spbuf, sizeof( spbuf ), f ) ) {
							const char* q = spbuf;
							while( *q == ' ' || *q == '\t' || *q == '\r' || *q == '\n' ) ++q;
							if( *q == '\0' || *q == '#' ) continue;
							double nm = 0.0, val = 0.0;
							if( sscanf( spbuf, "%lf %lf", &nm, &val ) == 2 ) {
								samples.emplace_back( Scalar( nm ), Scalar( val ) );
							}
						}
						fclose( f );
						if( samples.empty() ) {
							GlobalLog()->PrintEx( eLog_Error,
								"scalar_painter `%s`: file `%s` produced no samples",
								name.c_str(), fname.c_str() );
							return false;
						}
						RISE_API_CreatePiecewiseLinearScalarPainter( &painter, samples );
					}
					else if( hasSellmeier ) {
						double B1=0, B2=0, B3=0, C1=0, C2=0, C3=0;
						const std::string s = bag.GetString( "sellmeier" );
						if( sscanf( s.c_str(), "%lf %lf %lf %lf %lf %lf",
								&B1, &B2, &B3, &C1, &C2, &C3 ) != 6 ) {
							GlobalLog()->PrintEx( eLog_Error,
								"scalar_painter `%s`: sellmeier needs 6 values (B1 B2 B3 C1 C2 C3)",
								name.c_str() );
							return false;
						}
						RISE_API_CreateSellmeierScalarPainter( &painter,
							Scalar( B1 ), Scalar( B2 ), Scalar( B3 ),
							Scalar( C1 ), Scalar( C2 ), Scalar( C3 ) );
					}
					else if( hasPolynomial ) {
						const std::string s = bag.GetString( "polynomial" );
						std::vector<Scalar> coeffs;
						std::istringstream iss( s );
						double c;
						while( iss >> c ) coeffs.push_back( Scalar( c ) );
						// Reject trailing non-numeric content — silent
						// truncation would let a typo produce a different
						// polynomial than authored.  After the loop,
						// `iss.eof()` is true iff every input token was
						// consumed as a number; `fail()` is set when the
						// last `>>` ran off either valid input or a bad
						// token, which is fine if we hit EOF cleanly.
						if( ! iss.eof() ) {
							GlobalLog()->PrintEx( eLog_Error,
								"scalar_painter `%s`: polynomial has trailing non-numeric content in `%s`",
								name.c_str(), s.c_str() );
							return false;
						}
						if( coeffs.empty() ) {
							GlobalLog()->PrintEx( eLog_Error,
								"scalar_painter `%s`: polynomial needs at least one coefficient",
								name.c_str() );
							return false;
						}
						RISE_API_CreatePolynomialScalarPainter( &painter, coeffs );
					}
					else if( hasFunction1d ) {
						const std::string ref = bag.GetString( "function1d" );
						IFunction1D* f = pPriv->GetFunction1Ds()->GetItem( ref.c_str() );
						if( !f ) {
							GlobalLog()->PrintEx( eLog_Error,
								"scalar_painter `%s`: function1d `%s` not found",
								name.c_str(), ref.c_str() );
							return false;
						}
						RISE_API_CreateFunction1DScalarPainter( &painter, f );
					}
					else if( hasFunction2d ) {
						const std::string ref = bag.GetString( "function2d" );
						IFunction2D* f = pPriv->GetFunction2Ds()->GetItem( ref.c_str() );
						if( !f ) {
							GlobalLog()->PrintEx( eLog_Error,
								"scalar_painter `%s`: function2d `%s` not found",
								name.c_str(), ref.c_str() );
							return false;
						}
						const double scale = bag.GetDouble( "scale", 1.0 );
						const double bias  = bag.GetDouble( "bias",  0.0 );
						RISE_API_CreateFunction2DScalarPainterAffine( &painter, f, scale, bias );
					}
					else if( hasBase ) {
						const std::string ref = bag.GetString( "base" );
						const double scale = bag.GetDouble( "scale", 1.0 );
						IScalarPainter* base = pPriv->GetScalarPainters()->GetItem( ref.c_str() );
						if( !base ) {
							GlobalLog()->PrintEx( eLog_Error,
								"scalar_painter `%s`: base scalar_painter `%s` not found",
								name.c_str(), ref.c_str() );
							return false;
						}
						RISE_API_CreateScaledScalarPainter( &painter, base, Scalar( scale ) );
					}
					else if( hasMultiply ) {
						const std::string s = bag.GetString( "multiply" );
						char aname[256] = {0}, bname[256] = {0};
						if( sscanf( s.c_str(), "%255s %255s", aname, bname ) != 2 ) {
							GlobalLog()->PrintEx( eLog_Error,
								"scalar_painter `%s`: multiply needs two scalar_painter names",
								name.c_str() );
							return false;
						}
						IScalarPainter* a = pPriv->GetScalarPainters()->GetItem( aname );
						IScalarPainter* b = pPriv->GetScalarPainters()->GetItem( bname );
						if( !a || !b ) {
							GlobalLog()->PrintEx( eLog_Error,
								"scalar_painter `%s`: multiply operands `%s` / `%s` not found",
								name.c_str(), aname, bname );
							return false;
						}
						RISE_API_CreateMultiplyScalarPainter( &painter, a, b );
					}
					else if( hasAdd ) {
						const std::string s = bag.GetString( "add" );
						char aname[256] = {0}, bname[256] = {0};
						if( sscanf( s.c_str(), "%255s %255s", aname, bname ) != 2 ) {
							GlobalLog()->PrintEx( eLog_Error,
								"scalar_painter `%s`: add needs two scalar_painter names",
								name.c_str() );
							return false;
						}
						IScalarPainter* a = pPriv->GetScalarPainters()->GetItem( aname );
						IScalarPainter* b = pPriv->GetScalarPainters()->GetItem( bname );
						if( !a || !b ) {
							GlobalLog()->PrintEx( eLog_Error,
								"scalar_painter `%s`: add operands `%s` / `%s` not found",
								name.c_str(), aname, bname );
							return false;
						}
						const double weightA = bag.GetDouble( "weight_a", 1.0 );
						const double weightB = bag.GetDouble( "weight_b", 1.0 );
						RISE_API_CreateAddScalarPainter( &painter, a, b, Scalar( weightA ), Scalar( weightB ) );
					}
					else if( hasTexture ) {
						// Spatially-varying physical scalar driven by a 2D image
						// map sampled at the surface UV.  We resolve a previously
						// declared image painter (png_painter / jpg_painter / ...),
						// pull its raster accessor, and sample the chosen channel
						// DIRECTLY (no Jakob-Hanika uplift, no colourspace
						// conversion) via TextureScalarPainter.  This is the
						// physical-scalar analogue of binding a png_painter to a
						// colour slot, and is the supported way to map an
						// IScalarPainter slot (e.g. thin-film film_thickness) from
						// an image.  out = bias + scale * rawTexel, rawTexel in [0,1].
						const std::string ref = bag.GetString( "texture" );
						IPainter* imgPainter = pPriv->GetPainters()->GetItem( ref.c_str() );
						if( !imgPainter ) {
							GlobalLog()->PrintEx( eLog_Error,
								"scalar_painter `%s`: texture `%s` not found (declare a png_painter / jpg_painter / hdr_painter / exr_painter / tiff_painter with that name first)",
								name.c_str(), ref.c_str() );
							return false;
						}
						Implementation::TexturePainter* tex =
							dynamic_cast<Implementation::TexturePainter*>( imgPainter );
						if( !tex ) {
							GlobalLog()->PrintEx( eLog_Error,
								"scalar_painter `%s`: texture `%s` is not an image painter (only raster-backed painters such as png_painter / jpg_painter / hdr_painter / exr_painter / tiff_painter can be sampled spatially)",
								name.c_str(), ref.c_str() );
							return false;
						}
						IRasterImageAccessor* pRIA = tex->GetRasterImageAccessor();
						if( !pRIA ) {
							GlobalLog()->PrintEx( eLog_Error,
								"scalar_painter `%s`: texture `%s` has no raster accessor",
								name.c_str(), ref.c_str() );
							return false;
						}
						// channel select: R (default) / G / B.  A is rejected
						// here (TextureScalarPainter has no alpha read) --
						// use the `painter` form instead, which reads ANY
						// painter's GetAlpha.
						unsigned int channel = 0;
						if( bag.Has( "channel" ) ) {
							const std::string chs = bag.GetString( "channel" );
							if(      chs == "R" ) channel = 0;
							else if( chs == "G" ) channel = 1;
							else if( chs == "B" ) channel = 2;
							else if( chs == "A" ) {
								GlobalLog()->PrintEx( eLog_Error,
									"scalar_painter `%s`: `texture` form does not support channel A (TextureScalarPainter has no alpha read) -- use `painter %s channel A` instead",
									name.c_str(), ref.c_str() );
								return false;
							}
							else {
								GlobalLog()->PrintEx( eLog_Error,
									"scalar_painter `%s`: unknown channel `%s` (expected R, G, or B)",
									name.c_str(), chs.c_str() );
								return false;
							}
						}
						const double scale = bag.GetDouble( "scale", 1.0 );
						const double bias  = bag.GetDouble( "bias",  0.0 );
						RISE_API_CreateTextureScalarPainterAffine(
							&painter, pRIA, channel, Scalar( scale ), Scalar( bias ) );
					}
					else if( hasPainter ) {
						// P2.1 (doc 88 S3): the any-painter -> scalar bridge.
						// Generalizes `texture` (raster-only) to ANY colour
						// painter -- every painter kind (expression_painter
						// included) becomes bindable to every physical-scalar
						// slot.  See PainterChannelScalarPainter.h's file
						// header for the post-colourspace-value caveat (same
						// one PainterToScalarAdapter carries).
						const std::string ref = bag.GetString( "painter" );
						IPainter* srcPainter = pPriv->GetPainters()->GetItem( ref.c_str() );
						if( !srcPainter ) {
							GlobalLog()->PrintEx( eLog_Error,
								"scalar_painter `%s`: painter `%s` not found",
								name.c_str(), ref.c_str() );
							return false;
						}
						unsigned int channel = 0;	// R default
						if( bag.Has( "channel" ) ) {
							const std::string chs = bag.GetString( "channel" );
							if(      chs == "R" ) channel = 0;
							else if( chs == "G" ) channel = 1;
							else if( chs == "B" ) channel = 2;
							else if( chs == "A" ) channel = 3;
							else {
								GlobalLog()->PrintEx( eLog_Error,
									"scalar_painter `%s`: unknown channel `%s` (expected R, G, B, or A)",
									name.c_str(), chs.c_str() );
								return false;
							}
						}
						const double scale = bag.GetDouble( "scale", 1.0 );
						const double bias  = bag.GetDouble( "bias",  0.0 );
						RISE_API_CreatePainterChannelScalarPainter(
							&painter, *srcPainter, channel, Scalar( scale ), Scalar( bias ) );
					}
					else if( hasExpression ) {
						// Spatially-varying physical scalar driven by the doc-88
						// texture-expression VM -- the G1 fix (no colorspace, no
						// JH uplift, by construction).  Same param/def/seed
						// grammar as expression_painter (BuildExpressionProgramFromChunkFields
						// is the shared builder), full 3D context (u,v,P,Po,N,fw,fwo)
						// enabled.  `time` is NOT exposed here -- IScalarPainter
						// has no IKeyframable hook (see ExpressionScalarPainter's
						// class doc comment in ExpressionPainter.h).
						const std::string finalExpr = bag.GetString( "expression", "" );
						const double seed = bag.GetDouble( "seed", 0.0 );
						const std::vector<std::string>& params = bag.GetRepeatable( "param" );
						const std::vector<std::string>& defs = bag.GetRepeatable( "def" );

						const std::string context = "scalar_painter `" + name + "` (expression)";
						Implementation::ExpressionProgram prog = Implementation::ExpressionProgram::Invalid();
						std::vector<Implementation::ParamSpec> specs;
						// true/true: full context vars + auto-registered `seed`,
						// this call's ORIGINAL (pre-unification) behavior -- see
						// BuildExpressionProgramFromChunkFields's own doc comment
						// (ExpressionPainter.h).
						std::string exprErr;
						if( !Implementation::BuildExpressionProgramFromChunkFields(
								context, params, defs, Scalar( seed ), finalExpr, prog, specs,
								/*enableContextVars=*/true, /*autoRegisterSeed=*/true, &exprErr ) ) {
							// See Job::AddExpressionPainter's twin site -- thread the
							// specific compiler diagnostic into the CST sink instead
							// of leaving the caller with the generic apply-failed text.
							if( RISE::g_cstFinalizeDiagSink ) *RISE::g_cstFinalizeDiagSink = exprErr;
							return false;
						}
						RISE_API_CreateExpressionScalarPainter( &painter, prog, specs );
					}

					if( !painter ) {
						GlobalLog()->PrintEx( eLog_Error,
							"scalar_painter `%s`: construction failed", name.c_str() );
						return false;
					}

					pPriv->GetScalarPainters()->AddItem( painter, name.c_str() );
					painter->release();
					return true;
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "scalar_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Physical-scalar painter (no colorspace, no spectral uplift).  Used for IOR, scattering, roughness, absorption, phase asymmetry.  Pick exactly one form via the optional fields below.  The `function2d`, `texture`, `expression`, and `painter` forms VARY ACROSS THE SURFACE -- every other form is spatially constant, so spatially-varying roughness means scalar_painter { expression <body> } (the doc-88 texture-expression VM; see `expression` below), scalar_painter { function2d <a UV-domain painter or expression_function2d> }, scalar_painter { texture <image painter> }, or scalar_painter { painter <any colour painter> channel <R|G|B|A> } (the any-painter -> scalar bridge -- P2.1: binds ANY colour painter kind, not just raster images).  `expression` is also the only form that can yield a genuine per-channel triple (a vec3-typed body sets HasPerChannelVariation) for spatially-varying RGB dispersion.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";       p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "value";      p.kind = ValueKind::Double;     p.description = "Single scalar value (form 1: UniformScalarPainter)"; }
						{ auto& p = P(); p.name = "values";     p.kind = ValueKind::DoubleVec3; p.description = "Per-channel (R G B) scalars (form 2: RGBScalarPainter)"; }
						{ auto& p = P(); p.name = "file";       p.kind = ValueKind::Filename;   p.description = "2-column (nm value) file (form 3: PiecewiseLinearScalarPainter)"; }
						{ auto& p = P(); p.name = "sellmeier";  p.kind = ValueKind::String;     p.description = "Sellmeier coefficients `B1 B2 B3 C1 C2 C3` (form 4: SellmeierScalarPainter)"; }
						{ auto& p = P(); p.name = "polynomial"; p.kind = ValueKind::String;     p.description = "Polynomial coefficients `c0 c1 c2 ...` (form 5: PolynomialScalarPainter)"; }
						{ auto& p = P(); p.name = "function1d"; p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Function}; p.description = "Named IFunction1D to wrap (form 6: Function1DScalarPainter)"; p.semantics.pipe = ParameterPipe::Function1D; }
						{ auto& p = P(); p.name = "function2d"; p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter, ChunkCategory::Function}; p.description = "Named IFunction2D to wrap (form 7: Function2DScalarPainter) -- resolved via `pPriv->GetFunction2Ds()->GetItem` (this parser's own Finalize), which holds every `function`-category IFunction2D (piecewise_linear_function2d) PLUS every dual-registered colour painter except expression_painter/scalar_painter (Job.cpp's RegisterPainterDual), hence the {Painter, Function} pair"; p.semantics.pipe = ParameterPipe::Function2D; }
						{ auto& p = P(); p.name = "base";       p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "Base scalar_painter for ScaledScalarPainter (form 8)"; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.note = "referenceCategories lists {Painter} (the CATEGORY grouping every painter-family chunk, scalar_painter included -- see ChunkParserRegistry.cpp's Describe()) but the value resolves via GetScalarPainters(), i.e. the Scalar pipe specifically -- referenceCategories is category, not pipe."; }
						{ auto& p = P(); p.name = "scale";      p.kind = ValueKind::Double;     p.description = "Scale factor (companion to `base`, `texture`, `function2d`, and `painter`)"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "multiply";   p.kind = ValueKind::String;     p.tupleKinds = {ValueKind::Reference, ValueKind::Reference}; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Two scalar_painter names `a b` (form 9: MultiplyScalarPainter)"; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.note = "tuple of two scalar_painter names, both resolved via GetScalarPainters()"; }
						{ auto& p = P(); p.name = "add";        p.kind = ValueKind::String;     p.tupleKinds = {ValueKind::Reference, ValueKind::Reference}; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Two scalar_painter names `a b` (form 13: AddScalarPainter).  `multiply`'s additive sibling -- out = weight_a*a + weight_b*b, so a detail field layered on `add` never zeroes out where the OTHER operand is zero the way a product would.  See `weight_a` / `weight_b` below."; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.note = "tuple of two scalar_painter names, both resolved via GetScalarPainters()"; }
						{ auto& p = P(); p.name = "weight_a";   p.kind = ValueKind::Double;     p.description = "Multiplier on `add`'s first operand (companion to `add`)"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "weight_b";   p.kind = ValueKind::Double;     p.description = "Multiplier on `add`'s second operand (companion to `add`)"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "texture";    p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "Named raster image painter (png_painter / jpg_painter / hdr_painter / exr_painter / tiff_painter) to sample spatially at the surface UV (form 10: TextureScalarPainter; no JH-uplift / colourspace conversion; channel A not supported here -- use `painter` instead)"; p.semantics.pipe = ParameterPipe::Color; p.semantics.keywordAllowlist = {"png_painter", "jpg_painter", "hdr_painter", "exr_painter", "tiff_painter"}; p.semantics.note = "Special case (the \"texture-form channel A redirect\"): resolves via GetPainters() then dynamic_cast<TexturePainter*> -- only raster-image painter chunks pass, not every Color-pipe chunk (a checker_painter or blend_painter is Color-pipe but NOT a TexturePainter and is rejected). channel \"A\" is additionally refused at the value layer with a message redirecting to the `painter` form (ChunkParserRegistry.cpp: \"does not support channel A ... use `painter %s channel A` instead\") -- a value-level constraint CheckConnection does not model (it answers candidate-identity legality, not per-channel value legality), documented here so the special case is not silently lost."; }
						{ auto& p = P(); p.name = "channel";    p.kind = ValueKind::Enum;       p.enumValues = {"R","G","B","A"}; p.description = "Which channel sources the scalar (companion to `texture` [R/G/B only] and `painter` [R/G/B/A])"; p.defaultValueHint = "R"; }
						{ auto& p = P(); p.name = "bias";       p.kind = ValueKind::Double;     p.description = "Additive offset for the `texture` / `function2d` / `painter` forms: out = bias + scale * raw (raw in [0,1] for texture/painter)"; p.defaultValueHint = "0.0"; }
						{ auto& p = P(); p.name = "expression"; p.kind = ValueKind::String;     p.description = "Final value expression over the FULL 3D context (u, v, P, Po, N, fw, fwo, curv, curvR, plus the occlusion()/convexity()/thickness()/proximity()/interior() builtins; NOT time -- see `seed` below) (form 11: ExpressionScalarPainter, the doc-88 texture-expression VM).  `curv` / `curvR` are SURFACE CURVATURE at the hit, the geometry-derived signal wear and grime masks key on: POSITIVE = convex (an edge), NEGATIVE = concave (a crevice), 0 = flat.  `curvR` is the raw signed MEAN curvature in 1/world-length; `curv` is that value normalized by the hit geometry's world bounding-box diagonal, so it reads O(1) at object scale and `clamp(curv,0,1)` is an edge-wear mask / `clamp(-curv,0,1)` a crevice mask on ANY scene scale -- prefer `curv` unless you genuinely want physical units.  Quality by geometry: EXACT on sdf_geometry / skeleton_geometry (a true differential quantity on an implicit surface) and on the analytic curved primitives (sphere, ellipsoid, torus, cylinder); FACETED on triangle meshes (it is only as good as the vertex normals, and jumps across shared edges); correctly 0 on planar primitives; and 0 as an ABSENCE, not a claim of flatness, on the bezier/bilinear patch stubs, which report no derivatives at all.  Under NON-UNIFORM object scale the normalization is a documented geometric-mean approximation (no single length is right for a `scale 4 0.05 4` panel).  It is fine-scale and RADIUS-FREE -- the local differential curvature, not a radius-sampled \"wear the 2cm edges, ignore the 2mm ones\" signal.  BUMP / NORMAL MAPS DO NOT MOVE IT: it comes from the geometric normal field, so `curv` and `N` legitimately disagree on a bump-mapped surface -- that is correct (a wear mask wants the form, not the texture, and a bump-perturbed curvature would double-count detail the bump map already shades).  GEOMETRY SIGNALS -- `occlusion(radius)`, `convexity(radius)` and `thickness(radius)`, the three ARG-TAKING builtins that ask the hit geometry about itself.  `occlusion` returns [0,1] with 1 = UNOCCLUDED (the universal convention -- white is open, dark is cavity), `convexity` returns [0,1] with 0 = FLAT-OR-CONCAVE and 1 = a knife edge, `thickness` returns [0,1] with 1 = THICK, normalized by the query radius.  `occlusion` AND `convexity` PARTITION ONE QUESTION -- how open is the surface at scale `radius`, measured against what a FLAT surface would read -- with occlusion reporting how much LESS open than flat and convexity how much MORE.  Neither is ever a residual of the other, and that is the thing to rely on while authoring.  (1) A MERELY CONVEX EDGE READS occlusion EXACTLY 1, the same as a flat face; occlusion darkens only for real cavities, so `1 - occlusion(r)` is a usable crevice mask with NO thresholding needed to keep it off the arrises.  (Before 2026-09-06 it was not: the old estimator returned 0.707 on any convex edge a CSG `intersect`/`subtract` produced, and the SAME 0.707 in a concave 90-degree valley, so the two were indistinguishable and every such scene needed a hand-tuned smoothstep.)  (2) A FLAT FACE AND EVERY CAVITY READ convexity EXACTLY 0, and every value above that has a FIXED GEOMETRIC MEANING on any object at any scene scale -- 0.5 IS a 90-degree arris, 0.75 IS a three-face corner -- which is precisely what `curv` cannot offer, since `curv` is an unbounded differential quantity whose useful thresholds have to be probe-measured per object.  Reach for `convexity(r)` for edge wear (\"wear the edges about r across, ignore the rest\"), and for `curv` when you want the form's signed bending at no particular scale.  THEY ARE MEASURED DIFFERENTLY, which matters in two places: occlusion sphere-traces 12 cosine-weighted directions over the outward hemisphere, the set spun about the normal per hit so the answer is an expectation rather than a multiple of 1/12 (the same integral -- and since 2026-09-07 the same per-hit spin -- a mesh's baked AO computes) and therefore SEES NARROW APERTURES a crack, a slot, a fold; convexity samples the query BALL and therefore sees smooth bulges too (on a convex sphere of radius rho it reads 3*radius/(8*rho), where a purely directional measure would read 0).  On edges, creases and corners the two agree exactly.  Occlusion is also the more expensive of the two by roughly 3x (~91 field evaluations against convexity's 32) -- neither costs anything unless the body calls it.  `radius` is NOT a world length: it is a FRACTION of the hit geometry's own characteristic size (its bounding-box diagonal), so `occlusion(0.05)` means \"5% of the object\" and reads identically at any scene scale and on any instance of that geometry.  A LITERAL radius <= 0 is a COMPILE error; a computed one that lands <= 0 returns the neutral value.  TWO GEOMETRY FAMILIES ANSWER THEM, by different means.  The SDF family (`sdf_geometry`, `skeleton_geometry`) evaluates them LIVE from its distance field and accepts a DYNAMIC radius (any expression, recomputed per hit).  INDEXED TRIANGLE MESHES answer from a per-vertex field BAKED LAZILY -- the first query at a given radius pays the bake (it announces itself and its millisecond cost in the log), every later sample is a barycentric read of it -- which makes the radius on a mesh necessarily a LITERAL: a computed one reads the NEUTRAL fallback rather than silently borrowing a table baked at a different scale, and at most 8 distinct literal radii per signal per mesh are baked before further ones read neutral too.  Everything else returns the NEUTRAL fallback: the analytic primitives, non-indexed meshes, and heightfield-mode `sdf_geometry` (whose global Lipschitz bound would make a local answer systematically wrong).  That fallback is deliberately the do-nothing end of each range -- occlusion 1 (unoccluded), thickness 1 (thick), convexity 0 (flat) -- so a mask built on an unsupported geometry lights NOTHING up rather than lighting everything up.  Note that convexity's neutral sits at the OPPOSITE end of its range from the other two, for exactly that reason: an absent edge-wear signal must mean \"no edge here\", never \"knife edge everywhere\".  They cost nothing unless the body actually calls them.  CROSS-OBJECT PROXIMITY -- `proximity(radius)` and `interior(radius)`, the FOURTH and FIFTH signal builtins and the only two that look at the REST OF THE SCENE.  The three above ask the hit geometry about ITSELF; this one returns [0,1] with 1 = TOUCHING and 0 = nothing within `radius`, computed as `1 - d/radius` for `d` the shortest distance from the hit to the surface of any OTHER object.  It is the quantity contact grime actually is -- dirt collecting where a nail rests on a plank, dust where a wall meets a floor -- and it is NOT an occlusion: a thin object lying on a plane subtends only grazing directions, so `1 - occlusion(r)` reads a two-to-ten-pixel band there rather than a seam, which is why this exists as its own builtin rather than as a wider radius on that one.  The analogue is Unreal's DistanceToNearestSurface node and Houdini's xyzdist(), not Substance's AO baker.  ITS `radius` IS A WORLD LENGTH -- `proximity(0.002)` is 2 mm -- deliberately UNLIKE the other three, whose radius is a fraction of the hit object's own size: a fraction of the RECEIVER cannot describe how far away a NEIGHBOUR is, and authors already reason in world units for `fw` and feature sizes.  A literal radius <= 0 is a compile error naming that unit; a computed one that lands <= 0 returns the neutral.  TWO THINGS TO KNOW BEFORE AUTHORING WITH IT.  (1) IT CANNOT DRIVE RELIEF: `relief_modifier` holds the signal channel FIXED across its four taps by documented design, so a `proximity`-driven height expression has zero gradient and produces no displacement -- the seam is a colour and roughness signal.  (2) EMISSIVE OBJECTS NEVER COUNT: a `rect_light` panel parked millimetres off a wall must not paint grime on it, so any object whose material emits is skipped -- which also means a DECORATIVE emitter (a lava pool, a glowing rune) will not collect contact dirt either.  `casts_shadows FALSE` does NOT exempt a neighbour (this is geometry presence, not light visibility), a CSG composite's operands never count separately, two INSTANCED COPIES of one geometry do count against each other, and a point INSIDE another object reads 1 (interpenetration is contact).  Unlike the other three it works on EVERY receiver, including ones that publish no signal provider at all (a `box_geometry`, an infinite plane) -- what varies is which NEIGHBOURS can answer: the analytic primitives (plane, sphere, box, capped and open cylinder, disk, torus, and a clipped plane whose four corners are COPLANAR AND CONVEX -- every rect_light and every hand-authored panel) are exact, INDEXED TRIANGLE MESHES are exact too (every loader but RAW, every tessellated primitive, and `displaced_geometry`'s baked mesh -- answered by a bounded closest-point traversal of the mesh's own BVH, identical to a brute-force minimum over every triangle), the ellipsoid and `sdf_geometry` / `skeleton_geometry` are upper bounds (so the signal may UNDER-paint a seam and can never paint one that is not there), CSG COMPOSITES ANSWER TOO (a `union` reports the nearer of its operands; an `intersection` or a `subtraction` brackets the composed field and reports an upper bound), refusing only when an operand refuses -- an `intersection` or `subtraction` with a SHEET operand (a plane, a disk, an open cylinder, a mesh) refuses outright, since it cannot tell inside from outside there -- and RAW (non-indexed) meshes, patches, hair, heightfield-mode SDFs and non-convex or non-coplanar clipped planes contribute nothing and say so ONCE PER REFUSING OBJECT in the log, naming the chunk and its kind.  A MESH IS A SHEET for this query and every solid family is not: a point INSIDE a sphere, box, cylinder or SDF reads 1 (interpenetration is contact), while a point inside a closed MESH reads its honest distance to the nearest triangle -- a triangle soup carries no inside test, and reporting a distance rather than inventing one is the direction that can only under-paint.  HOW LOOSE THE BOUNDS ARE, since it changes the radius you author: an ECCENTRIC ELLIPSOID over-reports by up to its semi-axis ratio -- a 4:1 ellipsoid at a true distance of 2.75 reports 11.0 -- so against one you want roughly ratio-times the radius you actually mean, and an anisotropically SCALED object is bounded the same way (it says so in the log, with its factor).  `interior(radius)` IS THE SIGNED SIBLING, and the two together cover the signed distance without a sign convention to remember: it returns [0,1] with 0 = INSIDE NO NEIGHBOUR and 1 = at least `radius` deep inside one, computed as `depth/radius` for `depth` the deepest containment over every other object.  Its `radius` is a WORLD LENGTH too, and mandatory.  Reach for it when what you want to paint is BURIAL rather than contact -- the sunk shank of a nail, the embedded flank of a stone in mortar.  ONLY THE SOLID FAMILIES CONTRIBUTE TO IT: a sphere, box, capped cylinder, torus, ellipsoid, SDF or CSG composite of those can say whether a point is inside it, while every SHEET -- a plane, a disk, an open cylinder, a TRIANGLE MESH, a patch, hair -- cannot and contributes 0 silently (it would otherwise print a refusal for every mesh in the scene).  So a receiver buried inside a MESH neighbour reads `interior` 0 and `proximity` its honest distance to the nearest triangle.  ALL SIX SIGNALS (curv, occlusion, convexity, thickness, proximity, interior) ARE FULLY CORRECT UNDER PATH TRACING (the default rasterizer family); BDPT / VCM / MLT currently evaluate them as their neutral fallback in PARTS of their transport (the forward walk's own per-bounce re-evaluation, connection/NEE, MIS reverse-pdf, and more -- see docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md's Phase-2 Known-residual paragraph for the full list), which the renderer now surfaces as a one-time warning when such a render begins.  `occlusion` / `convexity` complement `curv`: `curv` is fine-scale and radius-free (a sharp edge), `occlusion` is radius-sampled and sees real cavities (a deep fold two bumps wide), `convexity` is the radius-sampled edge mask whose thresholds port between objects.  A scalar-typed body yields a uniform value; a vec3-typed body (x->R, y->G, z->B) yields a genuine per-channel triple, e.g. `vec3(ior_r, ior_g, ior_b)` for spatially-varying RGB dispersion.  No colorspace, no JH uplift, by construction."; }
						{ auto& p = P(); p.name = "param";      p.kind = ValueKind::String;     p.repeatable = true; p.description = "Companion to `expression`: named numeric constant `<name> <number> [min <a>] [max <b>] [step <s>] [label \"text\"]` (repeatable)"; }
						{ auto& p = P(); p.name = "def";        p.kind = ValueKind::String;     p.repeatable = true; p.description = "Companion to `expression`: named sub-expression `<name> <expr>` (repeatable, in order)"; }
						{ auto& p = P(); p.name = "seed";       p.kind = ValueKind::Double;     p.description = "Companion to `expression`: auto-registered named scalar constant `seed`, for deterministic per-instance variation"; p.defaultValueHint = "0.0"; }
						{ auto& p = P(); p.name = "painter";    p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "Named COLOUR painter (any of the 36 kinds, or expression_painter) whose channel sources the scalar -- out = bias + scale * channel(source) (form 12: PainterChannelScalarPainter, the P2.1 any-painter bridge).  CAVEAT: reads a POST-COLORSPACE value (source.GetColor/GetAlpha), same as PainterToScalarAdapter -- fine for procedural masks/fields, not a spectral-fidelity path for a wavelength-varying source."; p.semantics.pipe = ParameterPipe::Color; p.semantics.note = "the any-painter -> scalar bridge (P2.1): binds ANY Color-pipe chunk (all 36 kinds, expression_painter included), channel-select R/G/B/A. Unlike `texture`, no keyword allowlist."; }
						return cd;
					}();
					return d;
				}
			};

			// Shared helper: parse a wrap-mode chunk parameter (U or V axis)
			// into RISE's char encoding (0 = clamp, 1 = repeat, 2 = mirrored
			// repeat).  Accepted strings are exactly the three values the
			// matching ChunkDescriptor advertises (`clamp`, `repeat`,
			// `mirrored_repeat`) — no aliases, so external tooling that
			// reads the descriptor's enumValues to drive autocomplete /
			// validation sees the same set the parser accepts.
			//
			// Returns 0 (clamp) when the parameter is absent so existing
			// scenes that didn't specify wrap render byte-identically to
			// the pre-2026-05-01 clamp-to-edge default.  Logs and returns
			// false on an unrecognised string so users see the typo
			// instead of getting silent clamp.  Used by all 5 texture-
			// painter chunks (png / jpg / hdr / exr / tiff) so the surface
			// stays consistent.
			static inline bool ParseWrapModeParam(
				const ParseStateBag& bag,
				const char*          paramName,
				char&                outWrap )
			{
				outWrap = 0;	// eRasterWrap_ClampToEdge (legacy default)
				if( !bag.Has( paramName ) ) return true;
				std::string w = bag.GetString( paramName );
				if(      w == "clamp" )            outWrap = 0;
				else if( w == "repeat" )           outWrap = 1;
				else if( w == "mirrored_repeat" )  outWrap = 2;
				else {
					GlobalLog()->PrintEx( eLog_Error,
						"ChunkParser:: Unknown wrap mode `%s` on parameter `%s` "
						"(expected one of: clamp, repeat, mirrored_repeat)",
						w.c_str(), paramName );
					return false;
				}
				return true;
			}

			// Adds the wrap_s / wrap_t parameter descriptors to a
			// ChunkDescriptor so every painter chunk advertises the same
			// names + accepted values.  Default value hint is "clamp" to
			// match the runtime default + preserve the legacy contract for
			// scenes that don't set wrap.
			static inline void AddWrapModeParams( ChunkDescriptor& cd )
			{
				// Matches the per-chunk pattern: emplace_back() then take
				// a reference via .back().  Older codebases without
				// C++17's emplace_back-returns-reference compile cleanly
				// this way.
				{ cd.parameters.emplace_back(); ParameterDescriptor& p = cd.parameters.back(); p.name = "wrap_s"; p.kind = ValueKind::Enum; p.enumValues = {"clamp","repeat","mirrored_repeat"}; p.description = "Address-wrap mode for the U axis (S in glTF terminology) when sampling outside [0,1].  `repeat` matches glTF's default and lets tiled atlases (brick / wood / plaster, NewSponza floor) sample correctly across the full image; `clamp` saturates at the edge texel (legacy / default for non-glTF scenes); `mirrored_repeat` reflects across each integer crossing for seam-free alternating tiles."; p.defaultValueHint = "clamp"; }
				{ cd.parameters.emplace_back(); ParameterDescriptor& p = cd.parameters.back(); p.name = "wrap_t"; p.kind = ValueKind::Enum; p.enumValues = {"clamp","repeat","mirrored_repeat"}; p.description = "Address-wrap mode for the V axis (T in glTF terminology); see wrap_s for semantics."; p.defaultValueHint = "clamp"; }
			}

			// Shared helper: parse a filter_type chunk parameter into RISE's
			// char encoding for RasterImageAccessorFromChar (0 = nearest-
			// neighbour, 1 = bilinear, 2 = Catmull-Rom bicubic, 3 = uniform
			// B-spline bicubic) -- the four filter modes the runtime actually
			// implements.  The user-facing names are the lowercase forms the
			// matching ChunkDescriptor advertises; the PascalCase forms (NNB /
			// Bilinear / CatmullRom / UniformBSpline) are kept as backward-
			// compatible aliases because they were the ONLY strings the pre-
			// reconciliation Finalize accepted, so every pre-existing scene
			// that successfully set filter_type uses them.
			//
			// Returns 1 (bilinear) when the parameter is absent so scenes that
			// don't specify a filter render byte-identically to the historical
			// default.  Logs and returns false on an unrecognised string so
			// authors see the typo instead of a silent fallback.  Used by all 5
			// texture-painter chunks (png / jpg / hdr / exr / tiff) so the
			// surface stays consistent and descriptor/Finalize cannot drift.
			static inline bool ParseFilterTypeParam(
				const ParseStateBag& bag,
				char&                outFilter )
			{
				outFilter = 1;	// bilinear (historical default)
				if( !bag.Has( "filter_type" ) ) return true;
				std::string ft = bag.GetString( "filter_type" );
				if(      ft == "nearest"       || ft == "NNB" )            outFilter = 0;
				else if( ft == "bilinear"      || ft == "Bilinear" )       outFilter = 1;
				else if( ft == "catmull-rom"   || ft == "CatmullRom" )     outFilter = 2;
				else if( ft == "cubic-bspline" || ft == "UniformBSpline" ) outFilter = 3;
				else {
					GlobalLog()->PrintEx( eLog_Error,
						"ChunkParser:: Unknown filter type `%s` "
						"(expected one of: nearest, bilinear, catmull-rom, cubic-bspline)",
						ft.c_str() );
					return false;
				}
				return true;
			}

			// Adds the filter_type parameter descriptor to a ChunkDescriptor so
			// every image-painter chunk advertises the same names + accepted
			// values.  Only the four filter modes RasterImageAccessorFromChar
			// actually implements are advertised -- the earlier {box, gaussian}
			// entries had no backing accessor and hard-failed in Finalize,
			// making them un-authorable.  Mirror of AddWrapModeParams.
			static inline void AddFilterTypeParam( ChunkDescriptor& cd )
			{
				cd.parameters.emplace_back();
				ParameterDescriptor& p = cd.parameters.back();
				p.name = "filter_type";
				p.kind = ValueKind::Enum;
				p.enumValues = {"nearest","bilinear","catmull-rom","cubic-bspline"};
				p.description = "Texture filter";
				p.defaultValueHint = "bilinear";
			}

			struct PngPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name     = bag.GetString( "name",     "noname" );
					std::string filename = bag.GetString( "file",     "none" );
					bool lowmemory       = bag.GetBool(   "lowmemory", false );
					double scale[3] = {1,1,1};
					double shift[3] = {0,0,0};
					bag.GetVec3( "scale", scale );
					bag.GetVec3( "shift", shift );

					char color_space = 1;
					if( bag.Has( "color_space" ) ) {
						std::string cs = bag.GetString( "color_space" );
						if(      cs == "Rec709RGB_Linear" ) color_space = 0;
						else if( cs == "sRGB" )             color_space = 1;
						else if( cs == "ROMMRGB_Linear" )   color_space = 2;
						else if( cs == "ProPhotoRGB" )      color_space = 3;
						else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Unknown color space `%s`", cs.c_str() );
							return false;
						}
					}

					char filter_type = 1;
					if( !ParseFilterTypeParam( bag, filter_type ) ) return false;

					char wrap_s = 0, wrap_t = 0;
					if( !ParseWrapModeParam( bag, "wrap_s", wrap_s ) ) return false;
					if( !ParseWrapModeParam( bag, "wrap_t", wrap_t ) ) return false;

					return pJob.AddPNGTexturePainter( name.c_str(), filename.c_str(), color_space, filter_type, lowmemory, scale, shift, wrap_s, wrap_t );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "png_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Texture painter that loads a PNG image, sampled at the surface UV (2D domain).  `color_space` selects how the file's values are interpreted -- Rec709RGB_Linear is the verbatim-store idiom for non-colour data such as normal or mask maps.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "file";        p.kind = ValueKind::Filename;   p.description = "PNG file path"; }
						{ auto& p = P(); p.name = "color_space"; p.kind = ValueKind::Enum;       p.enumValues = {"sRGB","Rec709RGB_Linear","ROMMRGB_Linear","ProPhotoRGB"}; p.description = "Source colour space"; p.defaultValueHint = "sRGB"; }
						AddFilterTypeParam( cd );
						{ auto& p = P(); p.name = "lowmemory";   p.kind = ValueKind::Bool;       p.description = "Lower memory footprint (8-bit in-core)"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "scale";       p.kind = ValueKind::DoubleVec3; p.description = "R G B scale multipliers"; p.defaultValueHint = "1 1 1"; }
						{ auto& p = P(); p.name = "shift";       p.kind = ValueKind::DoubleVec3; p.description = "R G B additive shift"; p.defaultValueHint = "0 0 0"; }
						AddWrapModeParams( cd );
						return cd;
					}();
					return d;
				}
			};

			struct JpegPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name     = bag.GetString( "name",     "noname" );
					std::string filename = bag.GetString( "file",     "none" );
					bool lowmemory       = bag.GetBool(   "lowmemory", false );
					double scale[3] = {1,1,1};
					double shift[3] = {0,0,0};
					bag.GetVec3( "scale", scale );
					bag.GetVec3( "shift", shift );

					char color_space = 1;
					if( bag.Has( "color_space" ) ) {
						std::string cs = bag.GetString( "color_space" );
						if(      cs == "Rec709RGB_Linear" ) color_space = 0;
						else if( cs == "sRGB" )             color_space = 1;
						else if( cs == "ROMMRGB_Linear" )   color_space = 2;
						else if( cs == "ProPhotoRGB" )      color_space = 3;
						else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Unknown color space `%s`", cs.c_str() );
							return false;
						}
					}

					char filter_type = 1;
					if( !ParseFilterTypeParam( bag, filter_type ) ) return false;

					char wrap_s = 0, wrap_t = 0;
					if( !ParseWrapModeParam( bag, "wrap_s", wrap_s ) ) return false;
					if( !ParseWrapModeParam( bag, "wrap_t", wrap_t ) ) return false;

					return pJob.AddJPEGTexturePainter( name.c_str(), filename.c_str(), color_space, filter_type, lowmemory, scale, shift, wrap_s, wrap_t );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "jpg_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Texture painter that loads a JPEG image, sampled at the surface UV (2D domain).  See png_painter for the `color_space` note.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "file";        p.kind = ValueKind::Filename;   p.description = "JPEG file path"; }
						{ auto& p = P(); p.name = "color_space"; p.kind = ValueKind::Enum;       p.enumValues = {"sRGB","Rec709RGB_Linear","ROMMRGB_Linear","ProPhotoRGB"}; p.description = "Source colour space"; p.defaultValueHint = "sRGB"; }
						AddFilterTypeParam( cd );
						{ auto& p = P(); p.name = "lowmemory";   p.kind = ValueKind::Bool;       p.description = "Lower memory footprint (8-bit in-core)"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "scale";       p.kind = ValueKind::DoubleVec3; p.description = "R G B scale multipliers"; p.defaultValueHint = "1 1 1"; }
						{ auto& p = P(); p.name = "shift";       p.kind = ValueKind::DoubleVec3; p.description = "R G B additive shift"; p.defaultValueHint = "0 0 0"; }
						AddWrapModeParams( cd );
						return cd;
					}();
					return d;
				}
			};

			struct HdrPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name     = bag.GetString( "name",     "noname" );
					std::string filename = bag.GetString( "file",     "none" );
					bool lowmemory       = bag.GetBool(   "lowmemory", false );
					double scale[3] = {1,1,1};
					double shift[3] = {0,0,0};
					bag.GetVec3( "scale", scale );
					bag.GetVec3( "shift", shift );

					char filter_type = 1;
					if( !ParseFilterTypeParam( bag, filter_type ) ) return false;

					char wrap_s = 0, wrap_t = 0;
					if( !ParseWrapModeParam( bag, "wrap_s", wrap_s ) ) return false;
					if( !ParseWrapModeParam( bag, "wrap_t", wrap_t ) ) return false;

					return pJob.AddHDRTexturePainter( name.c_str(), filename.c_str(), filter_type, lowmemory, scale, shift, wrap_s, wrap_t );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "hdr_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Texture painter that loads a Radiance HDR image, sampled at the surface UV (2D domain).  The usual source for a rasterizer `radiance_map` environment dome.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "file";        p.kind = ValueKind::Filename;   p.description = "HDR file path"; }
						AddFilterTypeParam( cd );
						{ auto& p = P(); p.name = "lowmemory";   p.kind = ValueKind::Bool;       p.description = "Lower memory footprint"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "scale";       p.kind = ValueKind::DoubleVec3; p.description = "R G B scale"; p.defaultValueHint = "1 1 1"; }
						{ auto& p = P(); p.name = "shift";       p.kind = ValueKind::DoubleVec3; p.description = "R G B shift"; p.defaultValueHint = "0 0 0"; }
						AddWrapModeParams( cd );
						return cd;
					}();
					return d;
				}
			};

			struct ExrPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name     = bag.GetString( "name",     "noname" );
					std::string filename = bag.GetString( "file",     "none" );
					bool lowmemory       = bag.GetBool(   "lowmemory", false );
					double scale[3] = {1,1,1};
					double shift[3] = {0,0,0};
					bag.GetVec3( "scale", scale );
					bag.GetVec3( "shift", shift );

					char color_space = 0;
					if( bag.Has( "color_space" ) ) {
						std::string cs = bag.GetString( "color_space" );
						if(      cs == "Rec709RGB_Linear" ) color_space = 0;
						else if( cs == "sRGB" )             color_space = 1;
						else if( cs == "ROMMRGB_Linear" )   color_space = 2;
						else if( cs == "ProPhotoRGB" )      color_space = 3;
						else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Unknown color space `%s`", cs.c_str() );
							return false;
						}
					}

					char filter_type = 1;
					if( !ParseFilterTypeParam( bag, filter_type ) ) return false;

					char wrap_s = 0, wrap_t = 0;
					if( !ParseWrapModeParam( bag, "wrap_s", wrap_s ) ) return false;
					if( !ParseWrapModeParam( bag, "wrap_t", wrap_t ) ) return false;

					return pJob.AddEXRTexturePainter( name.c_str(), filename.c_str(), color_space, filter_type, lowmemory, scale, shift, wrap_s, wrap_t );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "exr_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Texture painter that loads an OpenEXR image, sampled at the surface UV (2D domain).  Also the usual high-dynamic-range source for a rasterizer `radiance_map` environment dome.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "file";        p.kind = ValueKind::Filename;   p.description = "EXR file path"; }
						{ auto& p = P(); p.name = "color_space"; p.kind = ValueKind::Enum;       p.enumValues = {"sRGB","Rec709RGB_Linear","ROMMRGB_Linear","ProPhotoRGB"}; p.description = "Source colour space"; p.defaultValueHint = "Rec709RGB_Linear"; }
						AddFilterTypeParam( cd );
						{ auto& p = P(); p.name = "lowmemory";   p.kind = ValueKind::Bool;       p.description = "Lower memory footprint"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "scale";       p.kind = ValueKind::DoubleVec3; p.description = "R G B scale"; p.defaultValueHint = "1 1 1"; }
						{ auto& p = P(); p.name = "shift";       p.kind = ValueKind::DoubleVec3; p.description = "R G B shift"; p.defaultValueHint = "0 0 0"; }
						AddWrapModeParams( cd );
						return cd;
					}();
					return d;
				}
			};

			struct TiffPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name     = bag.GetString( "name",     "noname" );
					std::string filename = bag.GetString( "file",     "none" );
					bool lowmemory       = bag.GetBool(   "lowmemory", false );
					double scale[3] = {1,1,1};
					double shift[3] = {0,0,0};
					bag.GetVec3( "scale", scale );
					bag.GetVec3( "shift", shift );

					char color_space = 1;
					if( bag.Has( "color_space" ) ) {
						std::string cs = bag.GetString( "color_space" );
						if(      cs == "Rec709RGB_Linear" ) color_space = 0;
						else if( cs == "sRGB" )             color_space = 1;
						else if( cs == "ROMMRGB_Linear" )   color_space = 2;
						else if( cs == "ProPhotoRGB" )      color_space = 3;
						else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Unknown color space `%s`", cs.c_str() );
							return false;
						}
					}

					char filter_type = 1;
					if( !ParseFilterTypeParam( bag, filter_type ) ) return false;

					char wrap_s = 0, wrap_t = 0;
					if( !ParseWrapModeParam( bag, "wrap_s", wrap_s ) ) return false;
					if( !ParseWrapModeParam( bag, "wrap_t", wrap_t ) ) return false;

					return pJob.AddTIFFTexturePainter( name.c_str(), filename.c_str(), color_space, filter_type, lowmemory, scale, shift, wrap_s, wrap_t );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "tiff_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Texture painter that loads a TIFF image, sampled at the surface UV (2D domain).  See png_painter for the `color_space` note.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "file";        p.kind = ValueKind::Filename;   p.description = "TIFF file path"; }
						{ auto& p = P(); p.name = "color_space"; p.kind = ValueKind::Enum;       p.enumValues = {"sRGB","Rec709RGB_Linear","ROMMRGB_Linear","ProPhotoRGB"}; p.description = "Source colour space"; p.defaultValueHint = "sRGB"; }
						AddFilterTypeParam( cd );
						{ auto& p = P(); p.name = "lowmemory";   p.kind = ValueKind::Bool;       p.description = "Lower memory footprint"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "scale";       p.kind = ValueKind::DoubleVec3; p.description = "R G B scale"; p.defaultValueHint = "1 1 1"; }
						{ auto& p = P(); p.name = "shift";       p.kind = ValueKind::DoubleVec3; p.description = "R G B shift"; p.defaultValueHint = "0 0 0"; }
						AddWrapModeParams( cd );
						return cd;
					}();
					return d;
				}
			};


			struct CheckerPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name   = bag.GetString( "name",   "noname" );
					std::string colora = bag.GetString( "colora", "none" );
					std::string colorb = bag.GetString( "colorb", "none" );
					double size        = bag.GetDouble( "size",   1.0 );

					return pJob.AddCheckerPainter( name.c_str(), size, colora.c_str(), colorb.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "checker_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Two-colour checkerboard in the surface UV (2D domain).  Deliberately synthetic: right for test / reference surfaces and tiled floors, wrong for material realism -- use a noise painter for that.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";   p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "First colour (painter)"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "colorb"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Second colour (painter)"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "size";   p.kind = ValueKind::Double;    p.description = "Checker cell size"; p.defaultValueHint = "1.0"; }
						return cd;
					}();
					return d;
				}
			};

			struct LinesPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name   = bag.GetString( "name",     "noname" );
					std::string colora = bag.GetString( "colora",   "none" );
					std::string colorb = bag.GetString( "colorb",   "none" );
					double size        = bag.GetDouble( "size",     1.0 );
					bool vertical      = bag.GetBool(   "vertical", false );

					return pJob.AddLinesPainter( name.c_str(), size, colora.c_str(), colorb.c_str(), vertical );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "lines_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Two-colour stripe field in the surface UV (2D domain); `vertical` picks the axis.  Synthetic like checker_painter -- for real fabric weave or wood grain use gabor3d_painter / turbulence3d_painter instead.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";     p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "First colour (painter)"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "colorb";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Second colour (painter)"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "size";     p.kind = ValueKind::Double;    p.description = "Stripe width"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "vertical"; p.kind = ValueKind::Bool;      p.description = "Vertical (vs horizontal) stripes"; p.defaultValueHint = "FALSE"; }
						return cd;
					}();
					return d;
				}
			};

			struct MandelbrotPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name   = bag.GetString( "name",   "noname" );
					std::string colora = bag.GetString( "colora", "none" );
					std::string colorb = bag.GetString( "colorb", "none" );
					double xstart      = bag.GetDouble( "xstart", 0.0 );
					double xend        = bag.GetDouble( "xend",   1.0 );
					double ystart      = bag.GetDouble( "ystart", 0.0 );
					double yend        = bag.GetDouble( "yend",   1.0 );
					// Legacy parser used toUInt() on the exponent value
					// despite holding it in a double — preserve the
					// truncation so backwards behaviour is identical.
					double exponent    = 12.0;
					if( bag.Has( "exponent" ) ) exponent = static_cast<double>( bag.GetUInt( "exponent" ) );

					return pJob.AddMandelbrotFractalPainter( name.c_str(), colora.c_str(), colorb.c_str(), xstart, xend, ystart, yend, exponent );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "mandelbrot_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Mandelbrot escape-time fractal in the surface UV (2D domain), colora -> colorb by iteration count.  An abstract-art surface, not a material.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";     p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Inside-set colour"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "colorb";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Outside-set colour"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "xstart";   p.kind = ValueKind::Double;    p.description = "Real-axis start"; p.defaultValueHint = "-2.0"; }
						{ auto& p = P(); p.name = "xend";     p.kind = ValueKind::Double;    p.description = "Real-axis end";   p.defaultValueHint = "2.0"; }
						{ auto& p = P(); p.name = "ystart";   p.kind = ValueKind::Double;    p.description = "Imag-axis start"; p.defaultValueHint = "-2.0"; }
						{ auto& p = P(); p.name = "yend";     p.kind = ValueKind::Double;    p.description = "Imag-axis end";   p.defaultValueHint = "2.0"; }
						{ auto& p = P(); p.name = "exponent"; p.kind = ValueKind::Double;    p.description = "Iteration exponent"; p.defaultValueHint = "2.0"; }
						return cd;
					}();
					return d;
				}
			};

			struct Perlin2DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",        "noname" );
					std::string colora      = bag.GetString( "colora",      "none" );
					std::string colorb      = bag.GetString( "colorb",      "none" );
					double persistence      = bag.GetDouble( "persistence", 1.0 );
					unsigned int octaves    = bag.GetUInt(   "octaves",     4 );
					double scale[2] = {1.0,1.0};
					double shift[2] = {0,0};
					if( bag.Has( "scale" ) ) sscanf( bag.GetString( "scale" ).c_str(), "%lf %lf", &scale[0], &scale[1] );
					if( bag.Has( "shift" ) ) sscanf( bag.GetString( "shift" ).c_str(), "%lf %lf", &shift[0], &shift[1] );

					return pJob.AddPerlin2DPainter( name.c_str(), persistence, octaves, colora.c_str(), colorb.c_str(), scale, shift );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "perlin2d_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Perlin fBm noise in the surface UV (2D domain -- follows the UV parameterisation, so it stretches with UV distortion and can show seams).  Smooth cloudy variation from colora (low) to colorb (high).  Prefer perlin3d_painter when the surface should look CARVED OUT of a solid material.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddNoisePainterCommonParams( P );
						return cd;
					}();
					return d;
				}
			};

			struct ControlledSmoothness2DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",        "noname" );
					std::string colora      = bag.GetString( "colora",      "none" );
					std::string colorb      = bag.GetString( "colorb",      "none" );
					double radius           = bag.GetDouble( "radius",      0.5 );
					double amplitude        = bag.GetDouble( "amplitude",   1.0 );
					unsigned int mode       = bag.GetUInt(   "smoothness",  3 );	// default: cubic Hermite
					double center[2] = { 0.5, 0.5 };
					if( bag.Has( "center" ) ) sscanf( bag.GetString( "center" ).c_str(), "%lf %lf", &center[0], &center[1] );

					return pJob.AddControlledSmoothness2DPainter(
						name.c_str(), colora.c_str(), colorb.c_str(),
						center[0], center[1], radius, amplitude, mode );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "controlled_smoothness2d_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Test painter: a single radial bump with controllable boundary smoothness order.  Use as a `displaced_geometry` displacement to isolate per-edge C¹ jump effects on SMS Newton convergence.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";       p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Low/zero-end color"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "colorb";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "High/peak-end color"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "center";     p.kind = ValueKind::DoubleVec3;p.description = "Bump center in UV space (only first two components used)"; p.defaultValueHint = "0.5 0.5"; }
						{ auto& p = P(); p.name = "radius";     p.kind = ValueKind::Double;    p.description = "Bump radius in UV space"; p.defaultValueHint = "0.5"; }
						{ auto& p = P(); p.name = "amplitude";  p.kind = ValueKind::Double;    p.description = "Peak height"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "smoothness"; p.kind = ValueKind::UInt;      p.description = "Boundary smoothness order: 0=Heaviside, 1=Tent, 2=Quadratic, 3=Cubic, 5=Quintic, 99=Gaussian"; p.defaultValueHint = "3"; }
						return cd;
					}();
					return d;
				}
			};

			struct GerstnerWavePainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name           = bag.GetString( "name",               "noname" );
					std::string colora         = bag.GetString( "colora",             "none" );
					std::string colorb         = bag.GetString( "colorb",             "none" );
					unsigned int numWaves      = bag.GetUInt(   "num_waves",          12 );
					double medianWavelength    = bag.GetDouble( "median_wavelength",  0.25 );
					double wavelengthRange     = bag.GetDouble( "wavelength_range",   3.0 );
					double medianAmplitude     = bag.GetDouble( "median_amplitude",   0.05 );
					double amplitudePower      = bag.GetDouble( "amplitude_power",    1.0 );
					double directionalSpread   = bag.GetDouble( "directional_spread", 0.5 );
					double dispersionSpeed     = bag.GetDouble( "dispersion_speed",   1.0 );
					unsigned int seed          = bag.GetUInt(   "seed",               42 );
					double time                = bag.GetDouble( "time",               0.0 );
					double windDir[2] = {1.0, 0.0};
					if( bag.Has( "wind_dir" ) ) sscanf( bag.GetString( "wind_dir" ).c_str(), "%lf %lf", &windDir[0], &windDir[1] );

					return pJob.AddGerstnerWavePainter(
						name.c_str(),
						colora.c_str(), colorb.c_str(),
						numWaves,
						medianWavelength, wavelengthRange,
						medianAmplitude, amplitudePower,
						windDir,
						directionalSpread,
						dispersionSpeed,
						seed,
						time );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "gerstnerwave_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "A sum of `num_waves` Gerstner waves in the surface UV (2D domain) -- the water-surface crest/trough field, steered by `wind_dir` / `directional_spread` and animated by `time`.  Most useful as a displaced_geometry displacement (real waves) with colora/colorb reading trough vs crest.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";                p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora";              p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Trough colour"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "colorb";              p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Crest colour"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "num_waves";           p.kind = ValueKind::UInt;      p.description = "Number of wave components (fewer + longer wavelengths reads as a calmer swell)"; p.defaultValueHint = "12"; }
						{ auto& p = P(); p.name = "median_wavelength";   p.kind = ValueKind::Double;    p.description = "Median wavelength, in UV units"; p.defaultValueHint = "0.25"; }
						{ auto& p = P(); p.name = "wavelength_range";    p.kind = ValueKind::Double;    p.description = "Ratio spread of wavelengths around the median"; p.defaultValueHint = "3.0"; }
						{ auto& p = P(); p.name = "median_amplitude";    p.kind = ValueKind::Double;    p.description = "Median wave amplitude"; p.defaultValueHint = "0.05"; }
						{ auto& p = P(); p.name = "amplitude_power";     p.kind = ValueKind::Double;    p.description = "Amplitude falloff exponent across the wave set"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "wind_dir";            p.kind = ValueKind::DoubleVec3;p.description = "Wind direction in UV space (only the first two components are used)"; p.defaultValueHint = "1 0 0"; }
						{ auto& p = P(); p.name = "directional_spread";  p.kind = ValueKind::Double;    p.description = "Angular spread about wind_dir (0 = a single travel direction)"; p.defaultValueHint = "0.5"; }
						{ auto& p = P(); p.name = "dispersion_speed";    p.kind = ValueKind::Double;    p.description = "Dispersion coefficient (only matters when `time` animates)"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "seed";                p.kind = ValueKind::UInt;      p.description = "RNG seed for the wave set"; p.defaultValueHint = "42"; }
						{ auto& p = P(); p.name = "time";                p.kind = ValueKind::Double;    p.description = "Animation time (keyframe this for moving water)"; p.defaultValueHint = "0.0"; }
						return cd;
					}();
					return d;
				}
			};

			struct PolynomialFunction2DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				// Map user-facing type string → API integer.  Unknown
				// strings fall through to 0 (radial_bump) with a
				// warning, matching the API layer's guard.
				static unsigned int ParseType( const std::string& s )
				{
					if( s == "radial_bump" )       return 0;
					if( s == "monomial" )          return 1;
					if( s == "paraboloid" )        return 2;
					if( s == "hyperbolic_saddle" ) return 3;
					if( s == "monkey_saddle" )     return 4;
					if( s == "bivariate" )         return 5;
					GlobalLog()->PrintEx( eLog_Warning,
						"polynomial_function2d_painter: unknown type '%s'; defaulting to 'radial_bump'",
						s.c_str() );
					return 0;
				}

				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name      = bag.GetString( "name",      "noname" );
					std::string colora    = bag.GetString( "colora",    "none" );
					std::string colorb    = bag.GetString( "colorb",    "none" );
					std::string typeStr   = bag.GetString( "type",      "radial_bump" );
					double amplitude      = bag.GetDouble( "amplitude", 1.0 );
					unsigned int degree   = bag.GetUInt(   "degree",    2 );
					unsigned int powerX   = bag.GetUInt(   "power_x",   0 );
					unsigned int powerY   = bag.GetUInt(   "power_y",   0 );

					// Defaults: centre and scale map [0,1]² UV → [-1, 1]²,
					// the natural domain for unit-form polynomials.
					double center[2] = { 0.5, 0.5 };
					double scale[2]  = { 0.5, 0.5 };
					if( bag.Has( "center" ) ) sscanf( bag.GetString( "center" ).c_str(), "%lf %lf", &center[0], &center[1] );
					if( bag.Has( "scale" )  ) sscanf( bag.GetString( "scale"  ).c_str(), "%lf %lf", &scale[0],  &scale[1]  );

					// Bivariate coefficients: space-separated doubles.
					// Token-by-token parse so the user may supply any
					// length, with implicit zero-fill / clipping per
					// the API contract.
					std::vector<double> coeffs;
					if( bag.Has( "coefficients" ) ) {
						const std::string& s = bag.GetString( "coefficients" );
						std::istringstream iss( s );
						double v;
						while( iss >> v ) {
							coeffs.push_back( v );
						}
					}

					const unsigned int polynomialType = ParseType( typeStr );

					return pJob.AddPolynomialFunction2DPainter(
						name.c_str(),
						colora.c_str(), colorb.c_str(),
						polynomialType,
						center, scale,
						amplitude,
						degree, powerX, powerY,
						coeffs.empty() ? nullptr : coeffs.data(),
						static_cast<unsigned int>( coeffs.size() ) );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "polynomial_function2d_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Polynomial-based Function2D painter.  Evaluates a polynomial in normalised coords ((u−center.u)/scale.u, (v−center.v)/scale.v); the `type` selects one of: radial_bump (compact-support bump), monomial (single term x^px·y^py), paraboloid, hyperbolic_saddle, monkey_saddle, or bivariate (general).  Drives `displaced_geometry` and texture materials alike.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";         p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora";       p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Zero/low-end colour"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "colorb";       p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Positive/peak-end colour"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "type";         p.kind = ValueKind::String;    p.description = "Polynomial family: radial_bump | monomial | paraboloid | hyperbolic_saddle | monkey_saddle | bivariate"; p.defaultValueHint = "radial_bump"; }
						{ auto& p = P(); p.name = "center";       p.kind = ValueKind::DoubleVec3;p.description = "(U, V) origin for the normalised coordinates"; p.defaultValueHint = "0.5 0.5"; }
						{ auto& p = P(); p.name = "scale";        p.kind = ValueKind::DoubleVec3;p.description = "(U, V) divisor: x = (u − center.u)/scale.u"; p.defaultValueHint = "0.5 0.5"; }
						{ auto& p = P(); p.name = "amplitude";    p.kind = ValueKind::Double;    p.description = "Global multiplier"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "degree";       p.kind = ValueKind::UInt;      p.description = "Degree for radial_bump (smoothness exponent) and bivariate (max total degree)"; p.defaultValueHint = "2"; }
						{ auto& p = P(); p.name = "power_x";      p.kind = ValueKind::UInt;      p.description = "x exponent for monomial type"; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "power_y";      p.kind = ValueKind::UInt;      p.description = "y exponent for monomial type"; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "coefficients"; p.kind = ValueKind::String;    p.description = "Bivariate coefficients, space-separated, row-major triangular order: a00, a10, a01, a20, a11, a02, a30, a21, a12, a03, ... (a_ij = coefficient of x^i y^j).  Length up to (degree+1)(degree+2)/2; shorter is OK (rest implicitly zero)."; p.defaultValueHint = ""; }
						return cd;
					}();
					return d;
				}
			};

			struct CompositeFunction2DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				// Map the user-facing op string to the integer the API
				// expects.  Unknown values resolve to 0 (Sum) with a
				// warning at Finalize time; the API layer also guards.
				static unsigned int ParseOp( const std::string& s )
				{
					if( s == "sum" )        return 0;
					if( s == "product" )    return 1;
					if( s == "lerp" )       return 2;
					if( s == "max" )        return 3;
					if( s == "min" )        return 4;
					if( s == "difference" ) return 5;
					GlobalLog()->PrintEx( eLog_Warning,
						"composite_function2d_painter: unknown op '%s'; defaulting to 'sum'",
						s.c_str() );
					return 0;
				}

				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name         = bag.GetString( "name",          "noname" );
					std::string colora       = bag.GetString( "colora",        "none" );
					std::string colorb       = bag.GetString( "colorb",        "none" );
					std::string childA       = bag.GetString( "child_a",       "none" );
					std::string childB       = bag.GetString( "child_b",       "none" );
					std::string opStr        = bag.GetString( "op",            "sum" );
					double weightA           = bag.GetDouble( "weight_a",      1.0 );
					double weightB           = bag.GetDouble( "weight_b",      1.0 );
					double lerpT             = bag.GetDouble( "lerp_t",        0.5 );
					double outputScale       = bag.GetDouble( "output_scale",  1.0 );
					double outputOffset      = bag.GetDouble( "output_offset", 0.0 );

					// (U, V) affine transform per operand.  Default
					// scale 1.0 1.0 and offset 0 0 = identity, so the
					// minimal-config case (just `op`, `child_a`,
					// `child_b`) Just Works.
					double uvScaleA[2]  = { 1.0, 1.0 };
					double uvOffsetA[2] = { 0.0, 0.0 };
					double uvScaleB[2]  = { 1.0, 1.0 };
					double uvOffsetB[2] = { 0.0, 0.0 };
					if( bag.Has( "uv_scale_a"  ) ) sscanf( bag.GetString( "uv_scale_a"  ).c_str(), "%lf %lf", &uvScaleA[0],  &uvScaleA[1]  );
					if( bag.Has( "uv_offset_a" ) ) sscanf( bag.GetString( "uv_offset_a" ).c_str(), "%lf %lf", &uvOffsetA[0], &uvOffsetA[1] );
					if( bag.Has( "uv_scale_b"  ) ) sscanf( bag.GetString( "uv_scale_b"  ).c_str(), "%lf %lf", &uvScaleB[0],  &uvScaleB[1]  );
					if( bag.Has( "uv_offset_b" ) ) sscanf( bag.GetString( "uv_offset_b" ).c_str(), "%lf %lf", &uvOffsetB[0], &uvOffsetB[1] );

					const unsigned int op = ParseOp( opStr );

					return pJob.AddCompositeFunction2DPainter(
						name.c_str(),
						colora.c_str(), colorb.c_str(),
						childA.c_str(), childB.c_str(),
						op,
						weightA, uvScaleA, uvOffsetA,
						weightB, uvScaleB, uvOffsetB,
						lerpT,
						outputScale, outputOffset );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "composite_function2d_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Composable Function2D painter: combines two operand Function2Ds per a binary operator (sum/product/lerp/max/min/difference), with per-operand weight + (u,v) affine transform and a global output remap.  Used for multi-scale displacement and procedural texture composition.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";          p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "op";            p.kind = ValueKind::String;    p.description = "Binary operator: sum | product | lerp | max | min | difference"; p.defaultValueHint = "sum"; }
						{ auto& p = P(); p.name = "colora";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Low-value color painter"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "colorb";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "High-value color painter"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "child_a";       p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter, ChunkCategory::Function}; p.description = "First operand Function2D -- resolved via pFunc2DManager (Job::AddCompositeFunction2DPainter), which accepts any colour painter EXCEPT expression_painter (single-registered) / scalar_painter (never registered there), PLUS a genuine `function`-category IFunction2D (piecewise_linear_function2d), hence the {Painter, Function} pair"; p.semantics.pipe = ParameterPipe::Function2D; }
						{ auto& p = P(); p.name = "child_b";       p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter, ChunkCategory::Function}; p.description = "Second operand Function2D -- same accepted-kind rule as `child_a` (resolved via the same pFunc2DManager lookup)"; p.semantics.pipe = ParameterPipe::Function2D; }
						{ auto& p = P(); p.name = "weight_a";      p.kind = ValueKind::Double;    p.description = "Scalar multiplier applied to A before the operator"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "weight_b";      p.kind = ValueKind::Double;    p.description = "Scalar multiplier applied to B before the operator"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "uv_scale_a";    p.kind = ValueKind::DoubleVec3;p.description = "(U, V) scale applied to (u,v) before sampling A (only first two components used)"; p.defaultValueHint = "1.0 1.0"; }
						{ auto& p = P(); p.name = "uv_offset_a";   p.kind = ValueKind::DoubleVec3;p.description = "(U, V) offset applied to (u,v) before sampling A"; p.defaultValueHint = "0.0 0.0"; }
						{ auto& p = P(); p.name = "uv_scale_b";    p.kind = ValueKind::DoubleVec3;p.description = "(U, V) scale applied to (u,v) before sampling B"; p.defaultValueHint = "1.0 1.0"; }
						{ auto& p = P(); p.name = "uv_offset_b";   p.kind = ValueKind::DoubleVec3;p.description = "(U, V) offset applied to (u,v) before sampling B"; p.defaultValueHint = "0.0 0.0"; }
						{ auto& p = P(); p.name = "lerp_t";        p.kind = ValueKind::Double;    p.description = "Lerp parameter (clamped to [0,1]); only used when op = lerp"; p.defaultValueHint = "0.5"; }
						{ auto& p = P(); p.name = "output_scale";  p.kind = ValueKind::Double;    p.description = "Final-stage scalar multiplier applied AFTER the operator"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "output_offset"; p.kind = ValueKind::Double;    p.description = "Final-stage scalar offset added AFTER output_scale"; p.defaultValueHint = "0.0"; }
						return cd;
					}();
					return d;
				}
			};

			struct Perlin3DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",        "noname" );
					std::string colora      = bag.GetString( "colora",      "none" );
					std::string colorb      = bag.GetString( "colorb",      "none" );
					double persistence      = bag.GetDouble( "persistence", 1.0 );
					unsigned int octaves    = bag.GetUInt(   "octaves",     4 );
					double scale[3] = {1.0,1.0,1.0};
					double shift[3] = {0,0,0};
					bag.GetVec3( "scale", scale );
					bag.GetVec3( "shift", shift );

					return pJob.AddPerlin3DPainter( name.c_str(), persistence, octaves, colora.c_str(), colorb.c_str(), scale, shift );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "perlin3d_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Perlin fBm noise sampled at the WORLD-SPACE intersection point (3D solid domain -- needs no UVs and shows no seams; the object slides through a fixed world field, so `shift` re-registers the pattern).  Smooth cloudy variation from colora (low) to colorb (high).  The workhorse under wood, marble and mottled stone -- give `scale` unequal components for grain.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddNoisePainterCommonParams( P, "1.0" );
						return cd;
					}();
					return d;
				}
			};

			struct Wavelet3DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",        "noname" );
					std::string colora      = bag.GetString( "colora",      "none" );
					std::string colorb      = bag.GetString( "colorb",      "none" );
					unsigned int tile_size  = bag.GetUInt(   "tile_size",   32 );
					double persistence      = bag.GetDouble( "persistence", 0.65 );
					unsigned int octaves    = bag.GetUInt(   "octaves",     4 );
					double scale[3] = {1.0,1.0,1.0};
					double shift[3] = {0,0,0};
					bag.GetVec3( "scale", scale );
					bag.GetVec3( "shift", shift );

					return pJob.AddWavelet3DPainter( name.c_str(), tile_size, persistence, octaves, colora.c_str(), colorb.c_str(), scale, shift );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "wavelet3d_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Band-limited wavelet noise at the world-space intersection point (3D solid domain).  Stays crisp under minification where plain Perlin aliases, so use it for fine detail seen at a distance.  colora (low) -> colorb (high).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddNoisePainterCommonParams( P, "0.65" );
						{ auto& p = P(); p.name = "tile_size"; p.kind = ValueKind::UInt; p.description = "Precomputed tile edge length"; p.defaultValueHint = "32"; }
						return cd;
					}();
					return d;
				}
			};

			struct ReactionDiffusion3DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",       "noname" );
					std::string colora      = bag.GetString( "colora",     "none" );
					std::string colorb      = bag.GetString( "colorb",     "none" );
					unsigned int grid_size  = bag.GetUInt(   "grid_size",  32 );
					double da               = bag.GetDouble( "da",         0.2 );
					double db               = bag.GetDouble( "db",         0.1 );
					double feed             = bag.GetDouble( "feed",       0.037 );
					double kill             = bag.GetDouble( "kill",       0.06 );
					unsigned int iterations = bag.GetUInt(   "iterations", 2000 );
					double scale[3] = {1.0,1.0,1.0};
					double shift[3] = {0,0,0};
					bag.GetVec3( "scale", scale );
					bag.GetVec3( "shift", shift );

					return pJob.AddReactionDiffusion3DPainter( name.c_str(), grid_size, da, db, feed, kill, iterations, colora.c_str(), colorb.c_str(), scale, shift );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "reactiondiffusion3d_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Gray-Scott reaction-diffusion, simulated ONCE onto a grid_size^3 volume at construction and then sampled at the world-space intersection point (3D solid domain).  ORGANIC spots / stripes / labyrinths -- `feed` + `kill` select which -- for animal hide, coral, lichen, oxidation blooms.  Setup cost grows as grid_size^3 * iterations, so keep both modest.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";       p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora";     p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "First colour"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "colorb";     p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "Second colour"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "grid_size";  p.kind = ValueKind::UInt;       p.description = "Simulation grid edge (cost is grid_size^3)"; p.defaultValueHint = "32"; }
						{ auto& p = P(); p.name = "da";         p.kind = ValueKind::Double;     p.description = "Diffusion rate of A";          p.defaultValueHint = "0.2"; }
						{ auto& p = P(); p.name = "db";         p.kind = ValueKind::Double;     p.description = "Diffusion rate of B";          p.defaultValueHint = "0.1"; }
						{ auto& p = P(); p.name = "feed";       p.kind = ValueKind::Double;     p.description = "Feed rate (with `kill`, selects spots vs stripes vs labyrinths)"; p.defaultValueHint = "0.037"; }
						{ auto& p = P(); p.name = "kill";       p.kind = ValueKind::Double;     p.description = "Kill rate (with `feed`, selects spots vs stripes vs labyrinths)"; p.defaultValueHint = "0.06"; }
						{ auto& p = P(); p.name = "iterations"; p.kind = ValueKind::UInt;       p.description = "Simulation iterations";        p.defaultValueHint = "2000"; }
						{ auto& p = P(); p.name = "scale";      p.kind = ValueKind::DoubleVec3; p.description = "Per-axis scale";               p.defaultValueHint = "1 1 1"; }
						{ auto& p = P(); p.name = "shift";      p.kind = ValueKind::DoubleVec3; p.description = "Per-axis shift";               p.defaultValueHint = "0 0 0"; }
						return cd;
					}();
					return d;
				}
			};

			struct Gabor3DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",            "noname" );
					std::string colora      = bag.GetString( "colora",          "none" );
					std::string colorb      = bag.GetString( "colorb",          "none" );
					double frequency        = bag.GetDouble( "frequency",       4.0 );
					double bandwidth        = bag.GetDouble( "bandwidth",       1.0 );
					double impulse_density  = bag.GetDouble( "impulse_density", 4.0 );
					double orientation[3] = {0,1,0};
					double scale[3] = {1.0,1.0,1.0};
					double shift[3] = {0,0,0};
					bag.GetVec3( "orientation", orientation );
					bag.GetVec3( "scale",       scale );
					bag.GetVec3( "shift",       shift );

					return pJob.AddGabor3DPainter( name.c_str(), frequency, bandwidth, orientation, impulse_density, colora.c_str(), colorb.c_str(), scale, shift );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "gabor3d_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Gabor noise (oriented band-limited wavelets) at the world-space intersection point (3D solid domain) -- DIRECTIONAL streaks along `orientation` at `frequency`, unlike the isotropic noises.  Brushed metal, wood fibre, fur flow, scratch fields.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";            p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora";          p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "First colour"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "colorb";          p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "Second colour"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "frequency";       p.kind = ValueKind::Double;     p.description = "Carrier frequency (streak pitch)"; p.defaultValueHint = "4.0"; }
						{ auto& p = P(); p.name = "bandwidth";       p.kind = ValueKind::Double;     p.description = "Gaussian bandwidth";  p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "orientation";     p.kind = ValueKind::DoubleVec3; p.description = "Orientation vector"; }
						{ auto& p = P(); p.name = "impulse_density"; p.kind = ValueKind::Double;     p.description = "Impulses per unit volume (higher = denser streaks, more cost)"; p.defaultValueHint = "4.0"; }
						{ auto& p = P(); p.name = "scale";           p.kind = ValueKind::DoubleVec3; p.description = "Per-axis scale";      p.defaultValueHint = "1 1 1"; }
						{ auto& p = P(); p.name = "shift";           p.kind = ValueKind::DoubleVec3; p.description = "Per-axis shift";      p.defaultValueHint = "0 0 0"; }
						return cd;
					}();
					return d;
				}
			};

			struct Simplex3DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",        "noname" );
					std::string colora      = bag.GetString( "colora",      "none" );
					std::string colorb      = bag.GetString( "colorb",      "none" );
					double persistence      = bag.GetDouble( "persistence", 0.65 );
					unsigned int octaves    = bag.GetUInt(   "octaves",     4 );
					double scale[3] = {1.0,1.0,1.0};
					double shift[3] = {0,0,0};
					bag.GetVec3( "scale", scale );
					bag.GetVec3( "shift", shift );

					return pJob.AddSimplex3DPainter( name.c_str(), persistence, octaves, colora.c_str(), colorb.c_str(), scale, shift );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "simplex3d_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Simplex noise (Perlin-like, cheaper, without Perlin's axis-aligned grid artifacts) at the world-space intersection point (3D solid domain).  Same colora-low / colorb-high ramp -- a drop-in when perlin3d shows grid alignment.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddNoisePainterCommonParams( P, "0.65" );
						return cd;
					}();
					return d;
				}
			};

			struct SDF3DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",            "noname" );
					std::string colora      = bag.GetString( "colora",          "none" );
					std::string colorb      = bag.GetString( "colorb",          "none" );
					double param1           = bag.GetDouble( "param1",          0.5 );
					double param2           = bag.GetDouble( "param2",          0.3 );
					double param3           = bag.GetDouble( "param3",          0.3 );
					double shell_thickness  = bag.GetDouble( "shell_thickness", 0.0 );
					double noise_amplitude  = bag.GetDouble( "noise_amplitude", 0.0 );
					double noise_frequency  = bag.GetDouble( "noise_frequency", 1.0 );
					double scale[3] = {1.0,1.0,1.0};
					double shift[3] = {0,0,0};
					bag.GetVec3( "scale", scale );
					bag.GetVec3( "shift", shift );

					unsigned int type = 0;		// 0=sphere, 1=box, 2=torus, 3=cylinder
					if( bag.Has( "type" ) ) {
						std::string t = bag.GetString( "type" );
						if(      t == "sphere" )   type = 0;
						else if( t == "box" )      type = 1;
						else if( t == "torus" )    type = 2;
						else if( t == "cylinder" ) type = 3;
						else                       type = String( t.c_str() ).toUInt();
					}

					return pJob.AddSDF3DPainter( name.c_str(), type, param1, param2, param3, shell_thickness, noise_amplitude, noise_frequency, colora.c_str(), colorb.c_str(), scale, shift );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "sdf3d_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Colours by the signed distance to an analytic primitive (`type`) at the world-space intersection point (3D solid domain): concentric shells / bands / an inlay footprint, optionally noise-perturbed.  This paints COLOUR only -- for actual SDF geometry use sdf_geometry.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";             p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora";           p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "Inside colour"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "colorb";           p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "Outside colour"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "type";             p.kind = ValueKind::Enum;       p.enumValues = {"sphere","box","torus","cylinder"}; p.description = "SDF primitive (SDFPrimitiveType has exactly these four; an unrecognised spelling falls through to sphere)"; }
						{ auto& p = P(); p.name = "param1";           p.kind = ValueKind::Double;     p.description = "Shape parameter 1 (sphere/cylinder radius, box half-extent x, torus major radius)"; p.defaultValueHint = "0.5"; }
						{ auto& p = P(); p.name = "param2";           p.kind = ValueKind::Double;     p.description = "Shape parameter 2 (box half-extent y, torus minor radius, cylinder half-height)"; p.defaultValueHint = "0.3"; }
						{ auto& p = P(); p.name = "param3";           p.kind = ValueKind::Double;     p.description = "Shape parameter 3 (box half-extent z; unused by the other primitives)"; p.defaultValueHint = "0.3"; }
						{ auto& p = P(); p.name = "shell_thickness";  p.kind = ValueKind::Double;     p.description = "Shell/band thickness"; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "noise_amplitude";  p.kind = ValueKind::Double;     p.description = "Noise displacement amplitude"; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "noise_frequency";  p.kind = ValueKind::Double;     p.description = "Noise displacement frequency"; p.defaultValueHint = "1"; }
						{ auto& p = P(); p.name = "scale";            p.kind = ValueKind::DoubleVec3; p.description = "Per-axis scale"; p.defaultValueHint = "1 1 1"; }
						{ auto& p = P(); p.name = "shift";            p.kind = ValueKind::DoubleVec3; p.description = "Per-axis shift"; p.defaultValueHint = "0 0 0"; }
						return cd;
					}();
					return d;
				}
			};

			struct CurlNoise3DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",        "noname" );
					std::string colora      = bag.GetString( "colora",      "none" );
					std::string colorb      = bag.GetString( "colorb",      "none" );
					double persistence      = bag.GetDouble( "persistence", 0.65 );
					unsigned int octaves    = bag.GetUInt(   "octaves",     4 );
					double epsilon          = bag.GetDouble( "epsilon",     0.01 );
					double scale[3] = {1.0,1.0,1.0};
					double shift[3] = {0,0,0};
					bag.GetVec3( "scale", scale );
					bag.GetVec3( "shift", shift );

					return pJob.AddCurlNoise3DPainter( name.c_str(), persistence, octaves, epsilon, colora.c_str(), colorb.c_str(), scale, shift );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "curlnoise3d_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Divergence-free curl noise at the world-space intersection point (3D solid domain) -- swirling flow-like filaments instead of blobs.  Smoke, marbled paper, eddies, whorls.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddNoisePainterCommonParams( P, "0.65" );
						{ auto& p = P(); p.name = "epsilon"; p.kind = ValueKind::Double; p.description = "Finite-difference step used for the curl"; p.defaultValueHint = "0.01"; }
						return cd;
					}();
					return d;
				}
			};

			struct DomainWarp3DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",           "noname" );
					std::string colora      = bag.GetString( "colora",         "none" );
					std::string colorb      = bag.GetString( "colorb",         "none" );
					double persistence      = bag.GetDouble( "persistence",    0.65 );
					unsigned int octaves    = bag.GetUInt(   "octaves",        4 );
					double warp_amplitude   = bag.GetDouble( "warp_amplitude", 4.0 );
					unsigned int warp_levels= bag.GetUInt(   "warp_levels",    2 );
					double scale[3] = {1.0,1.0,1.0};
					double shift[3] = {0,0,0};
					bag.GetVec3( "scale", scale );
					bag.GetVec3( "shift", shift );

					return pJob.AddDomainWarp3DPainter( name.c_str(), persistence, octaves, warp_amplitude, warp_levels, colora.c_str(), colorb.c_str(), scale, shift );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "domainwarp3d_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Noise whose INPUT coordinates are themselves noise-displaced (`warp_amplitude`, `warp_levels`), at the world-space intersection point (3D solid domain).  The cheapest way to stop regular fBm reading as `noise`: flowing, organic, marbled distortion.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddNoisePainterCommonParams( P, "0.65" );
						{ auto& p = P(); p.name = "warp_amplitude"; p.kind = ValueKind::Double; p.description = "Warp displacement amplitude (how far the coordinates are dragged)"; p.defaultValueHint = "4.0"; }
						{ auto& p = P(); p.name = "warp_levels";    p.kind = ValueKind::UInt;   p.description = "Warp iteration levels"; p.defaultValueHint = "2"; }
						return cd;
					}();
					return d;
				}
			};

			struct PerlinWorley3DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",          "noname" );
					std::string colora      = bag.GetString( "colora",        "none" );
					std::string colorb      = bag.GetString( "colorb",        "none" );
					double persistence      = bag.GetDouble( "persistence",   0.65 );
					unsigned int octaves    = bag.GetUInt(   "octaves",       4 );
					double worley_jitter    = bag.GetDouble( "worley_jitter", 1.0 );
					double blend            = bag.GetDouble( "blend",         0.5 );
					double scale[3] = {1.0,1.0,1.0};
					double shift[3] = {0,0,0};
					bag.GetVec3( "scale", scale );
					bag.GetVec3( "shift", shift );

					return pJob.AddPerlinWorley3DPainter( name.c_str(), persistence, octaves, worley_jitter, blend, colora.c_str(), colorb.c_str(), scale, shift );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "perlinworley3d_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Perlin and Worley blended (`blend` weights them) at the world-space intersection point (3D solid domain) -- billowy but clumpy.  The standard cloud / foam / lichen / moss field.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddNoisePainterCommonParams( P, "0.65" );
						{ auto& p = P(); p.name = "worley_jitter"; p.kind = ValueKind::Double; p.description = "Worley feature jitter amount"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "blend";         p.kind = ValueKind::Double; p.description = "Perlin/Worley blend weight";    p.defaultValueHint = "0.5"; }
						return cd;
					}();
					return d;
				}
			};

			struct Worley3DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",   "noname" );
					std::string colora      = bag.GetString( "colora", "none" );
					std::string colorb      = bag.GetString( "colorb", "none" );
					double jitter           = bag.GetDouble( "jitter", 1.0 );
					double scale[3] = {1.0,1.0,1.0};
					double shift[3] = {0,0,0};
					bag.GetVec3( "scale", scale );
					bag.GetVec3( "shift", shift );

					unsigned int metric = 0;	// 0=Euclidean, 1=Manhattan, 2=Chebyshev
					if( bag.Has( "metric" ) ) {
						std::string m = bag.GetString( "metric" );
						if(      m == "euclidean" ) metric = 0;
						else if( m == "manhattan" ) metric = 1;
						else if( m == "chebyshev" ) metric = 2;
						else                        metric = String( m.c_str() ).toUInt();
					}
					unsigned int output = 0;	// 0=F1, 1=F2, 2=F2-F1
					if( bag.Has( "output" ) ) {
						std::string o = bag.GetString( "output" );
						if(      o == "f1" )    output = 0;
						else if( o == "f2" )    output = 1;
						else if( o == "f2-f1" ) output = 2;
						else                    output = String( o.c_str() ).toUInt();
					}

					return pJob.AddWorley3DPainter( name.c_str(), jitter, metric, output, colora.c_str(), colorb.c_str(), scale, shift );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "worley3d_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Worley / cellular noise (distance to scattered feature points) at the world-space intersection point (3D solid domain).  `output f1` gives blobby CELLS -- pebbles, hammered metal, leather, wear patches; `output f2-f1` gives the cell BOUNDARIES -- cracks, crazing, dried mud, veins.  colora (near a feature point) -> colorb (far).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";   p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora"; p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "First colour"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "colorb"; p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "Second colour"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "jitter"; p.kind = ValueKind::Double;     p.description = "Feature jitter";               p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "metric"; p.kind = ValueKind::Enum;       p.enumValues = {"euclidean","manhattan","chebyshev"}; p.description = "Distance metric"; p.defaultValueHint = "euclidean"; }
						{ auto& p = P(); p.name = "output"; p.kind = ValueKind::Enum;       p.enumValues = {"f1","f2","f2-f1"};            p.description = "Value function: f1 = distance to nearest feature (blobby cells), f2 = second-nearest, f2-f1 = cell boundaries (cracks / veins)"; p.defaultValueHint = "f1"; }
						{ auto& p = P(); p.name = "scale";  p.kind = ValueKind::DoubleVec3; p.description = "Per-axis scale";               p.defaultValueHint = "1 1 1"; }
						{ auto& p = P(); p.name = "shift";  p.kind = ValueKind::DoubleVec3; p.description = "Per-axis shift";               p.defaultValueHint = "0 0 0"; }
						return cd;
					}();
					return d;
				}
			};

			struct Turbulence3DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",        "noname" );
					std::string colora      = bag.GetString( "colora",      "none" );
					std::string colorb      = bag.GetString( "colorb",      "none" );
					double persistence      = bag.GetDouble( "persistence", 1.0 );
					unsigned int octaves    = bag.GetUInt(   "octaves",     4 );
					double scale[3] = {1.0,1.0,1.0};
					double shift[3] = {0,0,0};
					bag.GetVec3( "scale", scale );
					bag.GetVec3( "shift", shift );

					return pJob.AddTurbulence3DPainter( name.c_str(), persistence, octaves, colora.c_str(), colorb.c_str(), scale, shift );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "turbulence3d_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Turbulence -- a sum of |Perlin| octaves -- at the world-space intersection point (3D solid domain).  Ridged and veiny where perlin3d is a smooth swell, so it is the first pick for marble veins, wood grain, soot, rust mottle and weathering.  colora (low) -> colorb (high).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddNoisePainterCommonParams( P, "1.0" );
						return cd;
					}();
					return d;
				}
			};

			struct Voronoi2DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name   = bag.GetString( "name",       "noname" );
					std::string border = bag.GetString( "border",     "none" );
					double bordersize  = bag.GetDouble( "bordersize", 0.0 );

					std::vector<double> ptx;
					std::vector<double> pty;
					std::vector<std::string> painters;

					// Repeatable inline generators: "gen <x> <y> <painter>"
					const std::vector<std::string>& gens = bag.GetRepeatable( "gen" );
					for( size_t k = 0; k < gens.size(); ++k ) {
						double x = 0, y = 0;
						char painter[256] = {0};
						sscanf( gens[k].c_str(), "%lf %lf %255s", &x, &y, painter );
						ptx.push_back( x );
						pty.push_back( y );
						painters.push_back( painter );
					}

					// Optional file-loaded generators
					if( bag.Has( "file" ) ) {
						std::string fname = bag.GetString( "file" );
						std::vector<std::string> fileLines;
						if( ReadDataFileLines( String( fname.c_str() ), fileLines ) ) {
							for( const std::string& ln : fileLines ) {
								double x = 0.0, y = 0.0;
								char painter[256] = {0};
								if( sscanf( ln.c_str(), "%lf %lf %255s", &x, &y, painter ) == 3 ) {
									ptx.push_back( x );
									pty.push_back( y );
									painters.push_back( painter );
								}
							}
						}
					}

					const unsigned int num = static_cast<unsigned int>(painters.size());
					char* pntrmem = new char[num*256];
					memset( pntrmem, 0, num*256 );
					char** pntrs = new char*[num];

					for( unsigned int i=0; i<num; i++ ) {
						pntrs[i] = &pntrmem[i*256];
						strncpy( pntrs[i], painters[i].c_str(), 255 );
					}

					bool bRet = pJob.AddVoronoi2DPainter( name.c_str(), &ptx[0], &pty[0], (const char**)pntrs, num, border=="none"?0:border.c_str(), bordersize );

					delete [] pntrs;
					delete [] pntrmem;

					return bRet;
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "voronoi2d_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Explicit Voronoi cells in the surface UV (2D domain): each repeatable `gen <u> <v> <painter>` seeds ONE cell with its OWN painter, plus an optional `border` painter.  Art-directable where worley3d_painter is random -- mosaic, tile, stained glass, hide patches.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";       p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "gen";        p.kind = ValueKind::String;    p.repeatable = true; p.tupleKinds = {ValueKind::Double, ValueKind::Double, ValueKind::Reference}; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Voronoi generator: x y paintername (repeatable)"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "file";       p.kind = ValueKind::Filename;  p.description = "Generator list file (each line: x y paintername)"; }
						{ auto& p = P(); p.name = "border";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Border colour (painter)"; p.defaultValueHint = "none"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "bordersize"; p.kind = ValueKind::Double;    p.description = "Border width"; p.defaultValueHint = "0"; }
						return cd;
					}();
					return d;
				}
			};

			struct Voronoi3DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name   = bag.GetString( "name",       "noname" );
					std::string border = bag.GetString( "border",     "none" );
					double bordersize  = bag.GetDouble( "bordersize", 0.0 );

					// P2.5 (doc 88): historically this painter always sampled
					// OBJECT space regardless of the "world space" claim in
					// skills/agent/procedural-textures.md -- `space` makes that
					// explicit and lets a scene opt into `world` to match the
					// rest of the 3D painter family.  Default preserves the
					// historical behaviour byte-identically.
					std::string spaceStr = bag.GetString( "space", "object" );
					bool worldSpace = false;
					if( spaceStr == "object" )      worldSpace = false;
					else if( spaceStr == "world" )  worldSpace = true;
					else {
						GlobalLog()->PrintEx( eLog_Error,
							"voronoi3d_painter `%s`: unknown space `%s` (expected object or world)",
							name.c_str(), spaceStr.c_str() );
						return false;
					}

					std::vector<double> ptx;
					std::vector<double> pty;
					std::vector<double> ptz;
					std::vector<std::string> painters;

					// Repeatable inline generators: "gen <x> <y> <z> <painter>"
					const std::vector<std::string>& gens = bag.GetRepeatable( "gen" );
					for( size_t k = 0; k < gens.size(); ++k ) {
						double x = 0, y = 0, z = 0;
						char painter[256] = {0};
						sscanf( gens[k].c_str(), "%lf %lf %lf %255s", &x, &y, &z, painter );
						ptx.push_back( x );
						pty.push_back( y );
						ptz.push_back( z );
						painters.push_back( painter );
					}

					// Optional file-loaded generators (count-prefixed)
					if( bag.Has( "file" ) ) {
						std::string fname = bag.GetString( "file" );
						std::vector<std::string> fileLines;
						if( ReadDataFileLines( String( fname.c_str() ), fileLines ) && !fileLines.empty() ) {
							int num = 0;
							sscanf( fileLines[0].c_str(), "%d", &num );
							for( int i = 1; i <= num && i < (int)fileLines.size(); ++i ) {
								double x = 0.0, y = 0.0, z = 0.0;
								char painter[256] = {0};
								if( sscanf( fileLines[i].c_str(), "%lf %lf %lf %255s", &x, &y, &z, painter ) == 4 ) {
									ptx.push_back( x );
									pty.push_back( y );
									ptz.push_back( z );
									painters.push_back( painter );
								}
							}
						}
					}

					const unsigned int num = static_cast<unsigned int>(painters.size());
					char* pntrmem = new char[num*256];
					memset( pntrmem, 0, num*256 );
					char** pntrs = new char*[num];

					for( unsigned int i=0; i<num; i++ ) {
						pntrs[i] = &pntrmem[i*256];
						strncpy( pntrs[i], painters[i].c_str(), 255 );
					}

					// P1-A fix (S7 review round 1): AddVoronoi3DPainter's
					// original 8-arg signature must stay byte-identical
					// (see IJob.h) -- call the tail-appended `WithSpace`
					// virtual instead, which always carries `worldSpace`
					// (computed above, defaulting to the historical FALSE
					// when `space` is omitted).
					bool bRet = pJob.AddVoronoi3DPainterWithSpace( name.c_str(), &ptx[0], &pty[0], &ptz[0], (const char**)pntrs, num, border=="none"?0:border.c_str(), bordersize, worldSpace );

					delete [] pntrs;
					delete [] pntrmem;

					return bRet;
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "voronoi3d_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Explicit Voronoi cells (3D solid domain): each repeatable `gen <x> <y> <z> <painter>` seeds ONE cell with its OWN painter, plus an optional `border` painter.  Art-directable where worley3d_painter is random -- aggregate, terrazzo, crystal grains.  HISTORICAL INCONSISTENCY (P2.5, doc 88): unlike the rest of the 3D painter family (perlin3d, worley3d, ...), which always sample the WORLD-space intersection, this painter has always sampled OBJECT space (`ptObjIntersec`) -- `space` makes that explicit; the default `object` preserves every existing scene byte-identically, `world` opts into the same convention as the other 3D painters.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";       p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "gen";        p.kind = ValueKind::String;    p.repeatable = true; p.tupleKinds = {ValueKind::Double, ValueKind::Double, ValueKind::Double, ValueKind::Reference}; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Voronoi generator: x y z paintername (repeatable).  These x/y/z coordinates are interpreted in whichever domain `space` selects (P2.5, doc 88): `object` (default) reads them as OBJECT-space coordinates (ptObjIntersec), `world` reads them as WORLD-space coordinates (ptIntersection) -- flipping `space` therefore RE-INTERPRETS every authored generator (and the border) position against a different point domain, it does not just change which point the distance test samples."; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "file";       p.kind = ValueKind::Filename;  p.description = "Generator list file (count-prefixed: N then N lines of x y z paintername)"; }
						{ auto& p = P(); p.name = "border";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Border colour (painter)"; p.defaultValueHint = "none"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "bordersize"; p.kind = ValueKind::Double;    p.description = "Border width"; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "space";      p.kind = ValueKind::Enum;      p.enumValues = {"object","world"}; p.description = "Domain to sample generator distances in (P2.5, doc 88).  `object` (default) is the HISTORICAL behaviour -- ptObjIntersec, so an instanced/transformed object keeps the same cell pattern.  `world` samples ptIntersection, matching perlin3d/worley3d/etc.  WARNING: this is not just a sampling-side switch -- every `gen` (and border) position was authored assuming ONE of these domains, so flipping `space` re-interprets every authored cell position against the OTHER domain and generally requires re-authoring the generator coordinates to match."; p.defaultValueHint = "object"; }
						return cd;
					}();
					return d;
				}
			};

			struct IridescentPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name   = bag.GetString( "name",   "noname" );
					std::string colora = bag.GetString( "colora", "none" );
					std::string colorb = bag.GetString( "colorb", "none" );
					double bias        = bag.GetDouble( "bias",   0.0 );

					return pJob.AddIridescentPainter( name.c_str(), colora.c_str(), colorb.c_str(), bias );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "iridescent_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "View-angle-dependent colour: interpolates from colora at GRAZING incidence to colorb at NORMAL incidence, on |dot(view, normal)| + `bias`.  Soap film, beetle shell, oil slick, pearlescent paint.  A cheap look-alike only -- the physical thin-film model is ggx_material with fresnel_mode thinfilm.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";   p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Grazing-angle colour (selected as |dot(view, normal)| approaches 0)"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "colorb"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Normal-incidence colour (selected as |dot(view, normal)| approaches 1)"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "bias";   p.kind = ValueKind::Double;    p.description = "Added to |dot(view, normal)| before the interpolation is clamped to [0,1]: positive pushes the whole surface toward colorb"; p.defaultValueHint = "0.0"; }
						return cd;
					}();
					return d;
				}
			};

			struct BlackBodyPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name      = bag.GetString( "name",        "noname" );
					double temperature    = bag.GetDouble( "temperature", 5600.0 );
					double lambda_begin   = bag.GetDouble( "nmbegin",     400.0 );
					double lambda_end     = bag.GetDouble( "nmend",       700.0 );
					unsigned int num_freq = bag.GetUInt(   "numfreq",     30 );
					double scale          = bag.GetDouble( "scale",       1.0 );
					bool normalize        = bag.GetBool(   "normalize",   true );

					return pJob.AddBlackBodyPainter( name.c_str(), temperature, lambda_begin, lambda_end, num_freq, normalize, scale );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "blackbody_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Planckian blackbody spectrum at `temperature` Kelvin -- a physically-derived incandescent / flame / hot-metal colour instead of a hand-guessed RGB triple.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";         p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "temperature";  p.kind = ValueKind::Double; p.description = "Temperature in Kelvin"; p.defaultValueHint = "5600"; }
						{ auto& p = P(); p.name = "nmbegin";      p.kind = ValueKind::Double; p.description = "Start wavelength (nm)"; p.defaultValueHint = "400"; }
						{ auto& p = P(); p.name = "nmend";        p.kind = ValueKind::Double; p.description = "End wavelength (nm)";   p.defaultValueHint = "700"; }
						{ auto& p = P(); p.name = "numfreq";      p.kind = ValueKind::UInt;   p.description = "Sample count";          p.defaultValueHint = "30"; }
						{ auto& p = P(); p.name = "normalize";    p.kind = ValueKind::Bool;   p.description = "Normalize peak to 1";   p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "scale";        p.kind = ValueKind::Double; p.description = "Overall amplitude";     p.defaultValueHint = "1.0"; }
						return cd;
					}();
					return d;
				}
			};

			struct BlendPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name   = bag.GetString( "name",   "noname" );
					std::string colora = bag.GetString( "colora", "none" );
					std::string colorb = bag.GetString( "colorb", "none" );
					std::string mask   = bag.GetString( "mask",   "none" );

					// P2.4 (doc 88): blend mode -- HOW colora/colorb combine
					// before mask interpolates toward colorb.  Absent = mix,
					// byte-identical to the pre-P2.4 formula.
					unsigned int mode = 0;
					if( bag.Has( "mode" ) ) {
						const std::string modeStr = bag.GetString( "mode" );
						if(      modeStr == "mix" )      mode = 0;
						else if( modeStr == "multiply" ) mode = 1;
						else if( modeStr == "screen" )   mode = 2;
						else if( modeStr == "overlay" )  mode = 3;
						else if( modeStr == "add" )      mode = 4;
						else {
							GlobalLog()->PrintEx( eLog_Error,
								"blend_painter `%s`: unknown mode `%s` (expected mix, multiply, screen, overlay, or add)",
								name.c_str(), modeStr.c_str() );
							return false;
						}
					}

					// P1-A fix (S7 review round 1): AddBlendPainter's
					// original 4-arg signature must stay byte-identical
					// (see IJob.h) -- call the tail-appended `WithMode`
					// virtual instead, which always carries `mode`
					// (defaulting to 0=mix, byte-identical, when `mode`
					// is omitted above).
					return pJob.AddBlendPainterWithMode( name.c_str(), colora.c_str(), colorb.c_str(), mask.c_str(), mode );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "blend_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Blends colora and colorb per a THIRD painter used as the `mask`: out = blend(colora,colorb) * mask + colorb * (1 - mask), so mask 1 selects the blended combination and mask 0 -> colorb (NOT the other way round).  `mode` (P2.4, doc 88) picks the blend(a,b) formula: `mix` (default) is blend(a,b)=a, which reduces the whole expression to the original `colora * mask + colorb * (1 - mask)` -- byte-identical when `mode` is omitted.  `multiply`/`screen`/`overlay`/`add` combine a and b per channel (RGB) or per spectral sample (GetColorNM) with the SAME formula shape before the mask still interpolates toward colorb.  `screen` and `overlay` are DISPLAY-COMPOSITING curves defined only on [0,1] -- colora/colorb are CLAMPED to [0,1] before those two formulas (an out-of-range input would otherwise sign-flip the result); `mix`/`multiply`/`add` stay unbounded, matching the rest of RISE's HDR colour math.  The mask is applied PER CHANNEL, so a coloured mask tints as well as blends.  THE composition verb for procedural materials: put a noise painter in `mask` and two colours in colora/colorb, and the surface gets spatially-varying reflectance.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";   p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "First colour"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "colorb"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Second colour"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "mask";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Blend-weight painter, applied per channel: weight 1 selects colora, weight 0 selects colorb"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "mode";   p.kind = ValueKind::Enum;      p.enumValues = {"mix","multiply","screen","overlay","add"}; p.description = "P2.4 (doc 88): how colora and colorb combine before `mask` interpolates.  `mix` (default) reduces to the original colora*mask+colorb*(1-mask) formula, byte-identical when omitted."; p.defaultValueHint = "mix"; }
						return cd;
					}();
					return d;
				}
			};

			struct ChannelPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name    = bag.GetString( "name",    "noname" );
					std::string source  = bag.GetString( "source",  "none" );
					std::string chanStr = bag.GetString( "channel", "R" );
					double scale        = bag.GetDouble( "scale",   1.0 );
					double bias         = bag.GetDouble( "bias",    0.0 );
					char chan = 0;
					if(      chanStr == "R" || chanStr == "r" ) chan = 0;
					else if( chanStr == "G" || chanStr == "g" ) chan = 1;
					else if( chanStr == "B" || chanStr == "b" ) chan = 2;
					else if( chanStr == "A" || chanStr == "a" ) chan = 3;
					else {
						GlobalLog()->PrintEx( eLog_Error, "channel_painter:: unknown channel `%s` (expected R / G / B / A)", chanStr.c_str() );
						return false;
					}
					return pJob.AddChannelPainter( name.c_str(), source.c_str(), chan, scale, bias );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "channel_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Extracts a single R / G / B / A channel from another painter, "
							"applies an affine `out = scale * channel + bias`, and broadcasts the "
							"result as (out, out, out).  Designed for glTF metallic-roughness "
							"texture decomposition: glTF stores metallic in the .B channel and "
							"roughness in .G of the metallicRoughnessTexture, and "
							"`pbr_metallic_roughness_material` routes the texture through two "
							"channel_painter chunks to feed `ggx_material`'s scalar `rs` and "
							"`alphax`/`alphay` inputs.  Channel `A` reads un-premultiplied alpha "
							"(via `IPainter::GetAlpha`) and is used by the alphaMode = MASK and "
							"BLEND wiring in the glTF importer.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";    p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "source";  p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Source painter (e.g. a png_painter for an MR texture)"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "channel"; p.kind = ValueKind::Enum;      p.enumValues = {"R","G","B","A"}; p.description = "Which channel to extract (R, G, B, or A); A reads un-premultiplied alpha"; p.defaultValueHint = "R"; }
						{ auto& p = P(); p.name = "scale";   p.kind = ValueKind::Double;    p.description = "Scale multiplier"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "bias";    p.kind = ValueKind::Double;    p.description = "Additive offset (post-scale)"; p.defaultValueHint = "0.0"; }
						return cd;
					}();
					return d;
				}
			};

			//////////////////////////////////////////
			// Functions
			//////////////////////////////////////////

			struct PiecewiseLinearFunctionChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",   "noname" );
					bool         bUseLUTs   = bag.GetBool(   "use_lut", false );
					unsigned int lutsize    = bag.GetUInt(   "lutsize", 1024 );

					std::vector<double> cp_x;
					std::vector<double> cp_y;

					// Repeatable per-sample control points: "cp <x> <y>"
					const std::vector<std::string>& cps = bag.GetRepeatable( "cp" );
					for( size_t k = 0; k < cps.size(); ++k ) {
						double x = 0.0, y = 0.0;
						sscanf( cps[k].c_str(), "%lf %lf", &x, &y );
						cp_x.push_back( x );
						cp_y.push_back( y );
					}

					// Optional file-loaded function (pairs)
					if( bag.Has( "file" ) ) {
						std::string fname = bag.GetString( "file" );
						std::vector<std::string> fileLines;
						if( !ReadDataFileLines( String( fname.c_str() ), fileLines ) ) {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to open file `%s`", fname.c_str() );
							return false;
						}
						for( const std::string& ln : fileLines ) {
							double x = 0.0, y = 0.0;
							if( sscanf( ln.c_str(), "%lf %lf", &x, &y ) == 2 ) {
								cp_x.push_back( x );
								cp_y.push_back( y );
							} else {
								GlobalLog()->PrintEx( eLog_Warning, "piecewise_linear_function:: skipping malformed line in `%s`", fname.c_str() );
							}
						}
					}

					if( cp_x.empty() ) {
						GlobalLog()->PrintEx( eLog_Error, "piecewise_linear_function `%s`: no control points (empty / all-comment file, no inline cp)", name.c_str() );
						return false;
					}
					return pJob.AddPiecewiseLinearFunction( name.c_str(), &cp_x[0], &cp_y[0], static_cast<unsigned int>(cp_x.size()), bUseLUTs, lutsize );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "piecewise_linear_function"; cd.category = ChunkCategory::Function;
						cd.description = "1D piecewise-linear scalar function.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";    p.kind = ValueKind::String;   p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "cp";      p.kind = ValueKind::String;   p.repeatable = true; p.description = "Control point: x y (repeatable)"; }
						{ auto& p = P(); p.name = "use_lut"; p.kind = ValueKind::Bool;     p.description = "Use lookup table for fast evaluation"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "lutsize"; p.kind = ValueKind::UInt;     p.description = "LUT size";                              p.defaultValueHint = "1024"; }
						{ auto& p = P(); p.name = "file";    p.kind = ValueKind::Filename; p.description = "Function text file (x y pairs)"; }
						return cd;
					}();
					return d;
				}
			};

			struct PiecewiseLinearFunction2DChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );

					std::vector<double> cp_x;
					std::vector<String> cp_y;

					// Repeatable per-row entries: "cp <x> <y values...>"
					const std::vector<std::string>& cps = bag.GetRepeatable( "cp" );
					for( size_t k = 0; k < cps.size(); ++k ) {
						double x = 0.0;
						char y[1024] = {0};
						sscanf( cps[k].c_str(), "%lf %s", &x, y );
						cp_x.push_back( x );
						cp_y.push_back( String(y) );
					}

					if( cp_x.empty() ) {
						// A row-less 2D function is well-defined at runtime:
						// PiecewiseLinearFunction2D::getYValueFromFunction falls
						// back to the constant 1.0 whenever it holds fewer than
						// two rows.  Scenes use the empty form as a placeholder
						// (e.g. a displacement slot bound but not yet authored),
						// so warn loudly rather than refuse.
						GlobalLog()->PrintEx( eLog_Warning, "piecewise_linear_function2d `%s`: no control points -- evaluates as the constant 1.0", name.c_str() );
					}

					// Setup the array of strings
					char** func = new char*[cp_x.size()];
					for( unsigned int i=0; i<cp_x.size(); i++ ) {
						func[i] = (char*)(&(*(cp_y[i].begin())));
					}

					bool bRet = pJob.AddPiecewiseLinearFunction2D( name.c_str(), cp_x.empty() ? 0 : &cp_x[0], func, static_cast<unsigned int>(cp_x.size()) );

					delete [] func;

					return bRet;
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "piecewise_linear_function2d"; cd.category = ChunkCategory::Function;
						cd.description = "2D piecewise-linear function (1D row at each x).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name"; p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "cp";   p.kind = ValueKind::String; p.repeatable = true; p.description = "Row: x then space-separated y values (repeatable)"; }
						return cd;
					}();
					return d;
				}
			};

			//////////////////////////////////////////
			// Materials
			//////////////////////////////////////////

			struct LambertianMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",        "noname" );
					std::string reflectance = bag.GetString( "reflectance", "none" );

					return pJob.AddLambertianMaterial( name.c_str(), reflectance.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "lambertian_material"; cd.category = ChunkCategory::Material;
						cd.description = "Pure Lambertian (diffuse) material.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "reflectance"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Albedo painter"; p.semantics.pipe = ParameterPipe::Color; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct PerfectReflectorMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",        "noname" );
					std::string reflectance = bag.GetString( "reflectance", "none" );

					return pJob.AddPerfectReflectorMaterial( name.c_str(), reflectance.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "perfectreflector_material"; cd.category = ChunkCategory::Material;
						cd.description = "Perfect mirror reflector.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "reflectance"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Reflectance painter"; p.semantics.pipe = ParameterPipe::Color; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct PerfectRefractorMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",        "noname" );
					std::string refractance = bag.GetString( "refractance", "none" );
					std::string ior         = bag.GetString( "ior",         "1.33" );

					return pJob.AddPerfectRefractorMaterial( name.c_str(), refractance.c_str(), ior.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "perfectrefractor_material"; cd.category = ChunkCategory::Material;
						cd.description = "Perfect refractor (glass).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "refractance"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Transmittance painter"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "ior";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Index of refraction (physical SCALAR: a scalar_painter name, or an inline `r g b` or single scalar -- a COLOUR painter does not bind here)"; p.semantics.pipe = ParameterPipe::Scalar; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct PolishedMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",        "noname" );
					std::string reflectance = bag.GetString( "reflectance", "none" );
					// tau is a physical SCALAR (IScalarPainter) -- "none" is the
					// COLOUR manager's default painter name and does not resolve
					// there (ISCALARPAINTER_REFACTOR wrong-pipe-default class; see
					// CLAUDE.md).  "0.0" reproduces the exact pre-refactor
					// behaviour: the historical default WAS the "none" IPainter,
					// which is RISE_API_CreateUniformColorPainter(RISEPel(0,0,0))
					// -- i.e. literal 0 on every channel -- so the numeric literal
					// default below is bit-identical to what a bare
					// `polished_material` (no `tau` line) rendered before the
					// refactor: kray = tau*Rs = 0, so the dielectric coat
					// contributes no specular lobe and the material reads as
					// plain Lambertian (`reflectance`) until `tau` is set.
					std::string tau         = bag.GetString( "tau",         "0.0" );
					std::string ior         = bag.GetString( "ior",         "1.0" );
					std::string scat        = bag.GetString( "scattering",  "64" );
					bool hg                 = bag.GetBool(   "henyey-greenstein", false );

					return pJob.AddPolishedMaterial( name.c_str(), reflectance.c_str(), tau.c_str(), ior.c_str(), scat.c_str(), hg );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "polished_material"; cd.category = ChunkCategory::Material;
						cd.description = "Polished surface (Fresnel dielectric over Lambertian substrate).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";              p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "reflectance";       p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Diffuse substrate"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "tau";               p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Transmittance (physical SCALAR: a scalar_painter name, or an inline `r g b` or single scalar -- a COLOUR painter does not bind here).  0.0 (default) reproduces the pre-refactor \"none\" IPainter default (black) -- the dielectric coat contributes no specular lobe until this is set."; p.defaultValueHint = "0.0"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "ior";               p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Index of refraction (physical SCALAR: a scalar_painter name, or an inline `r g b` or single scalar -- a COLOUR painter does not bind here)"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "scattering";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Scattering coefficient (physical SCALAR: a scalar_painter name, or an inline `r g b` or single scalar -- a COLOUR painter does not bind here)"; p.defaultValueHint = "64"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "henyey-greenstein"; p.kind = ValueKind::Bool;      p.description = "Use Henyey-Greenstein phase"; p.defaultValueHint = "FALSE"; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			//! `coated_material` -- docs/WETNESS_COAT_DESIGN.md Phase 2.
			//!
			//! OpenPBR-shaped coat names on purpose (7.2): OpenPBR is the
			//! industry's converging interchange model, and GUI_ROADMAP.md
			//! already records RISE's posture of adopting it as the
			//! conceptual model.  Matching its coat parameter names costs
			//! nothing and makes an eventual MaterialX/OpenPBR import a
			//! rename rather than a redesign.
			//!
			//! There is deliberately NO `substrate_wet_exponent` (Phase 2
			//! item 4).  The wet darkening is a property of the layered
			//! TRANSPORT here -- the per-wavelength recycling
			//! 1/(1 - r_i R(lambda)) -- so an additional R^k would
			//! double-count the same physics.  Phase 1's `pow(base, k)`
			//! recipe was an RGB fit standing in for exactly this term.
			struct CoatedMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name       = bag.GetString( "name", "noname" );
					std::string base       = bag.GetString( "base", "none" );
					// Every coat parameter below the `base` is a physical
					// SCALAR and rides IScalarPainter -- coverage, IOR,
					// roughness, world length, 1/length.  "none" is the
					// COLOUR manager's default painter name and does not
					// resolve in the scalar manager (the wrong-pipe-default
					// class in CLAUDE.md / ISCALARPAINTER_REFACTOR.md), so
					// each default is a numeric literal.
					//
					// Defaults describe a WATER film at full coverage, which
					// is the phenomenon this material was built for: n = 1.33
					// and alpha = 0.02 are 7.2's own water values, and
					// thickness/absorption at 0 make Beer-Lambert a no-op so
					// a bare chunk is a clean, energy-conserving wet coat.
					std::string weight     = bag.GetString( "coat_weight",     "1.0" );
					std::string ior        = bag.GetString( "coat_ior",        "1.33" );
					std::string roughness  = bag.GetString( "coat_roughness",  "0.02" );
					std::string thickness  = bag.GetString( "coat_thickness",  "0.0" );
					std::string absorption = bag.GetString( "coat_absorption", "0.0" );
					// coat_tint IS genuinely a colour (a tinted lacquer), so
					// it rides the IPainter colourspace + JH-uplift pipe.
					// The default is the "none" sentinel, which
					// Job::AddCoatedMaterial reads as UNTINTED and turns
					// into white -- NOT as the "none" manager entry, which
					// is black and would make an untinted coat opaque.  A
					// genuinely black coat is authored by binding an
					// explicit black painter by name.
					std::string tint       = bag.GetString( "coat_tint",       "none" );

					return pJob.AddCoatedMaterial(
						name.c_str(), base.c_str(), weight.c_str(), ior.c_str(),
						roughness.c_str(), thickness.c_str(), absorption.c_str(),
						tint.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "coated_material"; cd.category = ChunkCategory::Material;
						cd.description = "Transparent dielectric film over a restricted substrate, with the film's coverage as a spatially varying slot.  Unlike composite_material and polished_material, its BSDF is the COMBINED layer response, so NEE and BDPT/VCM connections evaluate the coated surface rather than the bare substrate.  This is the wetness / clearcoat / varnish / oil-film material.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";            p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "base";            p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Material}; p.required = true; p.semantics.pipe = ParameterPipe::Material;
						  p.description = "Substrate material.  RESTRICTED, not any material: accepted are `lambertian_material`, `orennayar_material`, `ggx_material` and `pbr_metallic_roughness_material` (which resolves to a ggx_material at scene-build time), and the substrate must not emit.  Anything else is refused at parse time.  The layered model needs the substrate's directional albedo to run its interreflection series, which a luminaire, a BSSRDF or a volumetric random walk cannot supply.  `required`: unlike `composite_material`'s `top`/`bottom` or the luminaire materials' `material` slot, the built-in `none` null material is not a fallback here -- it fails the substrate allowlist just like any other unsupported material, so there is no valid default and this slot must always be authored."; }
						{ auto& p = P(); p.name = "coat_weight";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "1.0";
						  p.description = "Coat COVERAGE fraction in [0,1] (physical SCALAR: a scalar_painter name or a single inline scalar -- a COLOUR painter does not bind here, and neither does a PER-CHANNEL scalar painter: this slot is read as one value, so an `r g b` triple would silently drop g and b).  Clamped to [0,1].  This is the spatially varying slot: paint it to make a surface wet in the joints and dry on the crowns.  Semantically a sub-pixel AREA fraction, not a gloss knob -- at coverage c the response is the statistical mixture c*(coated) + (1-c)*(bare), and the bare branch reaches the substrate through AIR with no Fresnel transmission and no interreflection.  Both branches conserve energy, so partial coverage does not darken an otherwise-white surface."; }
						{ auto& p = P(); p.name = "coat_ior";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "1.33";
						  p.description = "Coat index of refraction (physical SCALAR: a scalar_painter name or a single inline scalar -- a COLOUR painter does not bind here, and neither does a PER-CHANNEL scalar painter).  1.33 water, 1.5 varnish/lacquer, 1.4-1.6 oil.  CLAMPED to [1, 3]: values below the surrounding medium's IOR are raised to it (see the next sentence), and 3 is the top of the tabulated range (no dielectric coat anyone authors exceeds it).  Drives BOTH the coat's Fresnel lobe AND the internal reflectance that recycles light back into the substrate -- which is where wet darkening and the wet chroma boost actually come from.  Must exceed the surrounding medium's IOR; a lower value is clamped to it (the layer model assumes the coat is the denser medium)."; }
						{ auto& p = P(); p.name = "coat_roughness";  p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "0.02";
						  p.description = "GGX alpha of the coat lobe (physical SCALAR: a scalar_painter name or a single inline scalar -- a COLOUR painter does not bind here, and neither does a PER-CHANNEL scalar painter).  0.01-0.05 for water, 0.03-0.1 for a clearcoat.  CLAMPED to [1e-3, 1] -- 1 is fully rough.  Floored at 1e-3: the coat lobe is never a delta distribution, deliberately, so that direct lighting and bidirectional connections can see it."; }
						{ auto& p = P(); p.name = "coat_thickness";  p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "0.0";
						  p.description = "Coat thickness in WORLD length units (physical SCALAR: a scalar_painter name or a single inline scalar -- a COLOUR painter does not bind here, and neither does a PER-CHANNEL scalar painter).  Multiplies `coat_absorption` into a Beer-Lambert optical depth for one normal-incidence traversal; oblique paths lengthen correctly.  0 (default) makes absorption a no-op.  It does NOT scale `coat_tint`, which is defined per traversal."; }
						{ auto& p = P(); p.name = "coat_absorption"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "0.0";
						  p.description = "Coat absorption coefficient in 1/length (physical SCALAR: a scalar_painter name or a single inline scalar -- a COLOUR painter does not bind here, and neither does a PER-CHANNEL scalar painter).  Deliberately on the SCALAR pipe, not the colour pipe: a large coefficient routed through a colour painter would be silently clamped by the Jakob-Hanika spectral uplift.  Use `coat_tint` for a coloured coat.  0 (default) is a perfectly clear film."; }
						{ auto& p = P(); p.name = "coat_tint";       p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Color; p.defaultValueHint = "none";
						  p.description = "Colour transmitted by ONE normal-incidence traversal of the coat (COLOUR painter -- this is the one coat slot that is genuinely a colour, e.g. a tinted lacquer).  Applied on the way in and again on the way out, raised to the obliquity factor so grazing paths tint more.  Independent of `coat_thickness` by construction.  `none` (the default) means UNTINTED, i.e. white -- it is NOT read as the built-in black `none` painter, which would make a clear coat opaque.  Bind an explicit black painter by name if a fully absorbing coat is what you want."; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			//! `fabric_material` -- docs/CLOTH_FABRIC_DESIGN.md Phase 1
			//! (9.2 - 9.6).
			//!
			//! MULTI-SLOT PRESET SEEDING, WHICH IS NEW IN RISE.  RISE
			//! already has per-parameter named quick-picks
			//! (`ParameterDescriptor::presets`, e.g. `scene_unit`), but
			//! those are an editor affordance on ONE scalar.  Nothing
			//! anywhere seeded SEVERAL slots from one name, and no
			//! material carried a preset of any kind.  `fabric <name>`
			//! is that mechanism: it supplies the default for every slot
			//! the author did not write, and ANY EXPLICITLY WRITTEN SLOT
			//! WINS.
			//!
			//! `bag.GetString(name, default)` alone CANNOT express that
			//! rule -- it cannot distinguish "the author omitted this"
			//! from "the author wrote the default value" -- so the
			//! seeding below goes through `ParseStateBag::Has()`.
			//!
			//! A PRESET CANNOT CONFIGURE THE SUBSTRATE, and pretending
			//! otherwise is the trap.  This chunk holds a REFERENCE to
			//! an already-constructed base material; `Finalize` can
			//! neither retype it nor re-parameterise it, and it does not
			//! even know its runtime class (it sees a NAME, a string).
			//! So `fabric satin` over a Lambertian base yields chalk
			//! with a faint sheen -- the tight anisotropic silk
			//! highlight lives in a substrate this chunk cannot reach.
			//! The WARN-level mismatch diagnostic therefore lives in
			//! `Job::AddFabricMaterial`, where the material manager has
			//! resolved the name to a pointer; the preset name is
			//! forwarded there for exactly that purpose (and so the
			//! preset's sheen COLOUR, an RGB triple rather than a
			//! painter name, can be honoured by a layer able to
			//! synthesise an owned painter).
			//!
			//! THERE IS NO `weave` ENUM IN PHASE 1, and its absence is
			//! deliberate rather than an oversight: with anisotropy
			//! delegated to the substrate (9.5), a weave enum has
			//! nothing left to select -- the RATE of anisotropy is the
			//! substrate's alphax/alphay and the PATTERN is whatever the
			//! author's `weave_rotation` field says.  The name is
			//! RESERVED, unused, for Phase 2's structured two-yarn-family
			//! model, which will have real per-binding behaviour.
			struct FabricMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				//! Format a preset scalar as an inline literal the
				//! scalar-painter resolver accepts.  %.17g round-trips a
				//! double exactly, so the seeded value is bit-identical
				//! to the table's.
				static std::string PresetScalarText( const Scalar v )
				{
					char buf[64];
					std::snprintf( buf, sizeof(buf), "%.17g", (double)v );
					return std::string( buf );
				}

				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name   = bag.GetString( "name",   "noname" );
					std::string fabric = bag.GetString( "fabric", "custom" );
					std::string base   = bag.GetString( "base",   "none" );

					const RISE::Implementation::FabricPreset& P =
						RISE::Implementation::LookupFabricPreset( fabric.c_str() );

					// `sheen_color` is genuinely a COLOUR (the dye), so it
					// rides the IPainter colourspace + JH-uplift pipe.  The
					// default is the "none" sentinel, which
					// Job::AddFabricMaterial reads as "use the preset's
					// colour, else WHITE" -- NOT as the "none" manager
					// entry, which is black and would switch the sheen lobe
					// off entirely.  A genuinely black (disabled) sheen is
					// authored by binding an explicit black painter by name.
					std::string color = bag.GetString( "sheen_color", "none" );

					// PRESET SEEDING, the `Has()` rule.  An author who
					// writes `sheen_roughness` wins over the preset; an
					// author who omits it gets the preset's calibrated
					// value, not a fixed global default.
					const std::string presetRough = PresetScalarText( P.sheenRoughness );
					std::string rough = bag.Has( "sheen_roughness" )
						? bag.GetString( "sheen_roughness", presetRough )
						: presetRough;

					// No preset sets a weave angle: the ANGLE is a spatial
					// field the author paints (9.5), and a preset that
					// baked in one constant rotation would be authoring
					// geometry it cannot see.  0 = unrotated, which makes
					// the substrate's frame byte-identical to the
					// un-wrapped material's.
					std::string rot = bag.GetString( "weave_rotation", "0.0" );

					return pJob.AddFabricMaterial(
						name.c_str(), fabric.c_str(), base.c_str(),
						color.c_str(), rough.c_str(), rot.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "fabric_material"; cd.category = ChunkCategory::Material;
						cd.description = "Cloth: an ENERGY-COMPENSATED Charlie sheen lobe over a restricted substrate, with the weave direction delivered as a rotation of the frame the SUBSTRATE is evaluated in.  Unlike stacking `sheen_material` under `composite_material`, its BSDF is the COMBINED response -- so NEE and BDPT/VCM connections evaluate the fabric rather than the bare substrate -- and it SUBTRACTS the sheen's energy from the base via a baked directional-albedo table, so a white fabric no longer returns more light than it receives at grazing.  Pick a `fabric` preset and bind a `base`; everything else has a calibrated default.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";   p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "fabric"; p.kind = ValueKind::Enum;
						  p.enumValues = {"cotton","denim","silk","satin","velvet","wool","linen","custom"};
						  p.defaultValueHint = "custom";
						  p.description = "Fabric PRESET.  Supplies the default for every slot below that you did not write; any slot you DO write wins.  Seeds `sheen_roughness` (cotton 0.55, linen 0.65, denim 0.45, wool 0.75, silk 0.20, satin 0.12, velvet 0.08, custom 0.50) and, for `velvet` only, a dark `sheen_color`.  IT CANNOT CONFIGURE THE SUBSTRATE: this chunk holds a reference to a base material it can neither retype nor re-parameterise, so `silk`, `satin` and `denim` -- whose whole look is an ANISOTROPIC ggx_material base (silk alphax 0.30 / alphay 0.10, satin 0.34 / 0.06, denim 0.34 / 0.22, all with `fresnel_mode schlick_f0` and `rs` bound to a ~0.04 dielectric-F0 painter) -- read as chalk with a faint sheen over a Lambertian.  Binding one of those presets over a base of the wrong class logs a WARNING naming the class it wanted; the composition is still legal.  cotton/linen/wool want an `orennayar_material` (sigma ~0.4 / 0.5 / 0.6); velvet wants a DARK `lambertian_material` and no anisotropy at all -- it is a pile, not a weave."; }
						{ auto& p = P(); p.name = "base";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Material}; p.required = true; p.semantics.pipe = ParameterPipe::Material;
						  p.description = "Substrate material.  RESTRICTED, not any material: accepted are `lambertian_material`, `orennayar_material`, `ggx_material`, `pbr_metallic_roughness_material` (which resolves to a ggx_material at scene-build time) and `weave_material`, and the substrate must not emit.  Anything else is refused at parse time.  The layered model needs the substrate's hemispherical albedo to subtract the sheen's energy honestly, which a luminaire, a BSSRDF or a volumetric random walk cannot supply.  THIS IS WHERE ANISOTROPY LIVES: the sheen lobe is strictly isotropic (Charlie's normaliser and its Lambda visibility are isotropic-only fits), so a directional weave is either a `weave_material` base (a real two-thread-family draft with a pattern scale -- what silk/satin/denim want) or a `ggx_material` base with alphax != alphay, steered per-point by `weave_rotation`.  A `weave_material` base under `transmission thin` also makes the whole stack SEE-THROUGH: the sheen layer forwards its substrate's transmission, attenuated, rather than blocking it."; }
						{ auto& p = P(); p.name = "sheen_color"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Color; p.defaultValueHint = "none";
						  p.description = "The dye / fuzz tint (COLOUR painter -- this is the one fabric slot that is genuinely a colour).  Its max channel is also the ENERGY SPLIT: the base is scaled by 1 - max3(sheen_color)*E(alpha, cos), so a white sheen suppresses the substrate most and a black one not at all.  `none` (the default) means the PRESET's colour where the preset sets one (velvet) and otherwise WHITE -- it is NOT read as the built-in black `none` painter, which would switch the lobe off.  Bind an explicit black painter by name if a disabled sheen is what you want."; }
						{ auto& p = P(); p.name = "sheen_roughness"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "per `fabric` (custom: 0.5)";
						  p.description = "Charlie alpha of the sheen lobe (physical SCALAR: a scalar_painter name or a single inline scalar -- a COLOUR painter does not bind here, and neither does a PER-CHANNEL scalar painter: this slot is read as one value, so an `r g b` triple would silently drop g and b).  Low = a tight grazing halo (velvet 0.08, satin 0.12), high = a broad soft sheen (wool 0.75).  CLAMPED to [0.04, 1] -- note the 0.04 floor, which is TIGHTER than `sheen_material`'s 1e-3: below ~0.035 the Charlie lobe's baked directional albedo exceeds 1 near grazing, and the base-energy subtraction would go negative.  Omit it to take the `fabric` preset's calibrated value."; }
						{ auto& p = P(); p.name = "weave_rotation"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "0.0";
						  p.description = "Weave angle in RADIANS (physical SCALAR: a scalar_painter name or a single inline scalar; per-channel painters are refused).  Rotates the tangent frame handed to the SUBSTRATE -- not the sheen lobe, which is isotropic and unaffected.  Paint it to make an anisotropic `ggx_material` base follow the yarn: a twill wale, a satin float direction, the grain change across a seam.  It ADDS to the substrate's own `tangent_rotation` (both are rotations about the same normal, weave first), so a ggx base keeps its finer per-material offset.  It also gives `ward_material` and `ashikminshirley_material` -- which have no rotation slot of their own -- one, because the rotation happens in the frame rather than in the lobe.  0 (the default) is a no-op: the substrate's frame is byte-identical to the un-wrapped material's.  There is deliberately NO `weave` enum in Phase 1; that name is reserved for the structured two-yarn model."; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			//! `weave_material` -- docs/CLOTH_FABRIC_DESIGN.md Phase 2
			//! (slice P2-A), the structured two-thread-family cloth BSDF.
			//!
			//! WHY THIS PARSER SEEDS NOTHING, unlike its Phase-1 sibling.
			//! `FabricMaterialAsciiChunkParser` above resolves the
			//! preset's `sheen_roughness` HERE (via `bag.Has()`) while
			//! leaving the preset's COLOUR to `Job::AddFabricMaterial`.
			//! That split is fine for two slots and is a liability for
			//! eighteen: it would mean two files that must agree, line by
			//! line, about which slots the preset seeds and what "the
			//! author omitted this" means.  So this parser forwards the
			//! AUTHORED string or an EMPTY one, and
			//! `Job::AddWeaveMaterial` does ALL the seeding in one place.
			//!
			//! `bag.GetString(name, "")` is therefore exactly right here
			//! and would be exactly wrong in the sibling: an empty string
			//! IS the wire protocol for "unset", and substituting a
			//! parser-side default would silently give every `fabric
			//! satin` the `plain` draft's numbers.
			//!
			//! THE TWO ENUMS ARE INDEPENDENT.  `weave` is the DRAFT
			//! (which family is on top per cell) and `fabric` is the
			//! PRESET (a named starting point that seeds every slot,
			//! `weave` included).  `weave twill_2_1` under `fabric
			//! custom` is a perfectly ordinary authoring; so is `fabric
			//! denim` with an explicit `weave satin_5` overriding denim's
			//! twill.
			struct WeaveMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name   = bag.GetString( "name",   "noname" );
					std::string weave  = bag.GetString( "weave",  "" );
					std::string fabric = bag.GetString( "fabric", "custom" );
					std::string transmission = bag.GetString( "transmission", "" );

					// P2-B: `sheer` is an accepted ALIAS for `gap` -- the
					// identical slot, under the more honest name once it can
					// actually transmit (docs/CLOTH_FABRIC_DESIGN.md 10).
					// `sheer` wins if both are authored; a scene that only
					// ever wrote `gap` (every P2-A scene) is unaffected.
					// R8 P3-3: authoring BOTH on the same chunk is legal
					// (silently preferring `sheer` is not a parse error) but
					// is very likely a mistake -- the two spellings binding
					// the SAME slot means one of them does nothing, and an
					// author who does not know that would reasonably expect
					// them to combine.  Diagnose it rather than staying
					// silent.
					if( bag.Has( "gap" ) && bag.Has( "sheer" ) ) {
						GlobalLog()->PrintEx( eLog_Warning,
							"weave_material `%s`: both `gap` and `sheer` are authored on the "
							"same chunk -- they are the SAME slot (`sheer` is an alias for `gap`), "
							"not two separate ones, so `sheer` (`%s`) wins and `gap` (`%s`) is "
							"ignored.  Author only one.",
							name.c_str(), bag.GetString( "sheer", "" ).c_str(), bag.GetString( "gap", "" ).c_str() );
					}
					std::string gap = bag.GetString( "sheer", "" );
					if( gap.empty() ) {
						gap = bag.GetString( "gap", "" );
					}

					return pJob.AddWeaveMaterial(
						name.c_str(), weave.c_str(), fabric.c_str(), transmission.c_str(),
						bag.GetString( "weave_scale",    "" ).c_str(),
						bag.GetString( "weave_rotation", "" ).c_str(),
						bag.GetString( "weft_skew",      "" ).c_str(),
						bag.GetString( "coverage",       "" ).c_str(),
						gap.c_str(),
						bag.GetString( "warp_color",     "" ).c_str(),
						bag.GetString( "weft_color",     "" ).c_str(),
						bag.GetString( "warp_ior",       "" ).c_str(),
						bag.GetString( "weft_ior",       "" ).c_str(),
						bag.GetString( "warp_width",     "" ).c_str(),
						bag.GetString( "weft_width",     "" ).c_str(),
						bag.GetString( "warp_azimuth",   "" ).c_str(),
						bag.GetString( "weft_azimuth",   "" ).c_str(),
						bag.GetString( "warp_kd",        "" ).c_str(),
						bag.GetString( "weft_kd",        "" ).c_str(),
						bag.GetString( "warp_tilt",      "" ).c_str(),
						bag.GetString( "weft_tilt",      "" ).c_str(),
						bag.GetString( "warp_transmit",  "" ).c_str(),
						bag.GetString( "weft_transmit",  "" ).c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "weave_material"; cd.category = ChunkCategory::Material;
						cd.description = "Cloth with STRUCTURE: two thread families (warp and weft), each with its own direction, its own dye and its own pair of fibre lobes, mixed by a weave-draft coverage field that says which yarn is on top at each point.  This is what makes satin's directional float sheen and denim's twill wale -- `fabric_material` cannot, because an isotropic sheen over one elliptical GGX lobe has no PATTERN SCALE (measured: 95-99 % of the substrate's anisotropy survives the sheen and it still reads as brushed metal).  Pick a `fabric` preset and a `weave` draft; everything else has a calibrated default.  It has NO sheen term of its own -- for the fuzz layer, wrap it in a `fabric_material` as the `base` (fuzz over weave is the physical stack).  Set `transmission thin` (or pick a `fabric` that defaults to it -- linen, silk, satin) for a backlit, see-through cloth: a delta lobe glows through the open weave and a Lambertian lobe glows through the yarn itself.  Default `transmission none` is reflection-only and bit-identical to the original Phase-2 slice.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name"; p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "fabric"; p.kind = ValueKind::Enum;
						  p.enumValues = {"denim","silk","satin","linen","custom"};
						  p.defaultValueHint = "custom";
						  p.description = "Fabric PRESET.  Supplies the default for EVERY slot below that you did not write -- including `weave` -- and any slot you DO write wins.  Unlike `fabric_material`'s preset, this one CAN configure the whole appearance, because this material owns its own BSDF rather than holding a reference to a substrate it cannot reach.  denim: 3/1 twill, indigo warp over undyed weft, cellulose IOR 1.46.  silk: 5-harness satin, a flat 5-degree warp against a twisted 18-degree weft, IOR 1.345 (Sadeghi et al. 2013 Table II crepe de chine).  satin: 5-harness satin, polyester IOR 1.539, a 2.5-degree flat float -- the tightest lobe in the reference table, and the reason satin looks like satin.  linen: plain weave, natural flax, and the one preset with a real `gap` (a plain linen weave is not watertight).  The numbers are calibrated starting points from the paper's fit, not measurements of your fabric; the DYES are representative rather than the paper's own captured samples."; }
						{ auto& p = P(); p.name = "weave"; p.kind = ValueKind::Enum;
						  p.enumValues = {"plain","twill_2_1","twill_3_1","satin_5","custom"};
						  p.defaultValueHint = "per `fabric` (custom: plain)";
						  p.description = "The DRAFT: which family is on top in each cell of the repeating unit, and therefore the warp's mean coverage.  plain = 1/1 alternation (mean 1/2); twill_2_1 = warp over two under one on a diagonal (2/3); twill_3_1 = the denim wale (3/4); satin_5 = 5-harness satin with step 2, long warp floats and no wale at all (4/5); custom = bind your own `coverage` field.  The diagonal is what makes a twill a twill -- the predicate reads (i - j), so the float run marches one cell across for every cell down.  The field is CONTINUOUS, not a per-cell lookup: yarn edges are smooth ramps and the whole pattern fades to its own mean as the pixel footprint outgrows the cell, so it anti-aliases instead of turning into a checkerboard."; }
						{ auto& p = P(); p.name = "transmission"; p.kind = ValueKind::Enum;
						  p.enumValues = {"none","thin"};
						  p.defaultValueHint = "per `fabric` (custom: none)";
						  p.description = "Phase-2 slice P2-B (docs/CLOTH_FABRIC_DESIGN.md 10): `none` (the original slice-A behaviour, bit-identical) or `thin` -- a thin, backlit-glowing cloth.  `thin` adds a DELTA transmission lobe through the open gaps (weighted by `gap`/`sheer`) and a Lambertian diffuse-transmission lobe through the yarn itself (weighted per family by `warp_transmit`/`weft_transmit`), and turns on full-sphere next-event estimation so a light BEHIND the cloth is sampled directly.  linen, silk and satin default to `thin`; denim and custom default to `none` (a 3/1 twill at typical weight is not sheer, and `custom` is the author's own canvas)."; }
						{ auto& p = P(); p.name = "weave_scale"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "per `fabric` (custom: 40)";
						  p.description = "Weave cells per unit of surface UV (physical SCALAR: a scalar_painter name or a single inline scalar).  Higher = finer cloth.  This is the knob that decides whether the weave READS at all: below roughly one cell per two pixels the footprint fade takes over and the surface becomes its own mean, which is correct but structureless.  Match it to the UV scale of your geometry, not to a physical thread count."; }
						{ auto& p = P(); p.name = "weave_rotation"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "0.0";
						  p.description = "Warp direction in RADIANS about the shading normal (physical SCALAR), measured from the surface's own u-axis.  Rotates BOTH families together -- the weft stays perpendicular to the warp -- so this turns the whole cloth, it does not shear it (see `weft_skew` for that).  Paint it to make the grain follow a seam or a drape.  It uses the same helper `fabric_material`'s `weave_rotation` and `ggx_material`'s `tangent_rotation` use, so wrapping this material in a fabric_material makes the two rotations ADD.  0 (the default) aligns the warp with the u-axis."; }
						{ auto& p = P(); p.name = "weft_skew"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "0.0";
						  p.description = "How far the weft departs from perpendicular to the warp, in RADIANS (physical SCALAR).  0 is the orthogonal weave every built-in draft assumes; a non-zero value is a sheared or bias-cut cloth.  Authored rather than derived, because nothing in a draft can imply it."; }
						{ auto& p = P(); p.name = "coverage"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "none";
						  p.description = "`weave custom` ONLY: the warp's share of the surface at each point, in [0,1] (physical SCALAR).  1 = pure warp, 0 = pure weft, and intermediate values blend the two families' lobes.  Bind an expression or texture painter to author a draft the four built-ins do not cover, or to vary the weave across the surface.  IGNORED (with a warning) under a built-in draft, which computes its own field; and selecting `custom` WITHOUT binding this warns too, because the result is a structureless uniform 50/50 blend."; }
						{ auto& p = P(); p.name = "gap"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "per `fabric` (custom: 0)";
						  p.description = "The fraction of the surface covered by NEITHER family -- the holes in an open weave (physical SCALAR).  CLAMPED to [0, 0.3].  Under `transmission none` (the default) a gap simply DARKENS the response: light that finds a hole is lost rather than passed through.  Under `transmission thin` this SAME field is ALSO the aperture of a delta transmission lobe -- light passes straight through the hole undeviated -- which is why `sheer` (below) is the more honest spelling for a thin-cloth author; both names bind the identical slot."; }
						{ auto& p = P(); p.name = "sheer"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "per `fabric` (custom: 0)";
						  p.description = "ALIAS for `gap` -- the identical slot, under the name that actually describes it once `transmission thin` is set: the open-gap fraction that a delta transmission lobe lets straight through.  If both `gap` and `sheer` are authored on the same chunk, `sheer` wins.  Prefer this spelling for a thin/see-through weave; `gap` remains for the reflection-only, `transmission none` case and for back-compatibility with Phase-2 slice A scenes."; }
						{ auto& p = P(); p.name = "warp_color"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Color; p.defaultValueHint = "none";
						  p.description = "The warp's DYE (COLOUR painter -- one of only two genuinely-colour slots on this chunk).  It tints the VOLUME lobe ONLY: a dielectric's specular reflection preserves the incident spectrum, so the surface lobe stays untinted and a coloured fabric keeps a white highlight.  That is the physically correct behaviour and it is what lets a two-tone shot fabric work.  `none` (the default) means the PRESET's colour, or WHITE where the preset sets none -- it is NOT the built-in black `none` painter, which would switch the volume lobe off."; }
						{ auto& p = P(); p.name = "weft_color"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Color; p.defaultValueHint = "none";
						  p.description = "The weft's DYE (COLOUR painter).  Same rules as `warp_color`.  Giving the two families DIFFERENT dyes is what makes a shot fabric, a chambray or a denim: denim's whole look is an indigo warp floating over an undyed weft, and the draft decides how much of each you see."; }
						{ auto& p = P(); p.name = "warp_ior"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "per `fabric` (custom: 1.46)";
						  p.description = "Refractive index of the warp fibre (physical SCALAR).  Splits the two lobes: a higher IOR reflects more at the surface and admits less into the dyed volume, so it reads shinier and less saturated.  The reference table's whole range is 1.345 (silk) to 1.539 (polyester), with 1.46 for cellulose (cotton, linen); values outside 1.001-3 are clamped.  This is a FIBRE property consumed inside the lobe, not a boundary rays cross -- the material reports no refraction to SMS or the IOR stack."; }
						{ auto& p = P(); p.name = "weft_ior"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "per `fabric` (custom: 1.46)";
						  p.description = "Refractive index of the weft fibre (physical SCALAR).  See `warp_ior`."; }
						{ auto& p = P(); p.name = "warp_width"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "per `fabric` (custom: 0.25)";
						  p.description = "LONGITUDINAL width of the warp's highlight, in RADIANS (physical SCALAR) -- the angular spread of the specular band ALONG the yarn.  This is the single most important appearance knob on the chunk: 0.044 (2.5 degrees) is a flat satin float and gives a tight, high-contrast band; 0.52 (30 degrees) is a twisted matte thread and gives a broad wash.  CLAMPED to [0.005, 1].  The volume lobe's width is DERIVED as twice this, which is the ratio that recurs in all six rows of the reference table, so there is deliberately no second slot for it."; }
						{ auto& p = P(); p.name = "weft_width"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "per `fabric` (custom: 0.25)";
						  p.description = "LONGITUDINAL width of the weft's highlight, in RADIANS (physical SCALAR).  See `warp_width`.  Making the two families DIFFERENT is what the silk and satin presets do -- a flat shiny float crossing a twisted matte ground -- and it is a large part of why real satin reads as satin rather than as varnish."; }
						{ auto& p = P(); p.name = "warp_azimuth"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "per `fabric` (custom: 1.0)";
						  p.description = "AZIMUTHAL width of the warp's highlight, in RADIANS (physical SCALAR) -- how far AROUND the yarn the highlight spreads, as opposed to along it.  Broad by nature: a smooth cylinder scatters over most of its visible circumference, so the reference behaviour is around 1.2-1.4 and values below ~0.3 give an unnaturally wire-like thread.  CLAMPED to [0.02, 3].  Narrowing `warp_width` is what sharpens a highlight; narrowing this makes it retro-reflective."; }
						{ auto& p = P(); p.name = "weft_azimuth"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "per `fabric` (custom: 1.0)";
						  p.description = "AZIMUTHAL width of the weft's highlight, in RADIANS (physical SCALAR).  See `warp_azimuth`."; }
						{ auto& p = P(); p.name = "warp_kd"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "per `fabric` (custom: 0.3)";
						  p.description = "How ISOTROPICALLY the warp's volume scatters, in [0,1] (physical SCALAR).  0 keeps the transmitted light in a forward cone about the yarn (silk, polyester); 1 spreads it evenly (cellulose fibres -- cotton, linen -- which really do scatter near-isotropically, which is why they read matte).  The reference table's range is 0.1 for a flat satin float to 0.7 for the matte twisted thread underneath it."; }
						{ auto& p = P(); p.name = "weft_kd"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "per `fabric` (custom: 0.3)";
						  p.description = "How isotropically the weft's volume scatters, in [0,1] (physical SCALAR).  See `warp_kd`."; }
						{ auto& p = P(); p.name = "warp_tilt"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "per `fabric` (custom: 0)";
						  p.description = "FLOAT TILT: how far the warp leans out of the surface plane, in RADIANS (physical SCALAR).  A woven yarn does not lie flat -- it rides over and under its crossings -- and tilting the two families in OPPOSITE directions is what gives a satin face its asymmetric highlight, the one a symmetric lobe cannot produce.  CLAMPED to [-0.17, 0.17] (~10 degrees) -- bounded there, not further out, because the masking term's per-family azimuth has a coordinate pole at view latitude (90 - tilt in degrees); beyond this clamp the pole would sit inside an ordinary, in-frame view angle instead of staying past 80 degrees.  Small opposite values (about +-0.08 to +-0.09) are what the satin and silk presets use.  Large tilts bury whole bands of the yarn below the horizon and cost the sampler a little efficiency, so prefer small ones."; }
						{ auto& p = P(); p.name = "weft_tilt"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "per `fabric` (custom: 0)";
						  p.description = "Float tilt of the weft, in RADIANS (physical SCALAR).  See `warp_tilt` -- give it the OPPOSITE sign to the warp's."; }
						{ auto& p = P(); p.name = "warp_transmit"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "per `fabric` (custom: 0)";
						  p.description = "`transmission thin` ONLY: the warp's share of its OWN volume-scattering budget that is redirected to diffuse transmission through the yarn, CLAMPED to [0,1] (physical SCALAR).  One budget, split -- the reflect-side volume lobe is scaled down by `(1 - warp_transmit)` so the two together never exceed the pre-P2-B total.  Ignored (and always 0) under `transmission none`.  Presets: linen 0.25, silk 0.35, satin 0.15, denim/custom 0.0."; }
						{ auto& p = P(); p.name = "weft_transmit"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; p.defaultValueHint = "per `fabric` (custom: 0)";
						  p.description = "`transmission thin` ONLY: the weft's diffuse-transmission share, in [0,1] (physical SCALAR).  See `warp_transmit`."; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct DielectricMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name",       "noname" );
					// tau is a physical SCALAR (IScalarPainter) -- see the
					// identical note on polished_material::tau.  "0.0"
					// reproduces the pre-refactor "none" IPainter default
					// (black) bit-for-bit: pow(0, distance) = 0, i.e. the
					// medium fully absorbs over any nonzero path length until
					// `tau` is set explicitly (as every real scene already does).
					std::string tau  = bag.GetString( "tau",        "0.0" );
					std::string ior  = bag.GetString( "ior",        "1.33" );
					std::string scat = bag.GetString( "scattering", "10000" );
					bool hg          = bag.GetBool(   "henyey-greenstein", false );

					// AR coating layers in AMBIENT -> SUBSTRATE order (outermost,
					// air-side, first).  Preferred form: repeatable
					// `ar_layer <n> <thickness_nm> [k]` -> a broadband multi-
					// layer stack that reflects faint AND colour-neutral (a
					// single layer leaves the characteristic purple bloom).
					// Bound to the engine cap (DielectricSPF::kMaxARLayers, itself
					// static_assert-tied to ThinFilm::kMaxFilms) so this reject
					// limit can never drift from what the TMM actually evaluates.
					// Reject a too-deep stack rather than silently truncating it.
					const std::size_t kMaxARLayers = (std::size_t)RISE::Implementation::DielectricSPF::kMaxARLayers;
					std::vector<Scalar> arN, arK, arT;
					const std::vector<std::string>& layerLines = bag.GetRepeatable( "ar_layer" );
					if( layerLines.size() > kMaxARLayers ) {
						GlobalLog()->PrintEx( eLog_Error,
							"dielectric_material `%s`: %u ar_layer lines exceeds the %u-layer maximum",
							name.c_str(), (unsigned int)layerLines.size(), (unsigned int)kMaxARLayers );
						return false;
					}
					for( std::size_t i = 0; i < layerLines.size(); ++i ) {
						// Reject nan/inf spellings and non-numeric junk at the STRING
						// layer (value-level isfinite was unreliable under bare -ffast-math (fixed 2026-07-29; the string layer is still preferred);
						// see AllTokensAreFiniteNumbers), then count + range-check the
						// numbers (errno/ERANGE catches overflow like 1e999).
						if( !AllTokensAreFiniteNumbers( layerLines[i].c_str() ) ) {
							GlobalLog()->PrintEx( eLog_Error,
								"dielectric_material `%s`: ar_layer %u (`%s`) has a non-finite or non-numeric token",
								name.c_str(), (unsigned int)i, layerLines[i].c_str() );
							return false;
						}
						double vals[3] = { 0.0, 0.0, 0.0 }; int cnt = 0; bool overflow = false;
						for( const char* p = layerLines[i].c_str(); ; ) {
							while( *p == ' ' || *p == '\t' ) ++p;
							if( !*p || *p == '#' ) break;
							errno = 0; char* end = 0;
							const double v = strtod( p, &end );
							if( end == p ) break;
							if( errno == ERANGE ) overflow = true;
							if( cnt < 3 ) vals[cnt] = v;
							++cnt; p = end;
						}
						// A layer is `<n>0 <thickness_nm>0 [k>=0]`: exactly 2-3 finite
						// numbers, positive index + thickness (a zero-thickness layer
						// is a spurious extra interface), non-negative extinction.
						if( cnt < 2 || cnt > 3 || overflow ||
						    vals[0] <= 0.0 || vals[1] <= 0.0 || ( cnt == 3 && vals[2] < 0.0 ) ) {
							GlobalLog()->PrintEx( eLog_Error,
								"dielectric_material `%s`: ar_layer %u (`%s`) must be `<n>0 <thickness_nm>0 [k>=0]`",
								name.c_str(), (unsigned int)i, layerLines[i].c_str() );
							return false;
						}
						arN.push_back( (Scalar)vals[0] ); arT.push_back( (Scalar)vals[1] ); arK.push_back( (Scalar)( cnt == 3 ? vals[2] : 0.0 ) );
					}
					// Legacy single-layer form, honoured only when no ar_layer
					// lines are present (back-compat, bit-identical).
					if( arN.empty() ) {
						const double ar_ior   = bag.GetDouble( "ar_film_ior",        0.0 );
						const double ar_thick = bag.GetDouble( "ar_film_thickness",  0.0 );
						const double ar_ext   = bag.GetDouble( "ar_film_extinction", 0.0 );
						if( ar_thick > 0.0 ) {
							arN.push_back( (Scalar)ar_ior ); arT.push_back( (Scalar)ar_thick ); arK.push_back( (Scalar)ar_ext );
						}
					}
					const unsigned int nLayers = (unsigned int)arN.size();
					return pJob.AddDielectricMaterial( name.c_str(), tau.c_str(), ior.c_str(), scat.c_str(), hg,
						nLayers ? arN.data() : 0, nLayers ? arK.data() : 0, nLayers ? arT.data() : 0, nLayers );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "dielectric_material"; cd.category = ChunkCategory::Material;
						cd.description = "Fresnel dielectric (reflect + refract) with optional volumetric scattering.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";              p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "tau";               p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Transmittance (scalar_painter, or inline `r g b` or scalar).  0.0 (default) reproduces the pre-refactor \"none\" IPainter default (black) -- fully absorbing until this is set."; p.defaultValueHint = "0.0"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "ior";               p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Index of refraction (scalar_painter, or inline `r g b` or scalar)"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "scattering";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Scattering coefficient (scalar_painter, or inline `r g b` or scalar)"; p.defaultValueHint = "10000"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "ar_layer";           p.kind = ValueKind::String; p.repeatable = true; p.description = "One anti-reflective coating layer (repeatable, AMBIENT->SUBSTRATE / air-side first): `<n> <thickness_nm> [k]`, all positive (k optional, >=0).  One layer = the classic MgF2 quarter-wave (drops glare but leaves a purple bloom); a multi-layer broadband stack (e.g. quarter/half/quarter) reflects far fainter AND colour-neutral, as on real premium AR.  At most 8 layers (more is a parse error)."; }
						{ auto& p = P(); p.name = "ar_film_ior";        p.kind = ValueKind::Double; p.description = "LEGACY single-layer AR film index (e.g. MgF2 1.38); prefer ar_layer.  Honoured only when no ar_layer lines are present. 0 = no coating."; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "ar_film_thickness";  p.kind = ValueKind::Double; p.description = "LEGACY single-layer AR thickness, nm (MgF2 quarter-wave ~99.6 at 550nm); prefer ar_layer. 0 = no coating."; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "ar_film_extinction"; p.kind = ValueKind::Double; p.description = "LEGACY single-layer AR film extinction k (~0 for a transparent AR); prefer ar_layer."; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "henyey-greenstein"; p.kind = ValueKind::Bool;      p.description = "Use Henyey-Greenstein phase"; p.defaultValueHint = "FALSE"; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct SubSurfaceScatteringMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name       = bag.GetString( "name",       "noname" );
					std::string ior        = bag.GetString( "ior",        "1.3" );
					std::string absorption = bag.GetString( "absorption", "0.1" );
					std::string scattering = bag.GetString( "scattering", "1.0" );
					std::string g          = bag.GetString( "g",          "0.0" );
					std::string roughness  = bag.GetString( "roughness",  "0.0" );

					return pJob.AddSubSurfaceScatteringMaterial( name.c_str(), ior.c_str(), absorption.c_str(), scattering.c_str(), g.c_str(), roughness.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "subsurfacescattering_material"; cd.category = ChunkCategory::Material;
						cd.description = "Diffusion-based subsurface scattering material.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";       p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "ior";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Index of refraction (physical SCALAR: a scalar_painter name, or an inline `r g b` or single scalar -- a COLOUR painter does not bind here)"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "absorption"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Absorption coefficient (physical SCALAR: a scalar_painter name, or an inline `r g b` or single scalar -- a COLOUR painter does not bind here)"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "scattering"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Scattering coefficient (physical SCALAR: a scalar_painter name, or an inline `r g b` or single scalar -- a COLOUR painter does not bind here)"; p.semantics.pipe = ParameterPipe::Scalar; }
						// g / roughness are BAKED construction-time scalars (the SPF
						// holds plain doubles) -- Double kind engages the registry's
						// finite-numeric string gate; a painter name here previously
						// slipped through and silently atof'd to 0.0.
						{ auto& p = P(); p.name = "g";          p.kind = ValueKind::Double; p.description = "Henyey-Greenstein g (baked scalar)"; p.defaultValueHint = "0.0"; }
						{ auto& p = P(); p.name = "roughness";  p.kind = ValueKind::Double; p.description = "Surface roughness (baked scalar)"; p.defaultValueHint = "0.0"; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct RandomWalkSSSMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name       = bag.GetString( "name",        "noname" );
					std::string ior        = bag.GetString( "ior",         "1.3" );
					std::string absorption = bag.GetString( "absorption",  "0.1" );
					std::string scattering = bag.GetString( "scattering",  "1.0" );
					std::string g          = bag.GetString( "g",           "0.0" );
					std::string roughness  = bag.GetString( "roughness",   "0.0" );
					std::string maxBounces = bag.GetString( "max_bounces", "64" );

					return pJob.AddRandomWalkSSSMaterial( name.c_str(), ior.c_str(), absorption.c_str(), scattering.c_str(), g.c_str(), roughness.c_str(), maxBounces.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "randomwalk_sss_material"; cd.category = ChunkCategory::Material;
						cd.description = "Random-walk (path-traced) subsurface scattering.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "ior";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Index of refraction (physical SCALAR: a scalar_painter name, or an inline `r g b` or single scalar -- a COLOUR painter does not bind here)"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "absorption";  p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Absorption coefficient (physical SCALAR: a scalar_painter name, or an inline `r g b` or single scalar -- a COLOUR painter does not bind here)"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "scattering";  p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Scattering coefficient (physical SCALAR: a scalar_painter name, or an inline `r g b` or single scalar -- a COLOUR painter does not bind here)"; p.semantics.pipe = ParameterPipe::Scalar; }
						// g / roughness / max_bounces are BAKED construction-time
						// scalars (see the subsurfacescattering_material note).
						{ auto& p = P(); p.name = "g";           p.kind = ValueKind::Double; p.description = "Henyey-Greenstein g (baked scalar)"; p.defaultValueHint = "0.0"; }
						{ auto& p = P(); p.name = "roughness";   p.kind = ValueKind::Double; p.description = "Surface roughness (baked scalar)"; p.defaultValueHint = "0.0"; }
						{ auto& p = P(); p.name = "max_bounces"; p.kind = ValueKind::UInt;   p.description = "Max volume bounces per ray (baked count)"; p.defaultValueHint = "64"; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct LambertianLuminaireMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name     = bag.GetString( "name",     "noname" );
					std::string material = bag.GetString( "material", "none" );
					std::string painter  = bag.GetString( "exitance", "none" );
					double scale         = bag.GetDouble( "scale",    1.0 );

					return pJob.AddLambertianLuminaireMaterial( name.c_str(), painter.c_str(), material.c_str(), scale );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "lambertian_luminaire_material"; cd.category = ChunkCategory::Material;
						cd.description = "Emissive Lambertian material (area light).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";     p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "exitance"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Emitted radiance"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "material"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Material}; p.description = "Underlying material"; p.semantics.pipe = ParameterPipe::Material; }
						{ auto& p = P(); p.name = "scale";    p.kind = ValueKind::Double;    p.description = "Exitance multiplier"; p.defaultValueHint = "1.0"; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct PhongLuminaireMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name     = bag.GetString( "name",     "noname" );
					std::string material = bag.GetString( "material", "none" );
					std::string painter  = bag.GetString( "exitance", "none" );
					double scale         = bag.GetDouble( "scale",    1.0 );
					std::string N        = bag.GetString( "N",        "16.0" );

					return pJob.AddPhongLuminaireMaterial( name.c_str(), painter.c_str(), material.c_str(), N.c_str(), scale );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "phong_luminaire_material"; cd.category = ChunkCategory::Material;
						cd.description = "Emissive Phong (directional) luminaire.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";     p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "exitance"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Emitted radiance"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "material"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Material}; p.description = "Underlying material"; p.semantics.pipe = ParameterPipe::Material; }
						{ auto& p = P(); p.name = "N";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Phong exponent"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "scale";    p.kind = ValueKind::Double;    p.description = "Exitance multiplier"; p.defaultValueHint = "1.0"; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct AshikminShirleyAnisotropicPhongMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					std::string rd   = bag.GetString( "rd",   "none" );
					std::string rs   = bag.GetString( "rs",   "none" );
					std::string Nu   = bag.GetString( "nu",   "10.0" );
					std::string Nv   = bag.GetString( "nv",   "100.0" );

					return pJob.AddAshikminShirleyAnisotropicPhongMaterial( name.c_str(), rd.c_str(), rs.c_str(), Nu.c_str(), Nv.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "ashikminshirley_anisotropicphong_material"; cd.category = ChunkCategory::Material;
						cd.description = "Ashikhmin-Shirley anisotropic Phong BRDF.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name"; p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "rd";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Diffuse reflectance"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "rs";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Specular reflectance"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "nu";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "U-direction exponent (scalar_painter, or inline `r g b` or scalar)"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "nv";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "V-direction exponent (scalar_painter, or inline `r g b` or scalar)"; p.semantics.pipe = ParameterPipe::Scalar; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct IsotropicPhongMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					std::string rd   = bag.GetString( "rd",   "none" );
					std::string rs   = bag.GetString( "rs",   "none" );
					std::string N    = bag.GetString( "N",    "16.0" );

					return pJob.AddIsotropicPhongMaterial( name.c_str(), rd.c_str(), rs.c_str(), N.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "isotropic_phong_material"; cd.category = ChunkCategory::Material;
						cd.description = "Isotropic Phong BRDF.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name"; p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "rd";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Diffuse reflectance"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "rs";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Specular reflectance"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "N";    p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Phong exponent (scalar_painter, or inline `r g b` or scalar)"; p.semantics.pipe = ParameterPipe::Scalar; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct TranslucentMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name",       "noname" );
					std::string ref  = bag.GetString( "ref",        "none" );
					std::string tau  = bag.GetString( "tau",        "none" );
					// ext (extinction) is a physical SCALAR (IScalarPainter) --
					// see the identical note on polished_material::tau.  "0.0"
					// reproduces the pre-refactor "none" IPainter default's
					// NUMERIC VALUE bit-for-bit (black = RISEPel(0,0,0), i.e.
					// 0.0 on every channel) -- but note what that number DOES
					// here is the opposite of "opaque": TranslucentSPF applies
					// it as exp(-extinction*distance) (see TranslucentSPF.cpp),
					// so ext=0.0 means NO extinction, i.e. a fully clear
					// interior, not a black one.  Still the historically
					// correct default -- a bare chunk pre-refactor got the
					// same fully-clear interior, it just failed to parse
					// post-refactor before this fix.
					std::string ext  = bag.GetString( "ext",        "0.0" );
					std::string N    = bag.GetString( "N",          "1.0" );
					std::string scat = bag.GetString( "scattering", "0.0" );

					// Energy conservation: ref + tau must not exceed 1.0 per channel.
					// If violated, create new auto-scaled painters and use those instead.
					std::map<std::string, PainterColor>::const_iterator itRef = s_painterColors.find( ref.c_str() );
					std::map<std::string, PainterColor>::const_iterator itTau = s_painterColors.find( tau.c_str() );

					if( itRef != s_painterColors.end() && itTau != s_painterColors.end() )
					{
						const double* refColor = itRef->second.c;
						const double* tauColor = itTau->second.c;

						bool violated = false;
						for( int ch = 0; ch < 3; ch++ ) {
							if( refColor[ch] + tauColor[ch] > 1.0 ) {
								violated = true;
								break;
							}
						}

						if( violated ) {
							GlobalLog()->PrintEx( eLog_Warning,
								"TranslucentMaterial '%s': ref + tau exceeds 1.0 "
								"(R: %.3f+%.3f=%.3f, G: %.3f+%.3f=%.3f, B: %.3f+%.3f=%.3f), "
								"auto-scaling to conserve energy",
								name.c_str(),
								refColor[0], tauColor[0], refColor[0]+tauColor[0],
								refColor[1], tauColor[1], refColor[1]+tauColor[1],
								refColor[2], tauColor[2], refColor[2]+tauColor[2] );

							double scaledRef[3], scaledTau[3];
							for( int ch = 0; ch < 3; ch++ ) {
								const double sum = refColor[ch] + tauColor[ch];
								if( sum > 1.0 ) {
									const double scale = 1.0 / sum;
									scaledRef[ch] = refColor[ch] * scale;
									scaledTau[ch] = tauColor[ch] * scale;
								} else {
									scaledRef[ch] = refColor[ch];
									scaledTau[ch] = tauColor[ch];
								}
							}

							char buf[256];
							snprintf( buf, sizeof(buf), "%s_auto_ref", name.c_str() );
							pJob.AddUniformColorPainter( buf, scaledRef, "sRGB" );
							ref = buf;

							snprintf( buf, sizeof(buf), "%s_auto_tau", name.c_str() );
							pJob.AddUniformColorPainter( buf, scaledTau, "sRGB" );
							tau = buf;
						}
					}

					return pJob.AddTranslucentMaterial( name.c_str(), ref.c_str(), tau.c_str(), ext.c_str(), N.c_str(), scat.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "translucent_material"; cd.category = ChunkCategory::Material;
						cd.description = "Translucent material combining reflection and transmission.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";       p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "ref";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Reflectance"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "tau";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Transmittance"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "ext";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Extinction, applied as exp(-ext*distance) (physical SCALAR: a scalar_painter name, or an inline `r g b` or single scalar -- a COLOUR painter does not bind here).  0.0 (default) reproduces the pre-refactor \"none\" IPainter default's numeric value bit-for-bit -- which means NO extinction (a fully clear interior), not black/opaque."; p.defaultValueHint = "0.0"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "N";          p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Phong exponent (physical SCALAR: a scalar_painter name, or an inline `r g b` or single scalar -- a COLOUR painter does not bind here)"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "scattering"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Scattering coefficient (physical SCALAR: a scalar_painter name, or an inline `r g b` or single scalar -- a COLOUR painter does not bind here)"; p.semantics.pipe = ParameterPipe::Scalar; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct BioSpecSkinMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name                                = bag.GetString( "name",                                "noname" );
					std::string thickness_SC                        = bag.GetString( "thickness_SC",                        "0.001" );
					std::string thickness_epidermis                 = bag.GetString( "thickness_epidermis",                 "0.01" );
					std::string thickness_papillary_dermis          = bag.GetString( "thickness_papillary_dermis",          "0.02" );
					std::string thickness_reticular_dermis          = bag.GetString( "thickness_reticular_dermis",          "0.18" );
					std::string ior_SC                              = bag.GetString( "ior_SC",                              "1.55" );
					std::string ior_epidermis                       = bag.GetString( "ior_epidermis",                       "1.4" );
					std::string ior_papillary_dermis                = bag.GetString( "ior_papillary_dermis",                "1.36" );
					std::string ior_reticular_dermis                = bag.GetString( "ior_reticular_dermis",                "1.38" );
					std::string concentration_eumelanin             = bag.GetString( "concentration_eumelanin",             "80.0" );
					std::string concentration_pheomelanin           = bag.GetString( "concentration_pheomelanin",           "12.0" );
					std::string melanosomes_in_epidermis            = bag.GetString( "melanosomes_in_epidermis",            "0.10" );
					std::string hb_ratio                            = bag.GetString( "hb_ratio",                            "0.75" );
					std::string whole_blood_in_papillary_dermis     = bag.GetString( "whole_blood_in_papillary_dermis",     "0.012" );
					std::string whole_blood_in_reticular_dermis     = bag.GetString( "whole_blood_in_reticular_dermis",     "0.0091" );
					std::string bilirubin_concentration             = bag.GetString( "bilirubin_concentration",             "0.05" );
					std::string betacarotene_concentration_SC       = bag.GetString( "betacarotene_concentration_SC",       "2.1e-4" );
					std::string betacarotene_concentration_epidermis= bag.GetString( "betacarotene_concentration_epidermis","2.1e-4" );
					std::string betacarotene_concentration_dermis   = bag.GetString( "betacarotene_concentration_dermis",   "7.0e-5" );
					std::string folds_aspect_ratio                  = bag.GetString( "folds_aspect_ratio",                  "0.75" );
					bool bSubdermalLayer                            = bag.GetBool(   "subdermal_layer",                     true );

					return pJob.AddBioSpecSkinMaterial( name.c_str(), thickness_SC.c_str(), thickness_epidermis.c_str(), thickness_papillary_dermis.c_str(), thickness_reticular_dermis.c_str(),
						ior_SC.c_str(), ior_epidermis.c_str(), ior_papillary_dermis.c_str(), ior_reticular_dermis.c_str(), concentration_eumelanin.c_str(), concentration_pheomelanin.c_str(),
						melanosomes_in_epidermis.c_str(), hb_ratio.c_str(), whole_blood_in_papillary_dermis.c_str(), whole_blood_in_reticular_dermis.c_str(),
						bilirubin_concentration.c_str(), betacarotene_concentration_SC.c_str(), betacarotene_concentration_epidermis.c_str(), betacarotene_concentration_dermis.c_str(),
						folds_aspect_ratio.c_str(), bSubdermalLayer );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "biospec_skin_material"; cd.category = ChunkCategory::Material;
						cd.description = "BioSpec multi-layer skin model (Krishnaswamy & Baranoski).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						static const char* painterRefs[] = {
							"thickness_SC","thickness_epidermis","thickness_papillary_dermis","thickness_reticular_dermis",
							"ior_SC","ior_epidermis","ior_papillary_dermis","ior_reticular_dermis",
							"concentration_eumelanin","concentration_pheomelanin","melanosomes_in_epidermis",
							"hb_ratio","whole_blood_in_papillary_dermis","whole_blood_in_reticular_dermis",
							"bilirubin_concentration","betacarotene_concentration_SC","betacarotene_concentration_epidermis","betacarotene_concentration_dermis",
							"folds_aspect_ratio"
						};
						{ auto& p = P(); p.name = "name"; p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						for (const char* n : painterRefs) {
							auto& p = P(); p.name = n; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Skin-model painter parameter";
						}
						{ auto& p = P(); p.name = "subdermal_layer"; p.kind = ValueKind::Bool; p.description = "Include subdermal fat layer"; p.defaultValueHint = "TRUE"; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct DonnerJensenSkinBSSRDFMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					// Donner et al. 2008 spectral skin model parameters
					std::string name                 = bag.GetString( "name",                 "noname" );
					std::string melanin_fraction     = bag.GetString( "melanin_fraction",     "0.02" );
					std::string melanin_blend        = bag.GetString( "melanin_blend",        "0.5" );
					std::string hemoglobin_epidermis = bag.GetString( "hemoglobin_epidermis", "0.002" );
					std::string carotene_fraction    = bag.GetString( "carotene_fraction",    "0.001" );
					std::string hemoglobin_dermis    = bag.GetString( "hemoglobin_dermis",    "0.005" );
					std::string epidermis_thickness  = bag.GetString( "epidermis_thickness",  "0.025" );
					std::string ior_epidermis        = bag.GetString( "ior_epidermis",        "1.4" );
					std::string ior_dermis           = bag.GetString( "ior_dermis",           "1.38" );
					std::string blood_oxygenation    = bag.GetString( "blood_oxygenation",    "0.7" );
					std::string roughness            = bag.GetString( "roughness",            "0.35" );

					return pJob.AddDonnerJensenSkinBSSRDFMaterial( name.c_str(),
						melanin_fraction.c_str(), melanin_blend.c_str(),
						hemoglobin_epidermis.c_str(), carotene_fraction.c_str(),
						hemoglobin_dermis.c_str(), epidermis_thickness.c_str(),
						ior_epidermis.c_str(), ior_dermis.c_str(),
						blood_oxygenation.c_str(), roughness.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "donner_jensen_skin_bssrdf_material"; cd.category = ChunkCategory::Material;
						cd.description = "Donner & Jensen 2008 spectral skin BSSRDF.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						// roughness is NOT in this list -- it is a BAKED construction-
						// time scalar (plain double through the ctor), declared Double
						// below so the finite-numeric string gate applies.
						static const char* painterRefs[] = {
							"melanin_fraction","melanin_blend","hemoglobin_epidermis","carotene_fraction",
							"hemoglobin_dermis","epidermis_thickness","ior_epidermis","ior_dermis",
							"blood_oxygenation"
						};
						{ auto& p = P(); p.name = "name"; p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						for (const char* n : painterRefs) {
							auto& p = P(); p.name = n; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Skin-model painter parameter";
						}
						{ auto& p = P(); p.name = "roughness"; p.kind = ValueKind::Double; p.description = "Surface roughness (baked scalar)"; p.defaultValueHint = "0.35"; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct GenericHumanTissueMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name              = bag.GetString( "name",                      "noname" );
					// g (HG asymmetry) is a physical SCALAR (IScalarPainter) --
					// see the identical note on polished_material::tau.  "0.0"
					// reproduces the pre-refactor "none" IPainter default
					// (black) bit-for-bit, and happens to double as the
					// physically sensible "isotropic phase function" default.
					std::string g                 = bag.GetString( "g",                         "0.0" );
					std::string sca               = bag.GetString( "sca",                       "0.85" );
					double hb_ratio               = bag.GetDouble( "hb_ratio",                  0.75 );
					double whole_blood            = bag.GetDouble( "whole_blood",               0.012 );
					double bilirubin_concentration= bag.GetDouble( "bilirubin_concentration",   0.05 );
					double betacarotene_concentration = bag.GetDouble( "betacarotene_concentration", 7.0e-5 );
					bool diffuse                  = bag.GetBool(   "diffuse",                   true );

					return pJob.AddGenericHumanTissueMaterial( name.c_str(), sca.c_str(), g.c_str(), whole_blood, hb_ratio, bilirubin_concentration, betacarotene_concentration, diffuse );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "generic_human_tissue_material"; cd.category = ChunkCategory::Material;
						cd.description = "Parametric human-tissue scattering material.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";                     p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "sca";                      p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Scattering amplitude"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "g";                        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Phase-function asymmetry.  0.0 (default) reproduces the pre-refactor \"none\" IPainter default (black) and doubles as isotropic scattering."; p.defaultValueHint = "0.0"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "whole_blood";              p.kind = ValueKind::Double;    p.description = "Blood volume fraction"; p.defaultValueHint = "0.012"; }
						{ auto& p = P(); p.name = "hb_ratio";                 p.kind = ValueKind::Double;    p.description = "Oxygenated hemoglobin ratio"; p.defaultValueHint = "0.75"; }
						{ auto& p = P(); p.name = "bilirubin_concentration";  p.kind = ValueKind::Double;    p.description = "Bilirubin concentration"; p.defaultValueHint = "0.05"; }
						{ auto& p = P(); p.name = "betacarotene_concentration";p.kind = ValueKind::Double;   p.description = "Beta-carotene concentration"; p.defaultValueHint = "7.0e-5"; }
						{ auto& p = P(); p.name = "diffuse";                  p.kind = ValueKind::Bool;      p.description = "Use diffuse approximation"; p.defaultValueHint = "TRUE"; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct CompositeMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name           = bag.GetString( "name",                      "noname" );
					std::string top            = bag.GetString( "top",                       "none" );
					std::string bottom         = bag.GetString( "bottom",                    "none" );
					unsigned int max_recur     = bag.GetUInt(   "max_recursion",             3 );
					unsigned int max_refl_recur= bag.GetUInt(   "max_reflection_recursion",  3 );
					unsigned int max_refr_recur= bag.GetUInt(   "max_refraction_recursion",  3 );
					unsigned int max_diff_recur= bag.GetUInt(   "max_diffuse_recursion",     3 );
					unsigned int max_tran_recur= bag.GetUInt(   "max_translucent_recursion", 3 );
					double thickness           = bag.GetDouble( "thickness",                 0.0 );
					// Default "0.0", not the "none" IPainter sentinel: `extinction` is a
					// physical-scalar slot now, and the null painter's numeric value WAS
					// 0 -- so 0.0 reproduces the pre-refactor default bit-for-bit while
					// resolving through the scalar-painter path.  Same move translucent
					// _material's `ext` made.
					std::string extinction     = bag.GetString( "extinction",                "0.0" );

					return pJob.AddCompositeMaterial( name.c_str(), top.c_str(), bottom.c_str(), max_recur, max_refl_recur, max_refr_recur, max_diff_recur, max_tran_recur, thickness, extinction.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "composite_material"; cd.category = ChunkCategory::Material;
						cd.description = "Layered composite of two materials separated by a translucent interior.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";                 p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "top";                  p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Material}; p.description = "Top material"; p.semantics.pipe = ParameterPipe::Material; }
						{ auto& p = P(); p.name = "bottom";               p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Material}; p.description = "Bottom material"; p.semantics.pipe = ParameterPipe::Material; }
						{ auto& p = P(); p.name = "max_recursion";            p.kind = ValueKind::UInt; p.description = "Max composite recursion"; p.defaultValueHint = "3"; }
						{ auto& p = P(); p.name = "max_reflection_recursion"; p.kind = ValueKind::UInt; p.description = "Max reflection recursion"; p.defaultValueHint = "3"; }
						{ auto& p = P(); p.name = "max_refraction_recursion"; p.kind = ValueKind::UInt; p.description = "Max refraction recursion"; p.defaultValueHint = "3"; }
						{ auto& p = P(); p.name = "max_diffuse_recursion";    p.kind = ValueKind::UInt; p.description = "Max diffuse recursion"; p.defaultValueHint = "3"; }
						{ auto& p = P(); p.name = "max_translucent_recursion";p.kind = ValueKind::UInt; p.description = "Max translucent recursion"; p.defaultValueHint = "3"; }
						{ auto& p = P(); p.name = "thickness";            p.kind = ValueKind::Double;    p.description = "Layer thickness"; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "extinction";           p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Inter-layer extinction, applied as exp(-extinction*path) across the gap (physical SCALAR: a scalar_painter name, or an inline `r g b` or single scalar -- a COLOUR painter does not bind here).  0.0 (default) reproduces the pre-refactor \"none\" IPainter default's numeric value bit-for-bit -- NO extinction (a clear gap), not black/opaque."; p.defaultValueHint = "0.0"; p.semantics.pipe = ParameterPipe::Scalar; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct WardIsotropicGaussianMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name  = bag.GetString( "name",  "noname" );
					std::string rd    = bag.GetString( "rd",    "none" );
					std::string rs    = bag.GetString( "rs",    "none" );
					std::string alpha = bag.GetString( "alpha", "0.1" );

					return pJob.AddWardIsotropicGaussianMaterial( name.c_str(), rd.c_str(), rs.c_str(), alpha.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "ward_isotropic_material"; cd.category = ChunkCategory::Material;
						cd.description = "Ward isotropic Gaussian BRDF.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";  p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "rd";    p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Diffuse reflectance"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "rs";    p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Specular reflectance"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "alpha"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Surface slope RMS (scalar_painter, or inline `r g b` or scalar)"; p.semantics.pipe = ParameterPipe::Scalar; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct WardAnisotropicEllipticalGaussianMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name   = bag.GetString( "name",   "noname" );
					std::string rd     = bag.GetString( "rd",     "none" );
					std::string rs     = bag.GetString( "rs",     "none" );
					std::string alphax = bag.GetString( "alphax", "0.1" );
					std::string alphay = bag.GetString( "alphay", "0.2" );

					return pJob.AddWardAnisotropicEllipticalGaussianMaterial( name.c_str(), rd.c_str(), rs.c_str(), alphax.c_str(), alphay.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "ward_anisotropic_material"; cd.category = ChunkCategory::Material;
						cd.description = "Ward anisotropic elliptical-Gaussian BRDF.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";   p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "rd";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Diffuse reflectance"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "rs";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Specular reflectance"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "alphax"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "X-direction slope RMS (scalar_painter, or inline `r g b` or scalar)"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "alphay"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Y-direction slope RMS (scalar_painter, or inline `r g b` or scalar)"; p.semantics.pipe = ParameterPipe::Scalar; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct GGXMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name       = bag.GetString( "name",       "noname" );
					std::string rd         = bag.GetString( "rd",         "none" );
					std::string rs         = bag.GetString( "rs",         "none" );
					std::string alphax     = bag.GetString( "alphax",     "0.15" );
					std::string alphay     = bag.GetString( "alphay",     "0.15" );
					std::string ior        = bag.GetString( "ior",        "2.45" );
					std::string extinction = bag.GetString( "extinction", "3.45" );
					// Optional emissive: when present, fold a LambertianEmitter
					// into the material so glTF pbrMetallicRoughness emissives
					// can be expressed as one chunk instead of needing a
					// separate composite_emitter wrapper.
					std::string emissive   = bag.GetString( "emissive",        "none" );
					double emissive_scale  = bag.GetDouble( "emissive_scale",  1.0 );
					std::string fresnelMode = bag.GetString( "fresnel_mode",   "conductor" );
					// Thin-film FILM slots (eFresnelThinFilmConductor).  Default
					// "none" => no film painter; Job::AddGGXMaterial enforces that
					// thinfilm mode supplies film_ior + film_thickness (the P2-B
					// BSDF contract dereferences them when the mode is selected).
					std::string filmIor       = bag.GetString( "film_ior",        "none" );
					std::string filmExtinction= bag.GetString( "film_extinction", "none" );
					std::string filmThickness = bag.GetString( "film_thickness",  "none" );
					// P0-A: tangent-frame rotation (painter OR scalar radians; "none" =
					// aligned with the geometry tangent).  Job::AddGGXMaterial + the GGX
					// BRDF already apply it (ResolveTangentONB/RotateTangent) -- this just
					// stops ggx_material hard-coding "none".  Steers groove-aligned anisotropy.
					std::string tangentRot    = bag.GetString( "tangent_rotation", "none" );

					if( emissive == "none" ) {
						return pJob.AddGGXMaterial( name.c_str(), rd.c_str(), rs.c_str(),
							alphax.c_str(), alphay.c_str(), ior.c_str(), extinction.c_str(),
							fresnelMode.c_str(), tangentRot.c_str(),
							filmIor.c_str(), filmExtinction.c_str(), filmThickness.c_str() );
					}
					return pJob.AddGGXEmissiveMaterial( name.c_str(), rd.c_str(), rs.c_str(),
						alphax.c_str(), alphay.c_str(), ior.c_str(), extinction.c_str(),
						emissive.c_str(), emissive_scale, fresnelMode.c_str(), tangentRot.c_str(),
						filmIor.c_str(), filmExtinction.c_str(), filmThickness.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "ggx_material"; cd.category = ChunkCategory::Material;
						cd.description = "GGX (Trowbridge-Reitz) microfacet BRDF with optional Fresnel.  "
							"Optional `emissive` painter folds a LambertianEmitter into the material "
							"(matches glTF pbrMetallicRoughness with non-zero emissiveFactor / "
							"emissiveTexture); leave at default for non-emissive surfaces.  "
							"`fresnel_mode` selects Fresnel evaluation: `conductor` (default) uses "
							"the ior + extinction painters with a real conductor Fresnel and `rs` as "
							"a tint; `schlick_f0` uses Schlick's F0-shaped approximation, treating "
							"`rs` as F0 directly and ignoring ior + extinction (required by glTF "
							"metallicRoughness PBR mapping; pbr_metallic_roughness_material picks "
							"this automatically); `thinfilm` evaluates thin-film interference "
							"(heat-tint / anodization color) on an air / oxide-film / metal stack — "
							"`ior` + `extinction` carry the SUBSTRATE (metal) complex index and the "
							"three `film_*` slots carry the oxide film (docs/THIN_FILM_INTERFERENCE.md).  "
							"`thinfilm` REQUIRES `film_ior` and `film_thickness` (`film_extinction` optional).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";           p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "rd";             p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Diffuse reflectance"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "rs";             p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Specular reflectance / F0"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "alphax";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "X roughness (physical SCALAR: a scalar_painter name, or an inline `r g b` or single scalar -- a COLOUR painter does not bind here)"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "alphay";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Y roughness (physical SCALAR: a scalar_painter name, or an inline `r g b` or single scalar -- a COLOUR painter does not bind here)"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "ior";            p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Fresnel IOR (ignored in schlick_f0 mode) (physical SCALAR: a scalar_painter name, or an inline `r g b` or single scalar -- a COLOUR painter does not bind here)"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "extinction";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Fresnel extinction (ignored in schlick_f0 mode) (physical SCALAR: a scalar_painter name, or an inline `r g b` or single scalar -- a COLOUR painter does not bind here)"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "emissive";       p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Optional emissive painter (LambertianEmitter folded in when present)"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "emissive_scale"; p.kind = ValueKind::Double;    p.description = "Multiplier on emissive radiance"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "fresnel_mode";   p.kind = ValueKind::String;    p.description = "Fresnel model: conductor | schlick_f0 | thinfilm"; p.defaultValueHint = "conductor"; }
						{ auto& p = P(); p.name = "film_ior";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Thin-film oxide n (scalar_painter; eFresnelThinFilmConductor only)"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "film_extinction"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Thin-film oxide k (scalar_painter; eFresnelThinFilmConductor only; default 0/none = transparent film)"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "film_thickness";  p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Thin-film oxide thickness in nm (scalar_painter, may be spatially varying; eFresnelThinFilmConductor only)"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "tangent_rotation"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Anisotropy tangent-frame rotation in RADIANS: a colour-painter reference (an expression_function2d gives a SPATIALLY-VARYING field, e.g. groove direction) OR an inline scalar.  \"none\" = aligned with the geometry tangent.  Resolved in the colour-painter manager, so a scalar_painter does NOT bind here -- use expression_function2d or a scalar.  Steers alphax!=alphay anisotropy."; p.defaultValueHint = "none"; p.semantics.pipe = ParameterPipe::Color; p.semantics.note = "The documented oddball (ISCALARPAINTER_REFACTOR.md / MATERIAL_EDITOR.md sect. 1): an angle in radians by MEANING, plumbed through the Color pipe (IPainter) so an expression_function2d painter can drive a spatially-varying groove direction. A scalar_painter does NOT bind here."; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct PBRMetallicRoughnessMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name                = bag.GetString( "name",                 "noname" );
					std::string base_color          = bag.GetString( "base_color",           "none" );
					std::string metallic            = bag.GetString( "metallic",             "0.0" );
					std::string roughness           = bag.GetString( "roughness",            "0.5" );
					double      ior                 = bag.GetDouble( "ior",                  1.5 );
					std::string emissive            = bag.GetString( "emissive",             "none" );
					double      emissive_scale      = bag.GetDouble( "emissive_scale",       1.0 );
					// Landing 7 (KHR_materials_specular)
					std::string specular_factor     = bag.GetString( "specular_factor",      "1.0" );
					std::string specular_color      = bag.GetString( "specular_color",       "none" );
					// Landing 8 (KHR_materials_anisotropy)
					std::string anisotropy_factor   = bag.GetString( "anisotropy_factor",    "0.0" );
					std::string anisotropy_rotation = bag.GetString( "anisotropy_rotation",  "0.0" );
					return pJob.AddPBRMetallicRoughnessMaterial(
						name.c_str(), base_color.c_str(),
						metallic.c_str(), roughness.c_str(),
						ior, emissive.c_str(), emissive_scale,
						specular_factor.c_str(),
						specular_color.c_str(),
						anisotropy_factor.c_str(),
						anisotropy_rotation.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "pbr_metallic_roughness_material"; cd.category = ChunkCategory::Material;
						cd.description = "glTF 2.0 pbrMetallicRoughness material.  Single-chunk authoring "
							"surface that takes baseColor / metallic / roughness / emissive painters and "
							"composes the equivalent ggx_material under the hood.  The internal mapping is: "
							"`F0 = lerp(0.04, baseColor, metallic)` for the GGX specular reflectance, "
							"`baseColor * (1 - metallic)` for the diffuse colour painter, "
							"`roughness * roughness` for the GGX α (isotropic).  The (1 - max(F0)) "
							"diffuse-vs-specular energy split is applied by the BSDF at evaluation time "
							"(not pre-multiplied into the diffuse painter).  Fresnel mode is forced to "
							"`schlick_f0`, which treats `F0` as the Schlick-approximation F0 directly and "
							"ignores the `ior` argument (preserved for API stability only).  "
							"Internal painters live under prefix `__pbrmr_<name>__`; don't reference "
							"them by name.  `metallic` and `roughness` can be either a painter reference "
							"or a scalar string like \"0.5\" (auto-promoted to a uniformcolor painter).  "
							"`emissive` is optional; pass \"none\" or omit for non-emissive surfaces. "
							"Full design + Phase 3 status in docs/GLTF_IMPORT.md §4 and §13.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";           p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "base_color";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.required = true; p.description = "Base color painter (sRGB-decoded RGB; texture or uniform)"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "metallic";       p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Metallic painter or scalar string (default 0.0)"; p.defaultValueHint = "0.0"; p.semantics.pipe = ParameterPipe::Color; p.semantics.note = "Color pipe by construction, not by meaning: Job::AddPBRMetallicRoughnessMaterial's resolveOrSynth() checks pPntManager->GetItem() and falls back to atof()+a synthesized uniform-colour painter on a miss -- a scalar_painter name here is NOT found (wrong manager) and silently synthesizes a ZERO-valued painter rather than binding. MATERIAL_EDITOR.md sect. 6.4's \"pbr's colour-manager roughness\" oddball."; }
						{ auto& p = P(); p.name = "roughness";      p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Roughness painter or scalar string (default 0.5)"; p.defaultValueHint = "0.5"; p.semantics.pipe = ParameterPipe::Color; p.semantics.note = "Color pipe by construction, not by meaning: Job::AddPBRMetallicRoughnessMaterial's resolveOrSynth() checks pPntManager->GetItem() and falls back to atof()+a synthesized uniform-colour painter on a miss -- a scalar_painter name here is NOT found (wrong manager) and silently synthesizes a ZERO-valued painter rather than binding. MATERIAL_EDITOR.md sect. 6.4's \"pbr's colour-manager roughness\" oddball."; }
						{ auto& p = P(); p.name = "ior";            p.kind = ValueKind::Double;    p.description = "Preserved for API stability; ignored under the schlick_f0 Fresnel path that this chunk forces"; p.defaultValueHint = "1.5"; }
						{ auto& p = P(); p.name = "emissive";       p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Optional emissive painter; pass \"none\" / omit for non-emissive"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "emissive_scale"; p.kind = ValueKind::Double;    p.description = "Multiplier on emissive radiance"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "specular_factor";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Landing 7 / KHR_materials_specular: scalar in [0, 1] (or scalar painter) scaling the dielectric F0.  Default 1.0 = standard 0.04 dielectric F0; lower values reduce the dielectric specular highlight (matte plastic / paint).  Metals are unaffected (their F0 = baseColor).  Accepts a painter reference OR a literal scalar string like \"0.5\"."; p.defaultValueHint = "1.0"; p.semantics.pipe = ParameterPipe::Color; p.semantics.note = "Color pipe by construction, not by meaning: Job::AddPBRMetallicRoughnessMaterial's resolveOrSynth() checks pPntManager->GetItem() and falls back to atof()+a synthesized uniform-colour painter on a miss -- a scalar_painter name here is NOT found (wrong manager) and silently synthesizes a ZERO-valued painter rather than binding. MATERIAL_EDITOR.md sect. 6.4's \"pbr's colour-manager roughness\" oddball."; }
						{ auto& p = P(); p.name = "specular_color";    p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Landing 7 / KHR_materials_specular: RGB tint on dielectric F0.  Default \"none\" = white (untinted, F0 stays at 0.04 across all wavelengths).  Set to a non-white painter for measured dielectrics where the Fresnel response varies with wavelength.  Final F0 = 0.04 × specular_color × specular_factor."; p.defaultValueHint = "none"; p.semantics.pipe = ParameterPipe::Color; p.semantics.note = "Color pipe by construction, not by meaning: Job::AddPBRMetallicRoughnessMaterial's resolveOrSynth() checks pPntManager->GetItem() and falls back to atof()+a synthesized uniform-colour painter on a miss -- a scalar_painter name here is NOT found (wrong manager) and silently synthesizes a ZERO-valued painter rather than binding. MATERIAL_EDITOR.md sect. 6.4's \"pbr's colour-manager roughness\" oddball."; }
						{ auto& p = P(); p.name = "anisotropy_factor"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Landing 8 / KHR_materials_anisotropy: scalar in [0, 1] (or scalar painter) controlling specular-lobe stretch along the tangent direction.  Default 0 = isotropic (αx = αy = roughness²; bit-identical to pre-L8 PBR-MR).  Larger values stretch the lobe: αt = mix(α, 1, anisotropy²), αb = α.  Useful for brushed metal, hair, fabric.  Accepts a painter reference OR a literal scalar string."; p.defaultValueHint = "0.0"; p.semantics.pipe = ParameterPipe::Color; p.semantics.note = "Color pipe by construction, not by meaning: Job::AddPBRMetallicRoughnessMaterial's resolveOrSynth() checks pPntManager->GetItem() and falls back to atof()+a synthesized uniform-colour painter on a miss -- a scalar_painter name here is NOT found (wrong manager) and silently synthesizes a ZERO-valued painter rather than binding. MATERIAL_EDITOR.md sect. 6.4's \"pbr's colour-manager roughness\" oddball."; }
						{ auto& p = P(); p.name = "anisotropy_rotation"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Landing 8 / KHR_materials_anisotropy: tangent-frame rotation in RADIANS (or scalar painter).  Default 0 = aligned with the geometry's TANGENT attribute.  Phase 1 reads but does not yet APPLY the rotation; rotation is wired alongside the anisotropy_texture in L12.  Document for forward compatibility."; p.defaultValueHint = "0.0"; p.semantics.pipe = ParameterPipe::Color; p.semantics.note = "Same oddball as GGX's tangent_rotation (see there): an angle by meaning, Color pipe by construction (Job.cpp checks pPntManager->GetItem(anisotropy_rotation) directly)."; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct CookTorranceMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name       = bag.GetString( "name",       "noname" );
					std::string rd         = bag.GetString( "rd",         "none" );
					std::string rs         = bag.GetString( "rs",         "none" );
					std::string facets     = bag.GetString( "facets",     "0.15" );
					std::string ior        = bag.GetString( "ior",        "2.45" );
					std::string extinction = bag.GetString( "extinction", "1" );
					// Thin-film interference is intentionally GGX-only; reject it
					// here with a clear, specific diagnostic rather than silently
					// ignoring the mode (the CT BSDF has no thin-film Fresnel path).
					std::string fresnelMode = bag.GetString( "fresnel_mode", "conductor" );
					if( fresnelMode == "thinfilm" ) {
						GlobalLog()->PrintEx( eLog_Error,
							"cooktorrance_material `%s`: fresnel_mode `thinfilm` is not supported on Cook-Torrance (thin-film interference is GGX-only).  Use a ggx_material with fresnel_mode thinfilm + film_ior / film_extinction / film_thickness instead (docs/THIN_FILM_INTERFERENCE.md §7).",
							name.c_str() );
						return false;
					}
					if( fresnelMode != "conductor" ) {
						GlobalLog()->PrintEx( eLog_Error,
							"cooktorrance_material `%s`: unknown fresnel_mode `%s`.  Cook-Torrance supports only `conductor`.",
							name.c_str(), fresnelMode.c_str() );
						return false;
					}

					return pJob.AddCookTorranceMaterial( name.c_str(), rd.c_str(), rs.c_str(), facets.c_str(), ior.c_str(), extinction.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "cooktorrance_material"; cd.category = ChunkCategory::Material;
						cd.description = "Cook-Torrance microfacet BRDF.  Only `fresnel_mode conductor` is supported; "
							"`thinfilm` is GGX-only and rejected with a diagnostic.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";       p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "rd";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Diffuse reflectance"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "rs";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Specular reflectance"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "facets";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Microfacet slope distribution (scalar_painter, or inline `r g b` or scalar)"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "ior";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Fresnel IOR (scalar_painter, or inline `r g b` or scalar)"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "extinction"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Fresnel extinction (scalar_painter, or inline `r g b` or scalar)"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "fresnel_mode"; p.kind = ValueKind::String; p.description = "Fresnel model: conductor (only).  `thinfilm` is GGX-only and rejected here."; p.defaultValueHint = "conductor"; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct OrenNayarMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",        "noname" );
					std::string reflectance = bag.GetString( "reflectance", "none" );
					std::string roughness   = bag.GetString( "roughness",   "0.5" );

					return pJob.AddOrenNayarMaterial( name.c_str(), reflectance.c_str(), roughness.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "orennayar_material"; cd.category = ChunkCategory::Material;
						cd.description = "Oren-Nayar rough-diffuse BRDF.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "reflectance"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Albedo"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "roughness";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Surface roughness sigma (scalar_painter, or inline `r g b` or scalar)"; p.semantics.pipe = ParameterPipe::Scalar; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct SheenMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name      = bag.GetString( "name",            "noname" );
					std::string color     = bag.GetString( "sheen_color",     "none" );
					std::string roughness = bag.GetString( "sheen_roughness", "0.5" );
					return pJob.AddSheenMaterial( name.c_str(), color.c_str(), roughness.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "sheen_material"; cd.category = ChunkCategory::Material;
						// NOT "Charlie / Neubelt": CharlieSheen::V is the full
						// Lambda-polynomial Estevez & Kulla visibility, and the
						// cheap Neubelt closed form was REPLACED in 2026-05
						// because it blew up to rho ~ 8.7 at grazing.  Corrected
						// per docs/CLOTH_FABRIC_DESIGN.md section 2 debt 4.
						cd.description = "Charlie sheen BRDF (Estevez & Kulla 2017, with the full Lambda-polynomial visibility) for fabric / cloth surfaces.  "
							"Designed as the top layer in a CompositeMaterial(top=sheen, bottom=base) "
							"pairing for glTF KHR_materials_sheen, but usable standalone.  No diffuse "
							"or Fresnel — the layer just adds the colour-tinted grazing scatter "
							"characteristic of velvet, suede, satin.  Roughness controls how broad "
							"the lobe is (low = sharp grazing highlights, high = diffuse-like sheen).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";            p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "sheen_color";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.required = true; p.description = "Sheen tint (typical 0..1)"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "sheen_roughness"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Sheen roughness painter or scalar (clamped to >= 1e-3 internally)"; p.defaultValueHint = "0.5"; p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct SchlickMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name      = bag.GetString( "name",      "noname" );
					std::string rd        = bag.GetString( "rd",        "none" );
					std::string rs        = bag.GetString( "rs",        "none" );
					std::string roughness = bag.GetString( "roughness", "0.05" );
					std::string isotropy  = bag.GetString( "isotropy",  "1" );

					return pJob.AddSchlickMaterial( name.c_str(), rd.c_str(), rs.c_str(), roughness.c_str(), isotropy.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "schlick_material"; cd.category = ChunkCategory::Material;
						cd.description = "Schlick BRDF approximation.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";      p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "rd";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Diffuse reflectance"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "rs";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Specular reflectance"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "roughness"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Surface roughness (scalar_painter, or inline `r g b` or scalar)"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "isotropy";  p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Isotropy factor (scalar_painter, or inline `r g b` or scalar)"; p.semantics.pipe = ParameterPipe::Scalar; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			struct DataDrivenMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name     = bag.GetString( "name",     "noname" );
					std::string filename = bag.GetString( "filename", "" );

					return pJob.AddDataDrivenMaterial( name.c_str(), filename.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "datadriven_material"; cd.category = ChunkCategory::Material;
						cd.description = "Data-driven BRDF loaded from file (MERL format).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";     p.kind = ValueKind::String;   p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "filename"; p.kind = ValueKind::Filename; p.description = "BRDF data file"; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};

			// hair_material -- Chiang et al. 2016 near-field hair/fur BCSDF
			// (docs/HAIR_FUR_DESIGN.md).  Registers `HairMaterial`
			// (src/Library/Materials/HairMaterial.h), a real evaluable-anywhere
			// IBSDF + ISPF pair.  The descriptor's `color` / `sigma_a` /
			// `eumelanin` / `pheomelanin` defaults are all "none" (tier not
			// bound); Job::AddHairMaterial enforces that EXACTLY one of the
			// three tiers (melanin counts as one, whether one or both of
			// eumelanin/pheomelanin are set) ends up bound, with a diagnostic
			// naming all three options otherwise.
			struct HairMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",        "noname" );
					std::string color       = bag.GetString( "color",       "none" );
					std::string sigma_a     = bag.GetString( "sigma_a",     "none" );
					std::string eumelanin   = bag.GetString( "eumelanin",   "none" );
					std::string pheomelanin = bag.GetString( "pheomelanin", "none" );
					std::string beta_m      = bag.GetString( "beta_m",      "0.3" );
					std::string beta_n      = bag.GetString( "beta_n",      "0.3" );
					std::string alpha       = bag.GetString( "alpha",       "2.0" );
					std::string ior         = bag.GetString( "ior",         "1.55" );
					std::string medRatio    = bag.GetString( "medulla_ratio",   "0" );
					std::string medScatter  = bag.GetString( "medulla_scatter", "0.5" );
					std::string medG        = bag.GetString( "medulla_g",       "0.4" );

					return pJob.AddHairMaterial( name.c_str(), color.c_str(), sigma_a.c_str(),
						eumelanin.c_str(), pheomelanin.c_str(), beta_m.c_str(), beta_n.c_str(),
						alpha.c_str(), ior.c_str(),
						medRatio.c_str(), medScatter.c_str(), medG.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "hair_material"; cd.category = ChunkCategory::Material;
						cd.description = "Chiang et al. 2016 near-field hair/fur BCSDF (docs/HAIR_FUR_DESIGN.md).  "
							"Exactly ONE colour tier must be bound: `color` (Tier 3, artist reflectance -- "
							"inverted per Chiang's sigma_a = (ln C / D(beta_n))^2), `sigma_a` (Tier 2, direct "
							"per-wavelength absorption -- power users / measured data), or `eumelanin` / "
							"`pheomelanin` (Tier 1, melanin concentration -- physically-based, recommended "
							"default; either or both may be set, counting as the SINGLE melanin tier).  "
							"Zero tiers or two-or-more tiers bound is a parse-time error.  Hair lobes are "
							"never delta (SMS never sees hair) and are all tagged eRayReflection.  "
							"For ANIMAL FUR, `medulla_ratio` > 0 additionally enables the Yan et al. 2017 "
							"scattering medulla (two extra lobes, TTs and TRTs); it is 0 by default, which "
							"is the correct value for human hair and reproduces the plain Chiang model.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "color";       p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Tier 3: artist reflectance colour painter.  \"none\" (default) = tier not bound."; p.defaultValueHint = "none"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "sigma_a";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Tier 2: direct absorption coefficient per unit fibre diameter, per RGB channel (physical SCALAR: a scalar_painter name, or an inline `r g b` or single scalar).  \"none\" (default) = tier not bound."; p.defaultValueHint = "none"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "eumelanin";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Tier 1: eumelanin concentration (physical SCALAR, single value, >= 0; ~1.3 ~= brown-black hair).  \"none\" (default) = not bound; combines with `pheomelanin` as ONE tier."; p.defaultValueHint = "none"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "pheomelanin"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Tier 1: pheomelanin concentration (physical SCALAR, single value, >= 0; redheads).  \"none\" (default) = not bound; combines with `eumelanin` as ONE tier."; p.defaultValueHint = "none"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "beta_m";      p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Longitudinal roughness (physical SCALAR, single value; clamped to [0.05, 1] at evaluation)."; p.defaultValueHint = "0.3"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "beta_n";      p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Azimuthal roughness (physical SCALAR, single value; clamped to [0.05, 1] at evaluation)."; p.defaultValueHint = "0.3"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "alpha";       p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Cuticle scale tilt, in DEGREES (physical SCALAR, single value)."; p.defaultValueHint = "2.0"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "ior";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Fibre index of refraction (physical SCALAR, single value)."; p.defaultValueHint = "1.55"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "medulla_ratio";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "ANIMAL FUR: Yan et al. 2017 medulla radius ratio kappa = r_medulla / r_fibre (physical SCALAR, single value; clamped to [0, 0.95], so a negative value reads as 0).  0 -- the DEFAULT -- disables the medulla entirely and reproduces the plain Chiang model bit for bit; human hair wants 0, animal fur wants ~0.5-0.9.  Turning it on splits the TT and TRT lobes into unscattered and medulla-scattered halves, which is what makes fur read soft and saturated instead of thin and shiny.  Cycles has no medulla analogue, so the Blender bridge never sets this."; p.defaultValueHint = "0"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "medulla_scatter"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "ANIMAL FUR: medulla scattering coefficient sigma_m, in the same per-fibre-diameter units as `sigma_a` (physical SCALAR, single value).  A value of 0 -- or any negative value -- disables the medulla just as `medulla_ratio` 0 does; it is not diagnosed, so check the sign if fur renders like plain hair.  Ignored entirely when `medulla_ratio` is 0."; p.defaultValueHint = "0.5"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "medulla_g";       p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "ANIMAL FUR: Henyey-Greenstein anisotropy of the medulla phase function (physical SCALAR, single value, in (-1, 1); the baked profile table spans +/- 0.8 and clamps).  Positive = forward scattering.  Ignored when `medulla_ratio` is 0."; p.defaultValueHint = "0.4"; p.semantics.pipe = ParameterPipe::Scalar; }
						AddVariantTagParam( cd );
						return cd;
					}();
					return d;
				}
			};


			//////////////////////////////////////////
			// Scene-level options (top-of-file scope)
			//////////////////////////////////////////

			// `scene_options` declares the world-unit scale.  Stored
			// in thread_local parser state and consumed by camera
			// chunks at parse time.  Place AT OR NEAR THE TOP of the
			// .RISEscene file (before any camera) — values declared
			// after a camera don't reach back.  Same declaration-order
			// rule as `standard_shader`, `camera_defaults`, etc.
			//
			// Today this only governs camera lens-mm-to-scene-unit
			// conversion; future phases (volumetric atmosphere,
			// physical sky, sensor noise) will consume the same scale.
			struct SceneOptionsAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& /*pJob*/ ) const override
				{
					if( bag.Has( "scene_unit" ) ) {
						const double v = bag.GetDouble( "scene_unit" );
						if( v <= 0.0 ) {
							GlobalLog()->PrintEx( eLog_Error,
								"scene_options:: scene_unit must be positive (got %g). "
								"It's the meters-per-scene-unit factor — 1.0 for meters, "
								"0.001 for mm, 0.0254 for inches.", v );
							return false;
						}
						// Catch the misplaced-block footgun: if a camera
						// was already finalized with the old scene_unit,
						// changing it now has no effect on that camera
						// (its lens math is locked in).  Warn loudly so
						// the user notices rather than silently rendering
						// a 1000×-misscaled image.
						if( s_sceneOptions.camera_committed
						    && v != s_sceneOptions.scene_unit_meters ) {
							GlobalLog()->PrintEx( eLog_Warning,
								"scene_options:: declared AFTER a camera was already finalized. "
								"The camera locked in scene_unit = %g; this block sets %g but the "
								"camera's lens math is unchanged. Move `scene_options` to the top "
								"of the .RISEscene file, before any camera chunk.",
								s_sceneOptions.scene_unit_meters, v );
						}
						s_sceneOptions.scene_unit_meters = v;
					}
					return true;
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "scene_options"; cd.category = ChunkCategory::Camera;
						cd.description = "Scene-level options. Currently sets the world-unit scale that bridges scene-geometry units to mm-input on cameras. Place near the top of the file (before any camera).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{
							auto& p = P();
							p.name = "scene_unit"; p.kind = ValueKind::Double;
							p.description = "Meters per scene unit. Default 1.0 = scenes are in meters. Set 0.001 for mm-scale scenes, 0.0254 for inches, etc. Affects camera lens-input conversion (sensor_size / focal_length / shift_x/y are mm in the scene file regardless of this value; the camera converts to scene units via this factor).";
							p.defaultValueHint = "1.0";
							p.unitLabel = "m / unit";
							p.presets = {
								{ "Meters (default)", "1.0" },
								{ "Centimetres",      "0.01" },
								{ "Millimetres",      "0.001" },
								{ "Inches",           "0.0254" },
								{ "Feet",             "0.3048" },
							};
						}
						return cd;
					}();
					return d;
				}
			};

			//////////////////////////////////////////
			// Cameras
			//////////////////////////////////////////

			// Scene-level camera defaults.  Consumed by thinlens_camera
			// when a per-camera value is omitted.  Place this chunk
			// AT OR NEAR THE TOP of the .RISEscene file (before any
			// camera that should use it) — values declared after a
			// camera don't reach back.  Same declaration-order rule
			// as `standard_shader` and the other scene-level blocks.
			//
			// Currently sets fallbacks for the photographic-quartet
			// scalars; tilt/shift are deliberately not fallbackable
			// because they're shot-specific and tend to need per-camera
			// override even within a single scene.
			struct CameraDefaultsAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& /*pJob*/ ) const override
				{
					if( bag.Has( "sensor_size" ) ) {
						s_cameraDefaults.sensor_size     = bag.GetDouble( "sensor_size" );
						s_cameraDefaults.has_sensor_size = true;
					}
					if( bag.Has( "focal_length" ) ) {
						s_cameraDefaults.focal_length     = bag.GetDouble( "focal_length" );
						s_cameraDefaults.has_focal_length = true;
					}
					if( bag.Has( "fstop" ) ) {
						s_cameraDefaults.fstop     = bag.GetDouble( "fstop" );
						s_cameraDefaults.has_fstop = true;
					}
					return true;
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "camera_defaults"; cd.category = ChunkCategory::Camera;
						cd.description = "Scene-level fallback values for thinlens_camera. Cameras declared AFTER this chunk pick up these values when they omit the corresponding parameter; cameras declared before are unaffected.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{
							auto& p = P();
							p.name = "sensor_size"; p.kind = ValueKind::Double;
							p.description = "Default sensor width in MILLIMETRES.";
							p.defaultValueHint = "36";
							p.unitLabel = "mm";
							p.presets = {
								{ "Full-frame 35mm",         "36" },
								{ "APS-C (Sony/Nikon/Fuji)", "23.6" },
								{ "APS-C (Canon)",           "22.3" },
								{ "Super 35 (cinema)",       "24.89" },
								{ "Micro Four Thirds",       "17.3" },
								{ "Vista Vision",            "37.72" },
								{ "IMAX 70mm",               "70.41" },
								{ "645 medium format",       "56" },
								{ "6×7 medium format",       "70" },
								{ "6×9 medium format",       "84" },
								{ "4×5 large format",        "121" },
								{ "8×10 large format",       "254" },
							};
						}
						{ auto& p = P(); p.name = "focal_length"; p.kind = ValueKind::Double; p.description = "Default lens focal length in MILLIMETRES."; p.defaultValueHint = "35"; p.unitLabel = "mm"; }
						{ auto& p = P(); p.name = "fstop";        p.kind = ValueKind::Double; p.description = "Default f-number (dimensionless)."; p.defaultValueHint = "2.8"; }
						return cd;
					}();
					return d;
				}
			};

			struct PinholeCameraAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string requestedCameraName = bag.GetString( "name", "" );
					if( RejectReservedCameraName( "pinhole_camera", requestedCameraName ) ) return false;
					std::string name = AllocateCameraName( requestedCameraName );
					double fov          = 30.0 * DEG_TO_RAD;
					if( bag.Has( "fov" ) ) fov = bag.GetDouble( "fov" ) * DEG_TO_RAD;
					double exposure     = bag.GetDouble( "exposure",      0 );
					double scanningRate = bag.GetDouble( "scanning_rate", 0 );
					double pixelRate    = bag.GetDouble( "pixel_rate",    0 );
					// Landing 5: photographic exposure metadata.  Both
					// default 0 (= disabled), so any pre-L5 scene that
					// omits these keeps its exact pre-L5 behaviour.
					double iso          = bag.GetDouble( "iso",           0.0 );
					double fstop        = bag.GetDouble( "fstop",         0.0 );

					double loc[3]    = {0,0,0};
					double lookat[3] = {0,0,-1};
					double up[3]     = {0,1,0};
					bag.GetVec3( "location", loc );
					bag.GetVec3( "lookat",   lookat );
					bag.GetVec3( "up",       up );

					double orientation[3] = {0,0,0};
					if( bag.Has( "orientation" ) ) {
						bag.GetVec3( "orientation", orientation );
					}
					if( bag.Has( "pitch" ) ) orientation[0] = bag.GetDouble( "pitch" );
					if( bag.Has( "roll" ) )  orientation[1] = bag.GetDouble( "roll" );
					if( bag.Has( "yaw" ) )   orientation[2] = bag.GetDouble( "yaw" );

					double target_orientation[2] = {0,0};
					if( bag.Has( "target_orientation" ) ) {
						sscanf( bag.GetString( "target_orientation" ).c_str(), "%lf %lf", &target_orientation[0], &target_orientation[1] );
					}
					if( bag.Has( "theta" ) ) target_orientation[0] = bag.GetDouble( "theta" );
					if( bag.Has( "phi" ) )   target_orientation[1] = bag.GetDouble( "phi" );

					orientation[0] *= DEG_TO_RAD;
					orientation[1] *= DEG_TO_RAD;
					orientation[2] *= DEG_TO_RAD;

					target_orientation[0] *= DEG_TO_RAD;
					target_orientation[1] *= DEG_TO_RAD;

					// Landing 5: validate physical exposure parameters at
					// the parser layer so authors get clear feedback
					// before construction.  The camera constructor's own
					// guard returns 0 evCompensation as a fallback.
					if( iso > 0.0 ) {
						if( fstop <= 0.0 ) {
							GlobalLog()->PrintEx( eLog_Error,
								"pinhole_camera:: `iso` is set (%g) but `fstop` is missing or non-positive (%g).  "
								"Pinhole cameras have no geometric DOF, so `fstop` is used only for EV computation; "
								"set it to a typical photographic value (e.g. f/2.8 - f/16).",
								iso, fstop );
							return false;
						}
						if( exposure <= 0.0 ) {
							GlobalLog()->PrintEx( eLog_Error,
								"pinhole_camera:: `iso` is set (%g) but `exposure` (shutter time, seconds) is %g.  "
								"Physical exposure requires a real shutter time; set `exposure` to e.g. 0.004 for 1/250 s.",
								iso, exposure );
							return false;
						}
					}

					return pJob.AddPinholeCamera( name.c_str(), loc, lookat, up, fov, exposure, scanningRate, pixelRate, orientation, target_orientation, iso, fstop );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "pinhole_camera"; cd.category = ChunkCategory::Camera;
						cd.description = "Standard perspective (pinhole) camera.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "fov"; p.kind = ValueKind::Double; p.description = "Field of view (degrees)"; p.defaultValueHint = "45"; }
						AddCameraCommonParams( P );
						{ auto& p = P(); p.name = "iso";   p.kind = ValueKind::Double; p.description = "Landing 5: ISO sensitivity (sensor speed).  Default 0 = physical exposure DISABLED — the camera contributes no exposure compensation and existing scenes render bit-identically.  When > 0, the camera computes an EV-stops compensation from (iso, fstop, exposure) and stacks it ADDITIVELY into LDR outputs (PNG / JPEG); HDR archival outputs (EXR / RGBE) ignore it to preserve linear-radiance ground truth.  Requires `fstop` > 0 and `exposure` (shutter time, seconds) > 0.  Typical values: 100 (lowest noise), 400 (interior), 1600 (low light).  Pinhole has no geometric DOF, so `fstop` only drives the EV stack."; p.defaultValueHint = "0 (disabled)"; }
						{ auto& p = P(); p.name = "fstop"; p.kind = ValueKind::Double; p.description = "Landing 5: f-number for EV computation.  Required when `iso > 0`; ignored otherwise.  Typical values: f/1.4 (very fast lens), f/2.8 (fast prime), f/8 (balanced), f/16 (sunny outdoor)."; p.defaultValueHint = "0 (disabled)"; }
						return cd;
					}();
					return d;
				}
			};

			struct ONBPinholeCameraAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string requestedCameraName = bag.GetString( "name", "" );
					if( RejectReservedCameraName( "onb_pinhole_camera", requestedCameraName ) ) return false;
					std::string name = AllocateCameraName( requestedCameraName );
					double fov          = 30.0 * DEG_TO_RAD;
					if( bag.Has( "fov" ) ) fov = bag.GetDouble( "fov" ) * DEG_TO_RAD;
					double exposure     = bag.GetDouble( "exposure",      0 );
					double scanningRate = bag.GetDouble( "scanning_rate", 0 );
					double pixelRate    = bag.GetDouble( "pixel_rate",    0 );
					String components   = String( bag.GetString( "components" ).c_str() );

					double loc[3] = {0,0,0};
					double vA[3]  = {0,0,0};
					double vB[3]  = {0,0,0};
					bag.GetVec3( "location", loc );
					bag.GetVec3( "va",       vA );
					bag.GetVec3( "vb",       vB );

					OrthonormalBasis3D	onb;

					if( components == "UV" ) {
						onb.CreateFromUV( vA, vB );
					} else if( components == "VU" ) {
						onb.CreateFromVU( vA, vB );
					} else if( components == "UW" ) {
						onb.CreateFromUW( vA, vB );
					} else if( components == "WU" ) {
						onb.CreateFromWU( vA, vB );
					} else if( components == "VW" ) {
						onb.CreateFromVW( vA, vB );
					} else if( components == "WV" ) {
						onb.CreateFromWV( vA, vB );
					} else {
						GlobalLog()->PrintEx( eLog_Error, "ONBPinholeCameraAsciiChunkParser:: Unknown component type `%s`", components.c_str() );
							return false;
					}

					double ONB_U[3] = {onb.u().x, onb.u().y, onb.u().z};
					double ONB_V[3] = {onb.v().x, onb.v().y, onb.v().z};
					double ONB_W[3] = {onb.w().x, onb.w().y, onb.w().z};
					return pJob.AddPinholeCameraONB( name.c_str(), ONB_U, ONB_V, ONB_W, loc, fov, exposure, scanningRate, pixelRate );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "onb_pinhole_camera"; cd.category = ChunkCategory::Camera;
						cd.description = "Pinhole camera oriented via an orthonormal basis built from two axes (va, vb) plus a `components` selector.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";          p.kind = ValueKind::String;     p.description = "Optional identifier; defaults to \"default\" with auto-suffix on collision."; p.defaultValueHint = "default"; }
						{ auto& p = P(); p.name = "location";      p.kind = ValueKind::DoubleVec3; p.description = "Eye position"; }
						{ auto& p = P(); p.name = "va";            p.kind = ValueKind::DoubleVec3; p.description = "First ONB axis (used per `components`)"; }
						{ auto& p = P(); p.name = "vb";            p.kind = ValueKind::DoubleVec3; p.description = "Second ONB axis (used per `components`)"; }
						{ auto& p = P(); p.name = "components";    p.kind = ValueKind::Enum;       p.enumValues = {"UV","VU","UW","WU","VW","WV"}; p.description = "Which ONB components va/vb represent"; }
						{ auto& p = P(); p.name = "fov";           p.kind = ValueKind::Double;     p.description = "Field of view (degrees)"; p.defaultValueHint = "30"; }
						// width / height / pixelAR moved to the `film` chunk
						// in scene format v6 (Phase B2 of the Camera/Film/Output
						// split).  Camera chunks are now imaging-only.
						{ auto& p = P(); p.name = "exposure";      p.kind = ValueKind::Double;     p.description = "Shutter exposure"; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "scanning_rate"; p.kind = ValueKind::Double;     p.description = "Rolling-shutter rate"; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "pixel_rate";    p.kind = ValueKind::Double;     p.description = "Per-pixel time offset"; p.defaultValueHint = "0"; }
						return cd;
					}();
					return d;
				}
			};

			// thinlens_camera takes the photographic quartet
			// (sensor_size, focal_length, fstop, focus_distance) plus
			// optional aperture-shape params (blades, rotation,
			// anamorphic squeeze).  The pre-photographic parameters
			// `fov` and `aperture_size` are no longer accepted; the
			// descriptor-driven parser rejects them automatically with
			// "parameter not declared in descriptor" because they're
			// absent from Describe() below.  Migration formula in
			// docs/CAMERAS_ROADMAP.md Phase 1.0.
			struct ThinlensCameraAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string requestedCameraName = bag.GetString( "name", "" );
					if( RejectReservedCameraName( "thinlens_camera", requestedCameraName ) ) return false;
					std::string name = AllocateCameraName( requestedCameraName );
					// Photographic quartet — units (Phase 1.2):
					//
					//   sensor_size, focal_length, shift_x, shift_y
					//     in MILLIMETRES (the photographic convention).
					//   focus_distance
					//     in scene units (matches geometry coords —
					//     "focus at 5 metres" reads naturally in a
					//     metres scene as `focus_distance 5`).
					//
					// The renderer converts mm-input → scene-units
					// internally via the scene-level `scene_options
					// { scene_unit ... }` factor (default 1.0 = scenes
					// in metres).  This keeps the editor stable on mm
					// regardless of geometry unit, and keeps the lens
					// equation v=f·u/(u-f) unit-consistent inside the
					// camera.  See ThinLensCamera::Recompute().
					if( !bag.Has( "focus_distance" ) ) {
						GlobalLog()->PrintEx( eLog_Error,
							"thinlens_camera:: `focus_distance` is required and has no default — set it explicitly to "
							"the focus plane distance in your scene's unit (must be > focal_length-in-scene-units)." );
						return false;
					}
					double focus        = bag.GetDouble( "focus_distance" );
					// Per-camera value wins; otherwise fall through to
					// scene-level `camera_defaults` (if declared earlier
					// in the file); otherwise the hard-coded photographic
					// default.  Defaults are in mm.
					double sensor       = bag.Has( "sensor_size"  ) ? bag.GetDouble( "sensor_size"  )
					                    : s_cameraDefaults.has_sensor_size  ? s_cameraDefaults.sensor_size
					                    : 36.0;
					double focal        = bag.Has( "focal_length" ) ? bag.GetDouble( "focal_length" )
					                    : s_cameraDefaults.has_focal_length ? s_cameraDefaults.focal_length
					                    : 35.0;
					double fstop        = bag.Has( "fstop"        ) ? bag.GetDouble( "fstop"        )
					                    : s_cameraDefaults.has_fstop        ? s_cameraDefaults.fstop
					                    : 2.8;
					// Aperture-shape (Phase 1.0 enrichments).
					unsigned int blades = bag.GetUInt(   "aperture_blades",   0 );
					double rotation     = bag.GetDouble( "aperture_rotation", 0 );
					double squeeze      = bag.GetDouble( "anamorphic_squeeze", 1.0 );
					// Tilt-shift (Phase 1.1).  Tilt is degrees in the
					// scene file (parser converts to radians); shift is
					// MILLIMETRES.  Defaults of 0 give a plain
					// perpendicular-focus thin-lens (bit-identical to
					// Phase 1.0 output).
					double tilt_x       = bag.GetDouble( "tilt_x",            0 );
					double tilt_y       = bag.GetDouble( "tilt_y",            0 );
					double shift_x      = bag.GetDouble( "shift_x",           0 );
					double shift_y      = bag.GetDouble( "shift_y",           0 );

					// Scene-level unit scale (Phase 1.2).  Default 1.0
					// (metres scene); set via `scene_options { scene_unit ... }`
					// at the top of the .RISEscene file.
					const double sceneUnitMeters = s_sceneOptions.scene_unit_meters;

					double exposure     = bag.GetDouble( "exposure",       0 );
					double scanningRate = bag.GetDouble( "scanning_rate",  0 );
					double pixelRate    = bag.GetDouble( "pixel_rate",     0 );
					// Landing 5: photographic exposure metadata.  Default
					// 0 (= disabled) preserves pre-L5 behaviour.  When > 0,
					// fstop and exposure (already read above) must be > 0.
					double iso          = bag.GetDouble( "iso",            0.0 );

					double loc[3]    = {0,0,0};
					double lookat[3] = {0,0,-1};
					double up[3]     = {0,1,0};
					bag.GetVec3( "location", loc );
					bag.GetVec3( "lookat",   lookat );
					bag.GetVec3( "up",       up );

					double orientation[3] = {0,0,0};
					if( bag.Has( "orientation" ) ) {
						bag.GetVec3( "orientation", orientation );
					}
					if( bag.Has( "pitch" ) ) orientation[0] = bag.GetDouble( "pitch" );
					if( bag.Has( "roll" ) )  orientation[1] = bag.GetDouble( "roll" );
					if( bag.Has( "yaw" ) )   orientation[2] = bag.GetDouble( "yaw" );

					double target_orientation[2] = {0,0};
					if( bag.Has( "target_orientation" ) ) {
						sscanf( bag.GetString( "target_orientation" ).c_str(), "%lf %lf", &target_orientation[0], &target_orientation[1] );
					}
					if( bag.Has( "theta" ) ) target_orientation[0] = bag.GetDouble( "theta" );
					if( bag.Has( "phi" ) )   target_orientation[1] = bag.GetDouble( "phi" );

					if( fstop <= 0.0 ) {
						GlobalLog()->PrintEx( eLog_Error, "thinlens_camera:: fstop must be positive; got %g", fstop );
						return false;
					}
					if( sensor <= 0.0 || focal <= 0.0 ) {
						GlobalLog()->PrintEx( eLog_Error,
							"thinlens_camera:: sensor_size (%g mm) and focal_length (%g mm) must both be positive", sensor, focal );
						return false;
					}
					// Validate focus_distance > focal_length, both in
					// SCENE UNITS — the lens equation v=f·u/(u-f) gives
					// a negative film distance for u <= f.  focus is
					// already in scene units; convert focal mm → scene
					// units before comparing.
					{
						const double mm_to_scene  = 0.001 / sceneUnitMeters;
						const double focal_scene  = focal * mm_to_scene;
						if( focus <= focal_scene ) {
							GlobalLog()->PrintEx( eLog_Error,
								"thinlens_camera:: focus_distance (%g scene units) must be greater than "
								"focal_length (%g mm = %g scene units, given scene_unit = %g m). "
								"For a 35mm lens in a metres scene, set focus_distance to at least 0.05 (5cm); "
								"the lens equation v=f·u/(u-f) requires u > f.",
								focus, focal, focal_scene, sceneUnitMeters );
							return false;
						}
					}

					orientation[0] *= DEG_TO_RAD;
					orientation[1] *= DEG_TO_RAD;
					orientation[2] *= DEG_TO_RAD;

					target_orientation[0] *= DEG_TO_RAD;
					target_orientation[1] *= DEG_TO_RAD;

					rotation *= DEG_TO_RAD;
					tilt_x   *= DEG_TO_RAD;
					tilt_y   *= DEG_TO_RAD;

					// Cap tilt at ±80° (1.396 rad).  Beyond this,
					// off-axis chief rays may go parallel to (or
					// beyond) the focal plane, producing 1/0 in the
					// focus-point math.  Real tilt-shift lenses
					// max out around ±15°; an 80° ceiling leaves
					// enough headroom for stylized renders without
					// risking divide-by-zero NaN frames.
					{
						constexpr double kMaxTiltRad = 1.396;       // 80°, just shy of pi/2
						if( fabs( tilt_x ) >= kMaxTiltRad || fabs( tilt_y ) >= kMaxTiltRad ) {
							GlobalLog()->PrintEx( eLog_Error,
								"thinlens_camera:: |tilt_x| (%g°) and |tilt_y| (%g°) must each be < 80°. "
								"Real tilt-shift lenses max out around 15°; values closer to 90° would make "
								"chief rays parallel to the focal plane (divide-by-zero).",
								tilt_x * RAD_TO_DEG, tilt_y * RAD_TO_DEG );
							return false;
						}
					}

					// Mark the scene_options state as locked-in so a
					// later `scene_options` block can warn the user
					// it's too late to affect this camera.
					s_sceneOptions.camera_committed = true;

					// Landing 5: validate physical exposure parameters
					// at the parser layer.  thinlens already requires
					// fstop > 0 (validated above), so we only need to
					// check exposure here.
					if( iso > 0.0 && exposure <= 0.0 ) {
						GlobalLog()->PrintEx( eLog_Error,
							"thinlens_camera:: `iso` is set (%g) but `exposure` (shutter time, seconds) is %g.  "
							"Physical exposure requires a real shutter time; set `exposure` to e.g. 0.004 for 1/250 s.",
							iso, exposure );
						return false;
					}

					return pJob.AddThinlensCamera( name.c_str(), loc, lookat, up, sensor, focal, fstop, focus, sceneUnitMeters, exposure, scanningRate, pixelRate, orientation, target_orientation, blades, rotation, squeeze, tilt_x, tilt_y, shift_x, shift_y, iso );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "thinlens_camera"; cd.category = ChunkCategory::Camera;
						cd.description = "Perspective camera with thin-lens depth of field, parameterised photographically (sensor_size + focal_length + fstop + focus_distance).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{
							auto& p = P();
							p.name = "sensor_size"; p.kind = ValueKind::Double;
							p.description = "Sensor width in MILLIMETRES (always — the renderer converts to scene units via scene_options.scene_unit). 36 = full-frame 35mm.";
							p.defaultValueHint = "36";
							p.unitLabel = "mm";
							// Surface the canonical photographic formats as
							// quick-pick combo entries.  Values are in mm
							// directly — no scaling needed regardless of
							// the scene's geometry unit.
							p.presets = {
								{ "Full-frame 35mm",         "36" },
								{ "APS-C (Sony/Nikon/Fuji)", "23.6" },
								{ "APS-C (Canon)",           "22.3" },
								{ "Super 35 (cinema)",       "24.89" },
								{ "Micro Four Thirds",       "17.3" },
								{ "Vista Vision",            "37.72" },
								{ "IMAX 70mm",               "70.41" },
								{ "645 medium format",       "56" },
								{ "6×7 medium format",       "70" },
								{ "6×9 medium format",       "84" },
								{ "4×5 large format",        "121" },
								{ "8×10 large format",       "254" },
							};
						}
						{ auto& p = P(); p.name = "focal_length";       p.kind = ValueKind::Double; p.description = "Lens focal length in MILLIMETRES. 35 is 'natural' for a 36mm sensor; 50 is 'normal'; 24 is wide; 85 is portrait; 200+ is telephoto."; p.defaultValueHint = "35"; p.unitLabel = "mm"; }
						{ auto& p = P(); p.name = "fstop";              p.kind = ValueKind::Double; p.description = "f-number (aperture diameter = focal_length / fstop)."; p.defaultValueHint = "2.8"; }
						{ auto& p = P(); p.name = "focus_distance";     p.kind = ValueKind::Double; p.required = true; p.description = "Focus plane distance in SCENE UNITS (matches geometry coords — e.g. `focus_distance 5` means 5 scene units, which is 5 metres in a default metres scene). No default — set per scene. Must be greater than focal_length-converted-to-scene-units."; p.unitLabel = "scene units"; }
						{ auto& p = P(); p.name = "aperture_blades";    p.kind = ValueKind::UInt;   p.description = "Polygonal aperture blades; 0 = perfect disk, typical cinematic 5-9."; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "aperture_rotation";  p.kind = ValueKind::Double; p.description = "Polygon rotation in degrees."; p.defaultValueHint = "0"; p.unitLabel = "°"; }
						{ auto& p = P(); p.name = "anamorphic_squeeze"; p.kind = ValueKind::Double; p.description = "Aperture x-axis scale for oval bokeh (1.0 = circular)."; p.defaultValueHint = "1.0"; }
						// Tilt-shift (Phase 1.1).  Tilt rotates the
						// FOCAL plane (Scheimpflug); shift translates
						// the IMAGE plane (architectural correction).
						{ auto& p = P(); p.name = "tilt_x";             p.kind = ValueKind::Double; p.description = "Focal-plane tilt around camera x-axis in degrees (Scheimpflug). Default 0 = perpendicular focus plane. |tilt_x| must be < 80°."; p.defaultValueHint = "0"; p.unitLabel = "°"; }
						{ auto& p = P(); p.name = "tilt_y";             p.kind = ValueKind::Double; p.description = "Focal-plane tilt around camera y-axis in degrees (Scheimpflug). |tilt_y| must be < 80°."; p.defaultValueHint = "0"; p.unitLabel = "°"; }
						{ auto& p = P(); p.name = "shift_x";            p.kind = ValueKind::Double; p.description = "Lens shift along camera x-axis in MILLIMETRES. Positive = lens shifts right (camera 'looks right', image content moves left in frame). Matches Blender Cycles convention."; p.defaultValueHint = "0"; p.unitLabel = "mm"; }
						{ auto& p = P(); p.name = "shift_y";            p.kind = ValueKind::Double; p.description = "Lens shift along camera y-axis in MILLIMETRES. Positive = lens shifts up (camera 'looks up', image content moves down in frame). Architectural correction. Matches Blender Cycles convention."; p.defaultValueHint = "0"; p.unitLabel = "mm"; }
						AddCameraCommonParams( P );
						{ auto& p = P(); p.name = "iso"; p.kind = ValueKind::Double; p.description = "Landing 5: ISO sensitivity (sensor speed).  Default 0 = physical exposure DISABLED — the camera contributes no exposure compensation and existing scenes render bit-identically.  When > 0, the camera computes an EV-stops compensation from (iso, fstop, exposure) and stacks it ADDITIVELY into LDR outputs (PNG / JPEG); HDR archival outputs (EXR / RGBE) ignore it.  Reuses `fstop` (the same number that controls DOF) and `exposure` (the same number that controls motion blur) — physically consistent because the same aperture and shutter govern both effects.  Requires `exposure` > 0 when set.  Typical values: 100 (lowest noise), 400 (interior), 1600 (low light)."; p.defaultValueHint = "0 (disabled)"; }
						return cd;
					}();
					return d;
				}
			};

			// `realistic_camera` keyword is reserved for the future
			// multi-element lens-system camera (see
			// docs/CAMERAS_ROADMAP.md Phase 4).  The previous stub
			// (which delegated to thin-lens) was removed as part of
			// the Phase 1.0 thin-lens parameter overhaul; existing
			// scenes were migrated to `thinlens_camera`.

			struct FisheyeCameraAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string requestedCameraName = bag.GetString( "name", "" );
					if( RejectReservedCameraName( "fisheye_camera", requestedCameraName ) ) return false;
					std::string name = AllocateCameraName( requestedCameraName );
					double exposure     = bag.GetDouble( "exposure",      0 );
					double scanningRate = bag.GetDouble( "scanning_rate", 0 );
					double pixelRate    = bag.GetDouble( "pixel_rate",    0 );
					double scale        = bag.GetDouble( "scale",         1.0 );

					double loc[3]    = {0,0,0};
					double lookat[3] = {0,0,-1};
					double up[3]     = {0,1,0};
					bag.GetVec3( "location", loc );
					bag.GetVec3( "lookat",   lookat );
					bag.GetVec3( "up",       up );

					double orientation[3] = {0,0,0};
					if( bag.Has( "orientation" ) ) {
						bag.GetVec3( "orientation", orientation );
					}
					if( bag.Has( "pitch" ) ) orientation[0] = bag.GetDouble( "pitch" );
					if( bag.Has( "roll" ) )  orientation[1] = bag.GetDouble( "roll" );
					if( bag.Has( "yaw" ) )   orientation[2] = bag.GetDouble( "yaw" );

					double target_orientation[2] = {0,0};
					if( bag.Has( "target_orientation" ) ) {
						sscanf( bag.GetString( "target_orientation" ).c_str(), "%lf %lf", &target_orientation[0], &target_orientation[1] );
					}
					if( bag.Has( "theta" ) ) target_orientation[0] = bag.GetDouble( "theta" );
					if( bag.Has( "phi" ) )   target_orientation[1] = bag.GetDouble( "phi" );

					orientation[0] *= DEG_TO_RAD;
					orientation[1] *= DEG_TO_RAD;
					orientation[2] *= DEG_TO_RAD;

					target_orientation[0] *= DEG_TO_RAD;
					target_orientation[1] *= DEG_TO_RAD;

					return pJob.AddFisheyeCamera( name.c_str(), loc, lookat, up, exposure, scanningRate, pixelRate, orientation, target_orientation, scale );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "fisheye_camera"; cd.category = ChunkCategory::Camera;
						cd.description = "Fisheye (equidistant) camera.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "scale"; p.kind = ValueKind::Double; p.description = "Fisheye scale factor"; p.defaultValueHint = "1.0"; }
						AddCameraCommonParams( P );
						return cd;
					}();
					return d;
				}
			};

			struct OrthographicCameraAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string requestedCameraName = bag.GetString( "name", "" );
					if( RejectReservedCameraName( "orthographic_camera", requestedCameraName ) ) return false;
					std::string name = AllocateCameraName( requestedCameraName );
					double exposure   = bag.GetDouble( "exposure",      0 );
					double scanningRate = bag.GetDouble( "scanning_rate", 0 );
					double pixelRate    = bag.GetDouble( "pixel_rate",    0 );

					double loc[3]    = {0,0,0};
					double lookat[3] = {0,0,-1};
					double up[3]     = {0,1,0};
					bag.GetVec3( "location", loc );
					bag.GetVec3( "lookat",   lookat );
					bag.GetVec3( "up",       up );

					double vpscale[2] = {1.0,1.0};
					if( bag.Has( "viewport_scale" ) ) {
						sscanf( bag.GetString( "viewport_scale" ).c_str(), "%lf %lf", &vpscale[0], &vpscale[1] );
					}

					double orientation[3] = {0,0,0};
					if( bag.Has( "orientation" ) ) {
						bag.GetVec3( "orientation", orientation );
					}
					if( bag.Has( "pitch" ) ) orientation[0] = bag.GetDouble( "pitch" );
					if( bag.Has( "roll" ) )  orientation[1] = bag.GetDouble( "roll" );
					if( bag.Has( "yaw" ) )   orientation[2] = bag.GetDouble( "yaw" );

					double target_orientation[2] = {0,0};
					if( bag.Has( "target_orientation" ) ) {
						sscanf( bag.GetString( "target_orientation" ).c_str(), "%lf %lf", &target_orientation[0], &target_orientation[1] );
					}
					if( bag.Has( "theta" ) ) target_orientation[0] = bag.GetDouble( "theta" );
					if( bag.Has( "phi" ) )   target_orientation[1] = bag.GetDouble( "phi" );

					orientation[0] *= DEG_TO_RAD;
					orientation[1] *= DEG_TO_RAD;
					orientation[2] *= DEG_TO_RAD;

					target_orientation[0] *= DEG_TO_RAD;
					target_orientation[1] *= DEG_TO_RAD;

					return pJob.AddOrthographicCamera( name.c_str(), loc, lookat, up, vpscale, exposure, scanningRate, pixelRate, orientation, target_orientation );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "orthographic_camera"; cd.category = ChunkCategory::Camera;
						cd.description = "Orthographic (parallel-projection) camera.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "viewport_scale"; p.kind = ValueKind::Double; p.description = "Orthographic viewport scale"; p.defaultValueHint = "1.0"; }
						AddCameraCommonParams( P );
						return cd;
					}();
					return d;
				}
			};

			//////////////////////////////////////////
			// Film — pixel-grid descriptor
			//
			// The `film` chunk authors the scene's output resolution.
			// As of scene format v6 (Phase B2), camera chunks no longer
			// accept `width` / `height` / `pixelAR`; the `film` chunk
			// is the sole authoring surface for raster dims.  The CLI
			// flags --width / --height / --pixel-ar override whatever
			// the scene's film chunk authored (and override the qHD
			// default when neither is present).  Position within the
			// scene file is unimportant: Job::SetFilm walks the camera
			// manager and resyncs every camera's pixelAR + dim cache
			// on each call, so a late `film` chunk after several
			// cameras still wins.
			//////////////////////////////////////////

			struct FilmAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					// Defaults pulled from Rendering/Film.h so a `film {}`
					// chunk produces the same Film as omitting the chunk
					// (Job::InitializeContainers uses the same constants).
					const unsigned int width   = bag.GetUInt(   "width",   kDefaultFilmWidth );
					const unsigned int height  = bag.GetUInt(   "height",  kDefaultFilmHeight );
					const double       pixelAR = bag.GetDouble( "pixelAR", kDefaultFilmPixelAR );
					return pJob.SetFilm( width, height, pixelAR );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword     = "film";
						cd.category    = ChunkCategory::Film;
						cd.description = "Pixel-grid descriptor.  Sets the rendered image's "
						                 "width, height, and pixel aspect ratio.  Defaults to "
						                 "qHD (960 x 540) with square pixels if absent.  CLI "
						                 "flags --width / --height / --pixel-ar override this.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "width";   p.kind = ValueKind::UInt;   p.description = "Image width (pixels)";  p.defaultValueHint = "960"; }
						{ auto& p = P(); p.name = "height";  p.kind = ValueKind::UInt;   p.description = "Image height (pixels)"; p.defaultValueHint = "540"; }
						{ auto& p = P(); p.name = "pixelAR"; p.kind = ValueKind::Double; p.description = "Pixel aspect ratio (1.0 = square)"; p.defaultValueHint = "1.0"; }
						return cd;
					}();
					return d;
				}
			};

			//////////////////////////////////////////
			// Geometries
			//////////////////////////////////////////

			struct SphereGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name",   "noname" );
					double      radius = bag.GetDouble( "radius", 1.0 );
					return pJob.AddSphereGeometry( name.c_str(), radius );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "sphere_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Implicit sphere geometry.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";   p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "radius"; p.kind = ValueKind::Double; p.description = "Sphere radius"; p.defaultValueHint = "1.0"; }
						return cd;
					}();
					return d;
				}
			};

			struct EllipsoidGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					double radii[3] = {1.0,1.0,1.0};
					bag.GetVec3( "radii", radii );
					return pJob.AddEllipsoidGeometry( name.c_str(), radii );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "ellipsoid_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Implicit ellipsoid (per-axis radii).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";  p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "radii"; p.kind = ValueKind::DoubleVec3; p.description = "Per-axis radii (semi-axes, like sphere_geometry radius)"; p.defaultValueHint = "1 1 1"; }
						return cd;
					}();
					return d;
				}
			};

			struct CylinderGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name",   "noname" );
					double radius    = bag.GetDouble( "radius", 1.0 );
					double height    = bag.GetDouble( "height", 1.0 );
					bool   capped    = bag.GetBool( "capped", true );
					std::string axisStr = bag.GetString( "axis", "x" );
					char axis        = axisStr.empty() ? 'x' : axisStr[0];
					return pJob.AddCylinderGeometry( name.c_str(), axis, radius, height, capped );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "cylinder_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Implicit cylinder.  Default is a CLOSED SOLID (two end-cap disks) so an interior_medium bound to it is actually entered; set capped FALSE for a legacy open-ended tube (a camera ray along the axis then crosses no surface).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";   p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "axis";   p.kind = ValueKind::Enum;   p.enumValues = {"x","y","z"}; p.description = "Cylinder axis"; p.defaultValueHint = "x"; }
						{ auto& p = P(); p.name = "radius"; p.kind = ValueKind::Double; p.description = "Cylinder radius"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "height"; p.kind = ValueKind::Double; p.description = "Cylinder height"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "capped"; p.kind = ValueKind::Bool;   p.description = "TRUE: closed solid with end-cap disks (enterable by an interior_medium); FALSE: open-ended tube (side wall only)"; p.defaultValueHint = "TRUE"; }
						return cd;
					}();
					return d;
				}
			};

			struct TorusGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name    = bag.GetString( "name",        "noname" );
					double majorradius  = bag.GetDouble( "majorradius", 1.0 );
					double minorratio   = bag.GetDouble( "minorratio",  0.3 );
					return pJob.AddTorusGeometry( name.c_str(), majorradius, minorratio*majorradius );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "torus_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Implicit torus.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "majorradius"; p.kind = ValueKind::Double; p.description = "Major (tube-centre) radius"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "minorratio";  p.kind = ValueKind::Double; p.description = "Minor (tube) radius as a fraction of majorradius"; p.defaultValueHint = "0.3"; }
						return cd;
					}();
					return d;
				}
			};

			struct InfinitePlaneGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name",  "noname" );
					double xtile     = bag.GetDouble( "xtile", 1.0 );
					double ytile     = bag.GetDouble( "ytile", 1.0 );
					return pJob.AddInfinitePlaneGeometry( name.c_str(), xtile, ytile );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "infiniteplane_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Infinite tiling plane.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";  p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "xtile"; p.kind = ValueKind::Double; p.description = "Tile size in X"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "ytile"; p.kind = ValueKind::Double; p.description = "Tile size in Y"; p.defaultValueHint = "1.0"; }
						return cd;
					}();
					return d;
				}
			};

			struct BoxGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name",   "noname" );
					double width     = bag.GetDouble( "width",  1.0 );
					double height    = bag.GetDouble( "height", 1.0 );
					double depth     = bag.GetDouble( "depth",  1.0 );
					return pJob.AddBoxGeometry( name.c_str(), width, height, depth );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "box_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Axis-aligned box.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";   p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "width";  p.kind = ValueKind::Double; p.description = "X extent"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "height"; p.kind = ValueKind::Double; p.description = "Y extent"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "depth";  p.kind = ValueKind::Double; p.description = "Z extent"; p.defaultValueHint = "1.0"; }
						return cd;
					}();
					return d;
				}
			};

			struct ClippedPlaneGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					double pta[3] = {0,0,0};
					double ptb[3] = {0,0,0};
					double ptc[3] = {0,0,0};
					double ptd[3] = {0,0,0};
					bag.GetVec3( "pta", pta );
					bag.GetVec3( "ptb", ptb );
					bag.GetVec3( "ptc", ptc );
					bag.GetVec3( "ptd", ptd );
					bool doublesided = bag.GetBool( "doublesided", true );
					return pJob.AddClippedPlaneGeometry( name.c_str(), pta, ptb, ptc, ptd, doublesided );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "clippedplane_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Quad patch defined by four corner points.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "pta";         p.kind = ValueKind::DoubleVec3; p.description = "Corner A"; }
						{ auto& p = P(); p.name = "ptb";         p.kind = ValueKind::DoubleVec3; p.description = "Corner B"; }
						{ auto& p = P(); p.name = "ptc";         p.kind = ValueKind::DoubleVec3; p.description = "Corner C"; }
						{ auto& p = P(); p.name = "ptd";         p.kind = ValueKind::DoubleVec3; p.description = "Corner D"; }
						{ auto& p = P(); p.name = "doublesided"; p.kind = ValueKind::Bool;       p.description = "Rendered on both sides"; p.defaultValueHint = "TRUE"; }
						return cd;
					}();
					return d;
				}
			};

			struct Mesh3DSGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name",        "noname" );
					std::string file = bag.GetString( "file",        "none" );
					bool double_sided = bag.GetBool( "double_sided", false );
					bool face_normals = bag.GetBool( "face_normals", false );
					// Tier A2 cleanup (2026-04-27): `maxpolygons`, `maxdepth`,
					// and `bsp` are accepted but ignored — BVH is the sole
					// acceleration structure now and has no user-tunable
					// build parameters from scene files.
					return pJob.Add3DSTriangleMeshGeometry( name.c_str(), file.c_str(), double_sided, face_normals );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "3dsmesh_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Triangle mesh loaded from a 3D Studio .3ds file.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";         p.kind = ValueKind::String;   p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "file";         p.kind = ValueKind::Filename; p.description = "Source .3ds file"; }
						{ auto& p = P(); p.name = "double_sided"; p.kind = ValueKind::Bool;     p.description = "Render both sides"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "face_normals"; p.kind = ValueKind::Bool;     p.description = "Use flat per-face normals"; p.defaultValueHint = "FALSE"; }
						// Retired: accepted for backward compat with pre-A2 scene files; ignored.
						{ auto& p = P(); p.name = "maxpolygons";  p.kind = ValueKind::UInt;     p.description = "Retired (BVH is sole accelerator)"; }
						{ auto& p = P(); p.name = "maxdepth";     p.kind = ValueKind::UInt;     p.description = "Retired (BVH is sole accelerator)"; }
						{ auto& p = P(); p.name = "bsp";          p.kind = ValueKind::Bool;     p.description = "Retired (BVH is sole accelerator)"; }
						return cd;
					}();
					return d;
				}
			};

			struct RAWMeshGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name",        "noname" );
					std::string file = bag.GetString( "file",        "none" );
					bool double_sided = bag.GetBool( "double_sided", false );
					// Legacy maxpolygons/maxdepth/bsp keys accepted but ignored (Tier A2).
					return pJob.AddRAWTriangleMeshGeometry( name.c_str(), file.c_str(), double_sided );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "rawmesh_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Triangle mesh loaded from a RAW vertex file.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";         p.kind = ValueKind::String;   p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "file";         p.kind = ValueKind::Filename; p.description = "Source RAW file"; }
						{ auto& p = P(); p.name = "double_sided"; p.kind = ValueKind::Bool;     p.description = "Render both sides"; p.defaultValueHint = "FALSE"; }
						// Retired: accepted for backward compat with pre-A2 scene files; ignored.
						{ auto& p = P(); p.name = "maxpolygons";  p.kind = ValueKind::UInt;     p.description = "Retired (BVH is sole accelerator)"; }
						{ auto& p = P(); p.name = "maxdepth";     p.kind = ValueKind::UInt;     p.description = "Retired (BVH is sole accelerator)"; }
						{ auto& p = P(); p.name = "bsp";          p.kind = ValueKind::Bool;     p.description = "Retired (BVH is sole accelerator)"; }
						return cd;
					}();
					return d;
				}
			};

			struct RAWMesh2GeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name",        "noname" );
					std::string file = bag.GetString( "file",        "none" );
					bool double_sided = bag.GetBool( "double_sided", false );
					bool face_normals = bag.GetBool( "face_normals", false );
					// Legacy maxpolygons/maxdepth/bsp keys accepted but ignored (Tier A2).
					return pJob.AddRAW2TriangleMeshGeometry( name.c_str(), file.c_str(), double_sided, face_normals );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "rawmesh2_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Triangle mesh loaded from a RAW v2 file.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";         p.kind = ValueKind::String;   p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "file";         p.kind = ValueKind::Filename; p.description = "Source RAW2 file"; }
						{ auto& p = P(); p.name = "double_sided"; p.kind = ValueKind::Bool;     p.description = "Render both sides"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "face_normals"; p.kind = ValueKind::Bool;     p.description = "Flat per-face normals"; p.defaultValueHint = "FALSE"; }
						// Retired: accepted for backward compat with pre-A2 scene files; ignored.
						{ auto& p = P(); p.name = "maxpolygons";  p.kind = ValueKind::UInt;     p.description = "Retired (BVH is sole accelerator)"; }
						{ auto& p = P(); p.name = "maxdepth";     p.kind = ValueKind::UInt;     p.description = "Retired (BVH is sole accelerator)"; }
						{ auto& p = P(); p.name = "bsp";          p.kind = ValueKind::Bool;     p.description = "Retired (BVH is sole accelerator)"; }
						return cd;
					}();
					return d;
				}
			};

			struct RISEMeshGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name",           "noname" );
					std::string file = bag.GetString( "file",           "none" );
					bool loadintomem = bag.GetBool(   "loadintomemory", true );
					bool face_normals= bag.GetBool(   "face_normals",   false );
					return pJob.AddRISEMeshTriangleMeshGeometry( name.c_str(), file.c_str(), loadintomem, face_normals );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "risemesh_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Triangle mesh loaded from a RISE-native .risemesh file.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";           p.kind = ValueKind::String;   p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "file";           p.kind = ValueKind::Filename; p.description = "Source .risemesh file"; }
						{ auto& p = P(); p.name = "loadintomemory"; p.kind = ValueKind::Bool;     p.description = "Load entire mesh into memory"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "face_normals";   p.kind = ValueKind::Bool;     p.description = "Flat per-face normals"; p.defaultValueHint = "FALSE"; }
						return cd;
					}();
					return d;
				}
			};

			struct PLYMeshGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name        = bag.GetString( "name",         "noname" );
					std::string file        = bag.GetString( "file",         "none" );
					bool double_sided       = bag.GetBool(   "double_sided", false );
					bool invert_faces       = bag.GetBool(   "invert_faces", false );
					bool face_normals       = bag.GetBool(   "face_normals", false );
					return pJob.AddPLYTriangleMeshGeometry( name.c_str(), file.c_str(), double_sided, invert_faces, face_normals );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "plymesh_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Triangle mesh loaded from a Stanford PLY file.  "
							"Per-vertex colors (when present in the PLY) are read into the "
							"mesh and exposed at hit time via `vertex_color_painter`.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";         p.kind = ValueKind::String;   p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "file";         p.kind = ValueKind::Filename; p.description = "Source .ply file"; }
						{ auto& p = P(); p.name = "double_sided"; p.kind = ValueKind::Bool;     p.description = "Treat polygons as double sided"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "invert_faces"; p.kind = ValueKind::Bool;     p.description = "Reverse face winding";          p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "face_normals"; p.kind = ValueKind::Bool;     p.description = "Flat per-face normals";         p.defaultValueHint = "FALSE"; }
						return cd;
					}();
					return d;
				}
			};

			struct GLTFImportAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string file              = bag.GetString( "file",                "none" );
					std::string name_prefix       = bag.GetString( "name_prefix",         "gltf" );
					// `scene_index` defaults to UINT_MAX (sentinel: "use the
					// file's default scene") when omitted, so callers don't
					// inadvertently override an asset's `"scene":` field.
					// Explicit values map to the corresponding scenes[]
					// array index.
					unsigned int scene_index      = bag.Has( "scene_index" ) ?
					                                  bag.GetUInt( "scene_index", 0u ) :
					                                  UINT_MAX;
					bool import_meshes            = bag.GetBool(   "import_meshes",       true );
					bool import_materials         = bag.GetBool(   "import_materials",    true );
					bool import_lights            = bag.GetBool(   "import_lights",       true );
					bool import_cameras           = bag.GetBool(   "import_cameras",      true );
					bool import_normal_maps       = bag.GetBool(   "import_normal_maps",  true );
					bool lowmem_textures          = bag.GetBool(   "lowmem_textures",     false );
					double lights_intensity_override       = bag.GetDouble( "lights_intensity_override",        0.0 );
					double directional_intensity_override  = bag.GetDouble( "directional_intensity_override",   0.0 );
					double point_intensity_override        = bag.GetDouble( "point_intensity_override",         0.0 );
					double spot_intensity_override         = bag.GetDouble( "spot_intensity_override",          0.0 );
					bool   respect_baked_occlusion         = bag.GetBool(   "respect_baked_occlusion",          true );
					double emissive_intensity_scale        = bag.GetDouble( "emissive_intensity_scale",         1.0 );
					double emissive_tint[3] = { 1.0, 1.0, 1.0 };
					bag.GetVec3( "emissive_tint", emissive_tint );
					if( lights_intensity_override > 0.0 ) {
						GlobalLog()->PrintEx( eLog_Warning,
							"gltf_import:: `lights_intensity_override` is unit-blind (it conflates lux for "
							"directional lights with candela for point/spot, which are different physical "
							"quantities).  Prefer `directional_intensity_override` (lux), "
							"`point_intensity_override` (candela), and `spot_intensity_override` (candela) -- "
							"they respect glTF's per-type units.  Per-type values override this when set." );
					}
					return pJob.ImportGLTFScene(
						file.c_str(), name_prefix.c_str(),
						scene_index,
						import_meshes, import_materials,
						import_lights, import_cameras,
						import_normal_maps,
						lowmem_textures,
						lights_intensity_override,
						directional_intensity_override,
						point_intensity_override,
						spot_intensity_override,
						respect_baked_occlusion,
						emissive_intensity_scale,
						emissive_tint[0],
						emissive_tint[1],
						emissive_tint[2] );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "gltf_import"; cd.category = ChunkCategory::Geometry;
						// APPEND-class derive: Finalize -> Job::ImportGLTFScene walks the file's scene tree and
						// APPENDS one independent set of geometry/material/object/light/camera entities per call
						// (like timeline/keyframe's per-element appends, NOT a last-wins single-slot chunk) -- a
						// scene legitimately carries MULTIPLE unnamed `gltf_import` chunks, one per asset, as long
						// as each uses a distinct `name_prefix` (scenes/FeatureBased/Geometry/sponza_new_ivy.RISEscene
						// carries 2+ with an in-file comment blessing the idiom).  This flag was DEFERRED in
						// 436f604a pending a load-bearing fix: at the time, GLTFSceneImporter::ImportScene
						// unconditionally `return true`d and silently swallowed every duplicate-name
						// GenericManager::AddItem failure, so TWO unnamed imports sharing the SAME default
						// name_prefix would pass the dry-run derive with the second import's entities silently
						// masked.  That is now fixed: Job::ImportGLTFScene refuses a REPEATED name_prefix within
						// the same derive up front (a per-Job `mGltfImportPrefixes` record), and ImportScene itself
						// now propagates every entity-registration failure (material / geometry / object) as a
						// hard `false` instead of discarding it -- prefix collisions (the common case) and stray
						// entity-name collisions (the rare hand-authored case) both now fail the derive loudly.
						cd.unnamedRepeatable = true;
						cd.description = "Bulk-import of a glTF 2.0 (.gltf or .glb) scene.  Walks the "
							"scene tree and registers per-primitive standard_objects, per-material "
							"pbr_metallic_roughness materials (Schlick-from-F0 PBR with optional "
							"normal_map_modifier; per-material alpha-test or transparency shader op "
							"for alphaMode = MASK / BLEND with per-pixel alpha read straight from "
							"the baseColor texture's A channel), painters for each texture (embedded "
							"`.glb` images decode in-memory; external URIs read from disk -- no "
							"sidecar cache), lights from KHR_lights_punctual, and the first camera.  "
							"Node transforms flow through `Job::AddObjectMatrix` verbatim (lossless "
							"4x4, no Euler decomposition).  Object names are prefixed with "
							"`name_prefix` to keep manager namespaces clean.  Phase 4 also wires "
							"KHR_materials_emissive_strength, KHR_materials_unlit (via "
							"LambertianLuminaireMaterial), and the scalar subset of "
							"KHR_materials_transmission + volume + ior (refractive glass with Beer-"
							"Lambert absorption).  Out of scope: animations, skinning, morph targets "
							"(warn-and-skip), KHR_materials_clearcoat / sheen as a layer over PBR "
							"(warn-and-skip; standalone sheen_material chunk works), "
							"transmission_texture (per-pixel τ; warn), Draco / meshopt compression, "
							"other KHR_materials_* (specular, anisotropy, iridescence, dispersion).  "
							"alphaMode = MASK / BLEND are honoured only by integrators routing "
							"through IShader::Shade() (PT and the legacy direct shaders) -- BDPT, "
							"VCM, MLT, photon tracers bypass shader ops and treat both as opaque.  "
							"See docs/GLTF_IMPORT.md §7, §13, §15 for full status.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "file";               p.kind = ValueKind::Filename; p.description = "Source .gltf or .glb file"; }
						{ auto& p = P(); p.name = "name_prefix";        p.kind = ValueKind::String;   p.description = "Prefix for created geometry / material / object / light / camera names.  MUST be unique across every `gltf_import` in the scene (including two chunks that both omit it and fall back to the same default) -- a repeated prefix is REFUSED at derive time (\"name_prefix '...' collides with an existing import\") rather than silently masking one import's entities."; p.defaultValueHint = "gltf"; }
						{ auto& p = P(); p.name = "scene_index";        p.kind = ValueKind::UInt;     p.description = "Index into the file's scenes[] array.  Omit (or use UINT_MAX) to import the file's default scene (the `scene` field in the glTF JSON), which is what most authoring tools intend; explicit values force a particular array index."; p.defaultValueHint = "(default scene)"; }
						{ auto& p = P(); p.name = "import_meshes";      p.kind = ValueKind::Bool;     p.description = "Create per-primitive standard_objects"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "import_materials";   p.kind = ValueKind::Bool;     p.description = "Create one PBR material per glTF material"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "import_lights";      p.kind = ValueKind::Bool;     p.description = "Create lights from KHR_lights_punctual"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "import_cameras";     p.kind = ValueKind::Bool;     p.description = "Create the first camera (subsequent ones warn)"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "import_normal_maps"; p.kind = ValueKind::Bool;     p.description = "Attach normal_map_modifier when material has normalTexture"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "lowmem_textures";   p.kind = ValueKind::Bool;     p.description = "Defer texture color-space conversion to per-sample access.  Saves ~4x peak texture memory and 5-10x faster scene load on heavy-PBR assets (NewSponza-class), at the cost of ~25% per-sample render time (the sRGB->linear convert moves from load-time to ray-hit time).  Default FALSE (final-render workflow); flip TRUE for iteration on big scenes."; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "lights_intensity_override"; p.kind = ValueKind::Double; p.description = "DEPRECATED (Landing 4): unit-blind override that replaces zero authored intensities for ALL light types uniformly when > 0.  glTF assigns DIFFERENT physical units per type (lux for directional, candela for point/spot per KHR_lights_punctual) so a single number is physically meaningless across types.  Kept one release for back-compat (NewSponza-style scenes).  Prefer `directional_intensity_override`, `point_intensity_override`, `spot_intensity_override` below.  Per-type values override this when set."; p.defaultValueHint = "0 (no override)"; }
						{ auto& p = P(); p.name = "directional_intensity_override"; p.kind = ValueKind::Double; p.description = "Per-type override for KHR_lights_punctual directional lights.  Units: LUX (lm/m²) -- glTF's authored unit for directional intensity.  Replaces zero authored intensities for directional lights only (lights set non-zero stay untouched).  Typical values: ~120000 for noon clear-sky sun, ~10000 for overcast day, ~100 for moonlight."; p.defaultValueHint = "0 (no override)"; }
						{ auto& p = P(); p.name = "point_intensity_override";       p.kind = ValueKind::Double; p.description = "Per-type override for KHR_lights_punctual point lights.  Units: CANDELA (lm/sr) -- glTF's authored unit for point intensity.  Replaces zero authored intensities for point lights only.  Typical values: ~100 for a 60-W incandescent (~800 lm omnidirectional / 4 pi sr), ~1500 for a 100-W LED bulb."; p.defaultValueHint = "0 (no override)"; }
						{ auto& p = P(); p.name = "spot_intensity_override";        p.kind = ValueKind::Double; p.description = "Per-type override for KHR_lights_punctual spot lights.  Units: CANDELA (lm/sr) along the spot's central axis -- glTF's authored unit for spot intensity.  Replaces zero authored intensities for spot lights only."; p.defaultValueHint = "0 (no override)"; }
						{ auto& p = P(); p.name = "respect_baked_occlusion";       p.kind = ValueKind::Bool;   p.description = "Landing 13: when TRUE (default), import glTF `occlusionTexture` as a multiplier on the material's diffuse baseColor (× R-channel × occlusionStrength).  Recovers high-frequency baked AO that geometry can't reach (column flutes, brick mortar, fabric folds) but slightly double-counts the path tracer's own occlusion on direct light.  Set FALSE for strict-PB workflows where you want only the integrator's computed occlusion."; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "emissive_intensity_scale";      p.kind = ValueKind::Double; p.description = "Multiplier applied AFTER each material's authored `KHR_materials_emissive_strength` (or default 1.0).  Default 1.0 (no change).  Use to brighten ALL emissive surfaces in the import uniformly without editing the asset (e.g. a deep-dusk candle scene whose flame meshes are authored at daytime-balanced strength can multiply by 50-200 to make the candles dominate).  Unlike `lights_intensity_override` this is a SCALE (composes with authored values) -- emissive materials typically ship with meaningful chromatic / relative values whose ratios should be preserved.  Values <= 0 kill all emissive in the import.  Folded in once at import time, no per-sample cost."; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "emissive_tint";                 p.kind = ValueKind::DoubleVec3; p.description = "Per-channel R G B multiplier applied componentwise to every material's `emissiveFactor`.  Default (1, 1, 1) -- no tint.  Use to recolour emissive surfaces uniformly across the import without editing the asset, e.g. tint a pure-yellow flame `(1, 1, 0)` to warm orange via `emissive_tint 1.0 0.5 0.1` (final emissive becomes `(1, 0.5, 0)`).  Composes with `emissive_intensity_scale` (independent brightness vs chroma knobs).  Multiplies the FACTOR not the painted texture, so an authored 0.0 channel stays 0.0 (the tint can attenuate channels but cannot add a colour the asset never authored)."; p.defaultValueHint = "1 1 1"; }
						return cd;
					}();
					return d;
				}
			};

			struct GLTFMeshGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name           = bag.GetString( "name",         "noname" );
					std::string file           = bag.GetString( "file",         "none" );
					unsigned int mesh_idx      = bag.GetUInt(   "mesh_index",    0 );
					unsigned int primitive_idx = bag.GetUInt(   "primitive",     0 );
					bool double_sided          = bag.GetBool(   "double_sided",  false );
					bool face_normals          = bag.GetBool(   "face_normals",  false );
					// Default flip_v to FALSE.  glTF 2.0 spec says V=0 is at
					// the upper-left of the texture; PNG/JPEG decoders store
					// row 0 at the top and RISE's BilinRasterImageAccessor
					// passes V verbatim into the vertical-pixel index, so
					// glTF V=0 already lands at the correct row without a
					// flip.  Verified end-to-end via Avocado.glb (clean
					// brown pit + yellow flesh + green skin) at commit
					// 1c62acb.
					bool flip_v                = bag.GetBool(   "flip_v",        false );
					return pJob.AddGLTFTriangleMeshGeometry(
						name.c_str(), file.c_str(),
						mesh_idx, primitive_idx,
						double_sided, face_normals, flip_v );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "gltfmesh_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Triangle mesh loaded from a glTF 2.0 (.gltf or .glb) file.  "
							"Imports a single primitive of a single mesh -- materials, scene "
							"structure, lights, cameras, and animations are NOT imported by this "
							"chunk (Phase 1 of glTF support; bulk scene import comes in Phase 2 "
							"via `gltf_import`).  See docs/GLTF_IMPORT.md for the design plan.  "
							"POSITION + NORMAL + TANGENT + TEXCOORD_0 + TEXCOORD_1 + COLOR_0 + "
							"indices are honoured; other attributes warn-and-discard.  "
							"`flip_v` defaults to FALSE because the glTF 2.0 spec puts the UV "
							"origin at the upper-left of the texture (V increases downward), "
							"matching RISE's V-down sampling.  Override TRUE only for non-"
							"conformant assets that ship with V already baked in upside-down.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";         p.kind = ValueKind::String;   p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "file";         p.kind = ValueKind::Filename; p.description = "Source .gltf or .glb file"; }
						{ auto& p = P(); p.name = "mesh_index";   p.kind = ValueKind::UInt;     p.description = "Which mesh in the file (0-based)"; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "primitive";    p.kind = ValueKind::UInt;     p.description = "Which primitive within the mesh (0-based)"; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "double_sided"; p.kind = ValueKind::Bool;     p.description = "Treat polygons as double sided"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "face_normals"; p.kind = ValueKind::Bool;     p.description = "Flat per-face normals"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "flip_v";       p.kind = ValueKind::Bool;     p.description = "Flip TEXCOORD V at load (defaults FALSE — glTF UV origin is upper-left, matches RISE's V-down sampling already)"; p.defaultValueHint = "FALSE"; }
						return cd;
					}();
					return d;
				}
			};

			struct CircularDiskGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name",   "noname" );
					double radius    = bag.GetDouble( "radius", 1.0 );
					std::string axisStr = bag.GetString( "axis", "x" );
					char axis        = axisStr.empty() ? 'x' : axisStr[0];
					return pJob.AddCircularDiskGeometry( name.c_str(), radius, axis );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "circulardisk_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Flat circular disk.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";   p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "radius"; p.kind = ValueKind::Double; p.description = "Disk radius"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "axis";   p.kind = ValueKind::Enum;   p.enumValues = {"x","y","z"}; p.description = "Normal axis"; p.defaultValueHint = "x"; }
						return cd;
					}();
					return d;
				}
			};

			// bezierpatch_geometry is always analytic now.  The chunk accepts
			// ONLY the minimal set of parameters that control the patch file
			// and the patch-level acceleration structure.  Every parameter
			// that relates to tessellation (detail, face_normals, double_sided,
			// maxpolygons, maxpolydepth/poly_bsp, cache_size) or to the binary
			// analytic/mesh switch (analytic) or to displacement (displacement,
			// disp_scale) lives on `displaced_geometry` or is gone entirely.
			// Any such parameter found here is rejected with a clear message
			// so out-of-date scenes get an actionable error instead of silent
			// behaviour drift.
			struct BezierPatchGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					// Reject legacy parameters that have been retired.  The
					// descriptor lists them so the dispatcher accepts them
					// (instead of failing with a generic "unknown parameter"
					// message), and Finalize emits the actionable error.
					static const char* const kRetired[] = {
						"analytic", "cache_size", "detail", "face_normals",
						"double_sided", "poly_bsp", "maxpolygons", "maxpolydepth",
						"displacement", "disp_scale"
					};
					for( const char* r : kRetired ) {
						if( bag.Has( r ) ) {
							GlobalLog()->PrintEx( eLog_Error,
								"bezierpatch_geometry: parameter `%s` is no longer "
								"accepted.  Rendering is always analytic; for a "
								"tessellated or displaced mesh wrap the geometry in "
								"a displaced_geometry chunk and set detail/"
								"face_normals/double_sided/displacement/disp_scale "
								"there.",
								r );
							return false;
						}
					}

					std::string name = bag.GetString( "name",          "noname" );
					std::string file = bag.GetString( "file",          "none" );
					unsigned int maxPatches = bag.GetUInt( "maxpatches",    2 );
					unsigned int maxRecur   = bag.GetUInt( "maxdepth",      8 );
					bool bsp                = bag.GetBool( "bsp",           false );
					bool center_object      = bag.GetBool( "center_object", false );

					return pJob.AddBezierPatchGeometry( name.c_str(), file.c_str(), maxPatches, maxRecur, bsp, center_object );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "bezierpatch_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Bézier patch surface from file.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";          p.kind = ValueKind::String;   p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "file";          p.kind = ValueKind::Filename; p.description = "Patch file"; }
						{ auto& p = P(); p.name = "maxpatches";    p.kind = ValueKind::UInt;     p.description = "Max patches per BSP leaf"; p.defaultValueHint = "2"; }
						{ auto& p = P(); p.name = "maxdepth";      p.kind = ValueKind::UInt;     p.description = "Max BSP depth"; p.defaultValueHint = "8"; }
						{ auto& p = P(); p.name = "bsp";           p.kind = ValueKind::Bool;     p.description = "Build BSP"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "center_object"; p.kind = ValueKind::Bool;     p.description = "Auto-center the mesh"; p.defaultValueHint = "FALSE"; }
						// Retired parameters — accepted by the descriptor so we can
						// emit a specific error in Finalize, then rejected.
						{ auto& p = P(); p.name = "analytic";      p.kind = ValueKind::String;   p.description = "Retired — rendering is always analytic"; }
						{ auto& p = P(); p.name = "cache_size";    p.kind = ValueKind::UInt;     p.description = "Retired — wrap in displaced_geometry"; }
						{ auto& p = P(); p.name = "detail";        p.kind = ValueKind::UInt;     p.description = "Retired — wrap in displaced_geometry"; }
						{ auto& p = P(); p.name = "face_normals";  p.kind = ValueKind::Bool;     p.description = "Retired — wrap in displaced_geometry"; }
						{ auto& p = P(); p.name = "double_sided";  p.kind = ValueKind::Bool;     p.description = "Retired — wrap in displaced_geometry"; }
						{ auto& p = P(); p.name = "poly_bsp";      p.kind = ValueKind::Bool;     p.description = "Retired — wrap in displaced_geometry"; }
						{ auto& p = P(); p.name = "maxpolygons";   p.kind = ValueKind::UInt;     p.description = "Retired — wrap in displaced_geometry"; }
						{ auto& p = P(); p.name = "maxpolydepth";  p.kind = ValueKind::UInt;     p.description = "Retired — wrap in displaced_geometry"; }
						{ auto& p = P(); p.name = "displacement";  p.kind = ValueKind::String;   p.description = "Retired — wrap in displaced_geometry"; }
						{ auto& p = P(); p.name = "disp_scale";    p.kind = ValueKind::Double;   p.description = "Retired — wrap in displaced_geometry"; }
						return cd;
					}();
					return d;
				}
			};

			struct SDFGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					std::string file = bag.GetString( "file", "" );
					unsigned int maxSteps = bag.GetUInt( "maxsteps", 256 );
					double eps = bag.GetDouble( "epsilon", 0.0 );
					unsigned int samplingDetail = bag.GetUInt( "sampling_detail", 64 );

					// Heightfield mode (analytic exact-surface twin of a displaced
					// cartesian disk): when a heightfield_function is named, the SDF
					// IS the surface z = heightfield_scale * f(u,v), clipped to a DISK
					// of radius R centred at the origin in the local XY plane (matching
					// cartesian_disk_geometry's domain, not a square) -- sphere-traced,
					// O(1) memory -- and the `part` / `file` path is bypassed entirely.
					std::string hfFunction = bag.GetString( "heightfield_function", "none" );
					double hfRadius = bag.GetDouble( "heightfield_radius", 1.0 );
					double hfScale  = bag.GetDouble( "heightfield_scale", 0.0 );
					if( hfFunction != "none" ) {
						return pJob.AddSDFHeightfieldGeometry( name.c_str(), hfFunction.c_str(), hfRadius, hfScale, maxSteps, eps, samplingDetail );
					}

					// Inline `part` lines (repeatable, in authoring order) are
					// joined into the newline-separated source that
					// SDFGeometry::ParsePartLines understands.  The lines are
					// forwarded VERBATIM -- future part-grammar extensions
					// (new shapes / ops / per-part fields) need no change
					// here.  Exactly-one-of-{`part` lines, `file`} is
					// diagnosed by Job::AddSDFGeometry with the chunk name.
					const std::vector<std::string>& partLines = bag.GetRepeatable( "part" );
					std::string parts;
					for( std::size_t i = 0; i < partLines.size(); ++i ) {
						parts += partLines[i];
						parts += "\n";
					}

					return pJob.AddSDFGeometry( name.c_str(), file.c_str(), parts.c_str(), maxSteps, eps, samplingDetail );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "sdf_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Signed-distance-field (implicit) surface: transformed primitives composed with smooth-min / boolean ops, sphere-traced.  For melded / filleted organic shapes (e.g. lugs flowing into a bezel).  Author the parts inline with repeatable `part` lines; use `file` only for very large SDFs.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";     p.kind = ValueKind::String;   p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "part";     p.kind = ValueKind::String;   p.repeatable = true; p.description = "One SDF part (repeatable; parts compose in order): <prim> <op> <k>  <px py pz>  <exDeg eyDeg ezDeg>  <sx sy sz>  <a b c>  <round>.  prim: sphere|box|roundbox|cylinder|torus|capsule|roundcone|superellipsoid; op: union|smin|subtract|intersect (k = blend radius, 0 = hard).  k IS A WORLD-UNIT RADIUS, not a fraction of the parts: a k comparable to the smallest part in the join DISSOLVES that part into its neighbour, so a small feature meeting a large mass -- a finial on a post, a handle on a pot -- wants k at about a third of that small part's radius or less.  PER-PRIMITIVE <a b c> (verified against SDFGeometry.cpp's primDist dispatch): sphere -- a = radius (b, c unused).  box -- a/b/c = HALF-EXTENTS along local x/y/z (full size is 2a x 2b x 2c; `round` unused).  roundbox -- a/b/c = the CORE box's half-extents before rounding; `round` shrinks them and re-inflates the corners, so the part still spans a/b/c overall but with radius-`round` corners.  cylinder -- a = radius, b = HALF-HEIGHT along local Y (c unused).  torus -- a = ring (major) radius in the local XZ plane, b = tube (minor) radius (c unused).  capsule -- a = radius, b = HALF-LENGTH of the core segment along local Y (c unused).  roundcone -- a = base radius at local y=0, b = tip radius at local y=h, c = h, the cone's own length along local Y.  superellipsoid is the CONTINUUM primitive: a = radius, b = e1 (north-south exponent), c = e2 (east-west), `round` unused, ellipsoidal proportions from <sx sy sz> -- b=c=1 an ellipsoid, both -> 0 a box, b -> 0 with c = 1 a cylinder, b=c=2 an octahedron, 0.4-0.7 the cushion/torso range; both exponents clamped to [0.1, 2].  <sx sy sz> IS A PRE-TRANSFORM SCALE, not the shape's size -- leave it 1 1 1 unless you deliberately mean to stretch the primitive after its own <a b c> already defines it.  A RECURRING AUTHORING SLIP: a box's dimensions go in <a b c>, NOT the scale slot -- putting a box's half-extents into <sx sy sz> while leaving <a b c> at 0 0 0 collapses that part to a single point (ParsePartLines warns when this happens, but does not reject the line).  Lines are forwarded verbatim to the shared SDF part grammar (SDFGeometry::ParsePartLines), so future shapes / ops extend without chunk changes.  The FIRST part must be union or smin (the field starts empty)"; }
						{ auto& p = P(); p.name = "file";     p.kind = ValueKind::Filename; p.description = "External SDF parts file (same one-part-per-line grammar; for very large SDFs).  Provide either inline `part` lines or `file`, not both"; }
						{ auto& p = P(); p.name = "maxsteps"; p.kind = ValueKind::UInt;     p.description = "Sphere-trace step cap"; p.defaultValueHint = "256"; }
						{ auto& p = P(); p.name = "epsilon";  p.kind = ValueKind::Double;   p.description = "Surface hit epsilon as a fraction of the bbox diagonal (0 = auto)"; p.defaultValueHint = "0.0"; }
						{ auto& p = P(); p.name = "sampling_detail"; p.kind = ValueKind::UInt; p.description = "Tessellation cells along the longest bbox axis for area-light / SSS surface sampling (clamped 8..256)"; p.defaultValueHint = "64"; }
						{ auto& p = P(); p.name = "heightfield_function"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter, ChunkCategory::Function}; p.description = "HEIGHTFIELD MODE: a named IFunction2D giving f(u,v) in [0,1].  When set (not `none`), the SDF IS the exact analytic surface z = heightfield_scale*f(u,v), clipped to a DISK of radius heightfield_radius centred at the origin in the local XY plane -- not a square -- via u=(x+R)/2R, v=(y+R)/2R (both clamped to [0,1] at the rim); outside the disk the field is bounded by the vertical cylindrical rim wall (sphere-traced, O(1) memory -- the exact-geometry twin of a displaced_geometry on a cartesian_disk_geometry, which shares this circular domain); `part` / `file` are ignored.  Resolved via pFunc2DManager (Job::AddSDFHeightfieldGeometry), which holds `function`-category IFunction2D chunks (piecewise_linear_function2d) PLUS every dual-registered colour painter except expression_painter/scalar_painter -- declared Function2D-piped with {Painter, Function} since 2026-09-06 (was declared {Function} only, missing the colour-painter half of its real accepted set)"; p.defaultValueHint = "none"; p.semantics.pipe = ParameterPipe::Function2D; }
						{ auto& p = P(); p.name = "heightfield_radius"; p.kind = ValueKind::Double; p.description = "Heightfield mode: radius of the DISK domain the field is clipped to, centred at the origin in the local XY plane (object units; u=(x+R)/2R, v=(y+R)/2R)"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "heightfield_scale"; p.kind = ValueKind::Double; p.description = "Heightfield mode: world amplitude of the field (surface z = heightfield_scale*f(u,v))"; p.defaultValueHint = "0.0"; }
						return cd;
					}();
					return d;
				}
			};

			//! `skeleton_geometry` (C1, docs/agentic-redesign/85-geometry-
			//! expressiveness-candidates.md) -- a JOINT GRAPH that expands, at
			//! PARSE TIME, into a single `sdf_geometry`: one `roundcone` part
			//! per bone (parent -> child), joined with `smin`, registered
			//! under THIS chunk's own `name` -- so CST incremental apply/
			//! remove works generically via RemoveGeometry(name), exactly like
			//! a hand-authored sdf_geometry, and unlike the `__geo`-suffix
			//! chunks (shape_light, rect_light) that need a Cst.cpp special
			//! case.  Creature flesh authored the way an animator thinks
			//! (joints and bones), not sdf_geometry's raw part-line grammar.
			//!
			//! A BONE'S END CAPS ARE ITS TWO JOINTS.  A roundcone's local +Y
			//! axis runs base -> tip, and RISE's SDF round cone
			//! (SDFGeometry.cpp's sdRoundConeY, the Quilez sphere-swept cone)
			//! includes hemispherical caps of radius `a` (base) and `b` (tip)
			//! at the two ends.  So a non-root joint needs NO sphere of its
			//! own -- the incoming bone's tip cap already sits exactly at its
			//! position with exactly its radius -- and a root WITH CHILDREN
			//! needs none either, for the mirror reason (each outgoing bone's
			//! base cap already covers it).  Only a joint that is BOTH
			//! parentless AND childless (an isolated joint) would otherwise be
			//! invisible, so that one case alone gets an explicit `sphere`
			//! part.  DO NOT "fix" this by adding a sphere at every joint --
			//! that would double-cover every bone-adjacent joint with a
			//! redundant, blend-interacting primitive nobody asked for.
			//!
			//! A BONE NEED NOT BE A PIPE: the optional 7th `joint` token,
			//! `aspect`, is the bone's cross-section WIDTH-to-DEPTH ratio (1 =
			//! the historical circular bone, and the ONLY value that existed
			//! before doc 90 slice B; a 6-token line still expands
			//! byte-for-byte identically).  Without it a skeleton could only
			//! ever be a chain of tubes -- one radius per joint is a CIRCLE by
			//! grammar -- which is exactly why creature limbs built this way
			//! read as plumbing.  A thigh is aspect ~0.6, a wing-arm 1.0.
			//!
			//! WHICH AXIS FLATTENS, stated so an author can predict it.
			//! `aspect` becomes the part's LOCAL X scale, `<aspect> 1 1`.  For
			//! any CLEARLY LEANING bone (more than ~0.6 degrees off vertical)
			//! the expansion solves for roll ez = 0, and the rotation's FIRST
			//! column (the world direction local +X maps to) is then
			//! (cos ey, 0, -sin ey) -- identically ZERO in world Y.  So the
			//! flattening axis is ALWAYS HORIZONTAL and always perpendicular to
			//! the bone: aspect narrows a bone SIDE-TO-SIDE in the ground
			//! plane, and the perpendicular it leaves at full `radius` is the
			//! one lying in the VERTICAL plane that contains the bone.  A bone
			//! leaning less than ~0.6 degrees off vertical (the kNearVerticalHoriz
			//! threshold in DirectionToEulerDeg) is TREATED AS VERTICAL and
			//! flattens along world X too, but via a different, endpoint-exact
			//! construction (that function's own comment has the derivation)
			//! rather than ey = 0 -- a bone right at the boundary therefore
			//! measures half as wide along world X as along world Z at aspect
			//! 0.5, same as a truly vertical one, and the tip cap still lands
			//! exactly on the declared child joint either side of the threshold.
			//! An ISOLATED joint's `sphere` part flattens along world X
			//! unconditionally too (no bone direction exists to derive an axis
			//! from, so world X is the only sensible canonical choice).  Pinned
			//! by SkeletonGeometryChunkTest's closed-form ellipse probe
			//! (clearly-leaning and exactly-vertical), its near-vertical
			//! endpoint/flatten-axis-stability probes, and its isolated-joint
			//! aspect probe -- not by this paragraph.
			//!
			//! PURE SQUASH, NOT AREA-PRESERVING.  `<aspect> 1 1` and not
			//! `<sqrt(aspect)> 1 <1/sqrt(aspect)>`, so that `radius` keeps
			//! meaning what it always meant: the bone's cross-section is
			//! `aspect * radius` across the flattened axis and EXACTLY `radius`
			//! across the other.  Aspect only ever REMOVES material -- an
			//! author who flattens a thigh never gets a thigh that bulges
			//! DEEPER than the radius typed on the line, which is what
			//! area-preserving would hand them (aspect 0.5 would swell the
			//! other axis by 1.41x).  Local Y is left at 1 for a harder reason
			//! than taste: the roundcone's `c` IS the bone length, so any Y
			//! scale would move the tip cap off the child joint's declared
			//! position.
			//!
			//! A JOINT'S ASPECT SHAPES THE BONE THAT ARRIVES AT IT (and, for an
			//! isolated joint, its own sphere).  There is no inheritance: a
			//! child does NOT pick up its parent's aspect, so a chain's
			//! cross-section varies per joint.  One `roundcone` part carries
			//! ONE scale, so a bone cannot taper from one aspect to another;
			//! the arriving-bone rule is the one that leaves the FEWEST tokens
			//! unused, since a tree has few roots and many leaves (the mirror
			//! rule, "shapes the bones LEAVING it", would silently ignore the
			//! aspect on every leaf).  A root WITH children has no arriving
			//! bone and emits no part of its own, so a non-1 aspect there is
			//! inert -- warned about, in the same non-rejecting style as the
			//! degenerate-bone diagnostic below, rather than dropped in
			//! silence.
			//!
			//! CONSERVATIVE UNDER SPHERE-TRACING, for free: SDFGeometry's
			//! partEval divides into local space by the per-axis scale and
			//! multiplies the primitive's distance back by `minScale` (the
			//! smallest |scale| component), which caps the composed field at
			//! 1-Lipschitz for ANY positive per-axis scale -- see the comment
			//! above smaxP in SDFGeometry.cpp.  So a squashed bone smin-blended
			//! against a round one still cannot overstep its own surface.  The
			//! price is STEPS, not correctness: minScale = min(aspect, 1)
			//! shrinks every step by that factor, so a very small aspect wants
			//! a larger `maxsteps`.
			//!
			//! DECLARE-BEFORE-USE, STRUCTURALLY CYCLE-FREE: `parent` must name
			//! `none` or a joint already declared on an EARLIER `joint` line.
			//! A joint can therefore never (directly or transitively) become
			//! its own ancestor -- there is no legal walk that revisits an
			//! index behind the one being declared, so no cycle check is
			//! needed beyond this one ordering rule.
			struct SkeletonGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				//! Same one-place-both-diagnostics pattern as
				//! ShapeLightAsciiChunkParser::Reject -- rect_light's Reject,
				//! same reason.
				static bool Reject( const std::string& why )
				{
					if( RISE::g_cstFinalizeDiagSink ) *RISE::g_cstFinalizeDiagSink = why;
					GlobalLog()->PrintEx( eLog_Error, "skeleton_geometry:: %s", why.c_str() );
					return false;
				}

				//! One declared `joint <name> <parent|none> <x> <y> <z> <radius> [aspect] [blend]` line.
				struct JointDecl
				{
					std::string name;
					std::string parent;
					double x, y, z, r;
					double aspect;        // 7th, OPTIONAL token; 1 = today's circular bone
					double blend;         // 8th, OPTIONAL token; the bone's OWN blend multiplier,
					                      // overriding the chunk's `blend` for the bone that ARRIVES
					                      // at this joint (same "shapes the bone that arrives at it"
					                      // convention aspect already uses) -- see the struct-level
					                      // "REGION-AWARE BLEND CONTROL" comment for why this is the
					                      // grammar's answer to "these parts are spatially close but
					                      // must NOT merge".
					bool   hasBlend;      // false = inherit the chunk's `blend` (today's behaviour, unchanged)
					int    parentIndex;   // resolved index into the joints vector; -1 = root (parent "none")
					JointDecl() : x(0), y(0), z(0), r(0), aspect(1.0), blend(0.0), hasBlend(false), parentIndex(-1) {}
				};

				//! Formats a double with full IEEE-double round-trip precision.
				//! This text is consumed ONCE, immediately, by
				//! SDFGeometry::ParsePartLines -- it is never saved to disk or
				//! shown to an author (the CST round-trip persists the SHORT
				//! `skeleton_geometry` chunk, not this expansion) -- so exact
				//! precision matters and brevity does not.
				//! NB: this is the only place in this file that snprintf's a
				//! double and hands the text straight back to a sscanf-based
				//! parser (SDFGeometry::ParsePartLines).  Safe today because
				//! both halves run under the same LC_NUMERIC (nothing in
				//! src/ calls setlocale) -- '%.17g'/sscanf agree on '.' as
				//! the decimal point only while that holds.  If a future
				//! caller ever sets a locale with a different decimal
				//! separator between format and parse, this round-trip
				//! breaks silently.
				static std::string Num( double v )
				{
					char buf[64];
					std::snprintf( buf, sizeof(buf), "%.17g", v );
					return buf;
				}

				// PLAIN `//`, not Doxygen `//!`: this documents a file-static
				// helper inside an anonymous-namespace parser struct, not public
				// API, and the grammar it has to quote contains `<a b c>` --
				// which clang's -Wdocumentation-html (Xcode GUI build; the make
				// build does not enable it) parses as an unclosed HTML <a> anchor
				// inside a Doxygen comment.  Escaping the brackets would leave the
				// one escaped code span in a file whose siblings are all
				// unescaped; demoting the comment removes the whole class of
				// problem instead, and loses nothing -- nothing generates docs
				// from a local helper.
				//
				// One `part` line in SDFGeometry::ParsePartLines' 16-token
				// grammar: `<prim> <op> <k> <px py pz> <exDeg eyDeg ezDeg>
				// <sx sy sz> <a b c> <round>`.  Only the FIRST scale component
				// is ever non-1: it carries the joint's `aspect` (see the
				// struct-level "WHICH AXIS FLATTENS" block), while y stays 1
				// because the roundcone's `c` is the bone LENGTH and z stays 1
				// because the squash is a pure one.  aspect == 1 formats as
				// "1" under %.17g, so a 6-token joint line still produces the
				// byte-identical `1 1 1` this function used to hardcode --
				// which SkeletonGeometryChunkTest's back-compat digest pins.
				static std::string PartLine( const char* prim, double k,
					double px, double py, double pz, double exDeg, double eyDeg, double ezDeg,
					double sx, double a, double b, double c )
				{
					return std::string( prim ) + " smin " + Num(k) + " " +
						Num(px) + " " + Num(py) + " " + Num(pz) + " " +
						Num(exDeg) + " " + Num(eyDeg) + " " + Num(ezDeg) + " " +
						Num(sx) + " 1 1 " +
						Num(a) + " " + Num(b) + " " + Num(c) + " 0";
				}

				//! Every OTHER chunk in this file forwards author-typed
				//! numbers verbatim; this one SYNTHESIZES new numbers (bone
				//! length from coordinate deltas, blend width from a radius
				//! product) that individually-finite inputs can still
				//! combine into a non-finite result (e.g. `dx*dx` overflows
				//! for absurd joint coordinates even though `dx` itself was
				//! a finite authored value).  `%.17g` on an inf/nan value
				//! prints "inf"/"nan", and SDFGeometry::ParsePartLines'
				//! `sscanf "%lf"` ACCEPTS that spelling -- so this chunk owns
				//! the finiteness check that the plain per-token text
				//! validation upstream (AllTokensAreFiniteNumbers) cannot
				//! cover, because it runs before this arithmetic exists.
				static bool AllFiniteD( std::initializer_list<double> vals )
				{
					for( double v : vals ) if( !std::isfinite( v ) ) return false;
					return true;
				}

				//! Euler DEGREES (ex, ey, ez) that rotate local +Y onto the
				//! given UNIT direction (dx,dy,dz) under RISE's SDF part
				//! convention R = Rz(ez)*Ry(ey)*Rx(ex) (SDFGeometry::
				//! RecomputePartDerived).  ez is 0 for every CLEARLY LEANING
				//! bone (horiz = sqrt(dx^2+dz^2) >= kNearVerticalHoriz,
				//! unchanged since before the near-vertical fix below): the
				//! rotation's SECOND column, the world direction local +Y
				//! maps to, is then (sin(ey)sin(ex), cos(ex), cos(ey)sin(ex)),
				//! so dy = cos(ex) and horiz = sin(ex) (ex chosen in [0,180],
				//! sin(ex) >= 0), giving ex = atan2(horiz, dy); then
				//! dx = sin(ey)sin(ex), dz = cos(ey)sin(ex), giving
				//! ey = atan2(dx, dz).  With ez = 0 the FIRST column (world
				//! direction local +X -- the `aspect` flatten axis) is
				//! (cos(ey), 0, -sin(ey)): ALWAYS exactly horizontal.  Pinned
				//! by SkeletonGeometryChunkTest's closed-form ellipse probe.
				//!
				//! NEAR-VERTICAL (horiz < kNearVerticalHoriz, ~0.6 degrees of
				//! lean): that ey = atan2(dx, dz) is the hairy-ball problem
				//! made concrete.  atan2 of a horizontal projection that is
				//! shrinking to a point is maximally sensitive to WHICH
				//! direction it is approached from, so a bone at
				//! (0.001, 0.999999, 0) and one at (0, 0.999999, 0.001) --
				//! indistinguishable to an author typing a third decimal --
				//! flatten along perpendicular world axes.  Simply snapping
				//! ex/ey to the canonical vertical values (0 or 180, ey = 0),
				//! which is what this function did for horiz < 1e-9 before
				//! this fix, throws away the POSITION claim: ex = 0 forces
				//! local +Y onto world +Y exactly, which is only where the
				//! bone actually points when horiz is EXACTLY 0.  For any
				//! nonzero horiz inside a widened threshold that would move
				//! the tip cap off the declared child joint by O(horiz) --
				//! up to ~1% of the bone length right at the boundary -- not
				//! the "identical to before" this chunk's whole design
				//! promises (struct comment, "A BONE'S END CAPS ARE ITS TWO
				//! JOINTS").
				//!
				//! So this band spends the THIRD Euler angle instead of
				//! zeroing anything.  A rotation has exactly 3 degrees of
				//! freedom; fixing local +Y's world direction uses 2 of them,
				//! leaving exactly ONE free -- the ROLL around that
				//! direction, i.e. ez, which the clearly-leaning formula
				//! above always burns on 0.  Spend it here: solve for the
				//! (ex,ey,ez) triple whose SECOND column is still EXACTLY
				//! (dx,dy,dz) (endpoint exactness, UNCONDITIONAL) and whose
				//! FIRST column is world +X Gram-Schmidt-projected
				//! perpendicular to (dx,dy,dz) -- the closest horizontal-ish
				//! axis to world X that is still orthogonal to the bone
				//! direction.  That projection, normalize((1,0,0) -
				//! dx*(dx,dy,dz)), has magnitude sqrt(1-dx^2) = sqrt(dy^2+dz^2)
				//! -- BOUNDED AWAY FROM ZERO throughout the near-vertical band
				//! (dy is close to +-1 there), so it is well-conditioned
				//! exactly where atan2(dx,dz) alone is not, and it varies
				//! CONTINUOUSLY as (dx,dz) -> (0,0) from any direction,
				//! landing on exactly world +X at horiz = 0.  The price: for
				//! a genuinely (tiny) leaning bone in this band the flatten
				//! axis is no longer bit-exactly horizontal -- it tilts out
				//! of the ground plane by an angle bounded by the lean
				//! itself (at most ~kNearVerticalHoriz radians), invisible
				//! next to the ~0.6 degree threshold that put it there.
				//! Endpoint exactness is unconditional; horizontality is
				//! exact for clearly-leaning bones and near-exact (bounded by
				//! the lean) for near-vertical ones.  Verified analytically
				//! (Cy reproduces the target direction to double-precision
				//! roundoff for every direction tried, including right at the
				//! threshold boundary) and pinned by
				//! SkeletonGeometryChunkTest's near-vertical endpoint probe
				//! and flatten-axis-stability probe.
				static constexpr double kNearVerticalHoriz = 0.01;   // ~0.57 degrees of lean

				static void DirectionToEulerDeg( double dx, double dy, double dz,
					double& exDeg, double& eyDeg, double& ezDeg )
				{
					const double horiz = std::sqrt( dx*dx + dz*dz );
					if( horiz < kNearVerticalHoriz ) {
						// Gram-Schmidt: world +X projected perpendicular to the
						// TRUE bone direction (dx,dy,dz) -- see the struct-level
						// comment above for the derivation.  |v| bounded away
						// from 0 throughout this band (dy ~= +-1 here).
						const double vx = 1.0 - dx*dx, vy = -dx*dy, vz = -dx*dz;
						const double vlen = std::sqrt( vx*vx + vy*vy + vz*vz );
						const double xhx = vx / vlen, xhy = vy / vlen, xhz = vz / vlen;
						// z-component of local +Z = cross(local +X, target) --
						// the only component ex's solve needs.
						const double zhz = xhx*dy - xhy*dx;
						const double c2 = std::sqrt( xhx*xhx + xhy*xhy );   // cos(ey), > 0 here
						eyDeg = std::atan2( -xhz, c2 ) * RAD_TO_DEG;
						ezDeg = std::atan2( xhy, xhx ) * RAD_TO_DEG;
						// atan2(dz/c2, zhz/c2) == atan2(dz, zhz) for c2 > 0.
						exDeg = std::atan2( dz, zhz ) * RAD_TO_DEG;
						return;
					}
					exDeg = std::atan2( horiz, dy ) * RAD_TO_DEG;
					eyDeg = std::atan2( dx, dz ) * RAD_TO_DEG;
					ezDeg = 0.0;
				}

				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					const std::string name   = bag.GetString( "name", "noname" );
					const double blend        = bag.GetDouble( "blend", 0.35 );
					const unsigned int maxSteps      = bag.GetUInt( "maxsteps", 256 );
					const double eps                 = bag.GetDouble( "epsilon", 0.0 );
					const unsigned int samplingDetail = bag.GetUInt( "sampling_detail", 64 );

					const std::vector<std::string>& jointLines = bag.GetRepeatable( "joint" );
					if( jointLines.empty() ) {
						return Reject( "at least one `joint` is required" );
					}
					if( blend < 0.0 ) {
						char buf[64];
						std::snprintf( buf, sizeof(buf), "%g", blend );
						return Reject( std::string( "`blend` (" ) + buf + ") must be >= 0" );
					}

					std::vector<JointDecl> joints;
					joints.reserve( jointLines.size() );
					std::map<std::string,int> nameToIndex;

					for( std::size_t i = 0; i < jointLines.size(); ++i ) {
						const std::string& line = jointLines[i];

						// Tokenize on whitespace -- a mixed identifier/number
						// line, so the strict all-numeric AllTokensAreFiniteNumbers
						// helper is applied only to the trailing numeric run
						// (same mixed-line idiom sweep_geometry's
						// `profile_circle` uses elsewhere in this file), and an
						// exact 6/7/8-token count catches both too few and too
						// many (trailing garbage glued as its own token).  The
						// 7th token is the OPTIONAL `aspect`; the 8th (blend-
						// domain-control slice, 2026-08-25) is the OPTIONAL
						// per-joint `blend` override -- see the struct-level
						// "REGION-AWARE BLEND CONTROL" comment.  6 tokens is
						// the pre-doc-90 grammar (aspect 1, inherited blend);
						// 7 adds aspect; 8 adds the override, and REQUIRES
						// aspect to be spelled too (no "skip aspect, give
						// blend" positional gap -- the same reasoning
						// AllTokensAreFiniteNumbers' contiguous-run design
						// already forces: this grammar has no named/keyword
						// tokens, only position).
						std::vector<std::string> toks;
						{
							std::istringstream iss( line );
							std::string t;
							while( iss >> t ) toks.push_back( t );
						}
						if( toks.size() != 6 && toks.size() != 7 && toks.size() != 8 ) {
							char buf[32];
							std::snprintf( buf, sizeof(buf), "%u", (unsigned int)toks.size() );
							return Reject( "joint `" + line + "` must be 6, 7 or 8 tokens "
								"`<name> <parent|none> <x> <y> <z> <radius> [aspect] [blend]` -- got " + buf );
						}
						const bool haveAspect = ( toks.size() >= 7 );
						const bool haveBlend  = ( toks.size() == 8 );
						const int  nNumeric   = haveBlend ? 6 : ( haveAspect ? 5 : 4 );

						std::string tail = toks[2] + " " + toks[3] + " " + toks[4] + " " + toks[5];
						if( haveAspect ) tail += " " + toks[6];
						if( haveBlend )  tail += " " + toks[7];
						int nTok = 0;
						if( !AllTokensAreFiniteNumbers( tail.c_str(), &nTok ) || nTok != nNumeric ) {
							return Reject( "joint `" + line + "`: the last " +
								( haveBlend ? "six fields (x y z radius aspect blend)"
								            : haveAspect ? "five fields (x y z radius aspect)"
								                         : "four fields (x y z radius)" ) +
								" must be finite numbers, with no trailing garbage" );
						}

						// NB: `jd.aspect`/`jd.blend` are left at their
						// constructed defaults when the line has fewer
						// numeric fields -- sscanf simply stops, it does not
						// zero an unmatched argument.
						JointDecl jd;
						jd.name = toks[0];
						jd.parent = toks[1];
						jd.hasBlend = haveBlend;
						std::sscanf( tail.c_str(), "%lf %lf %lf %lf %lf %lf",
							&jd.x, &jd.y, &jd.z, &jd.r, &jd.aspect, &jd.blend );

						if( nameToIndex.find( jd.name ) != nameToIndex.end() ) {
							return Reject( "joint `" + jd.name + "` is declared more than once" );
						}
						if( jd.parent == jd.name ) {
							return Reject( "joint `" + jd.name + "` names itself as `parent` -- "
								"a joint cannot be its own parent" );
						}
						if( jd.parent != "none" ) {
							std::map<std::string,int>::const_iterator it = nameToIndex.find( jd.parent );
							if( it == nameToIndex.end() ) {
								return Reject( "joint `" + jd.name + "`: parent `" + jd.parent +
									"` is not a joint declared EARLIER in this chunk (declare-before-use -- "
									"forward references and unknown names are both rejected the same way, "
									"and this rule is exactly what makes a parent cycle impossible)" );
							}
							jd.parentIndex = it->second;
						}
						if( !( jd.r > 0.0 ) ) {
							char buf[64];
							std::snprintf( buf, sizeof(buf), "%g", jd.r );
							return Reject( "joint `" + jd.name + "`: radius (" + buf + ") must be > 0" );
						}
						// `aspect` is a RATIO, so 0 (a zero-thickness bone with
						// no surface) and a negative (a mirrored bone) are both
						// meaningless rather than merely extreme -- refused,
						// naming the joint, the same shape as the radius check
						// directly above.  Non-finite is already impossible
						// here (AllTokensAreFiniteNumbers rejects nan/inf
						// spellings upstream), but the `> 0` test is written in
						// the negated idiom so a NaN that ever reached it would
						// fail rather than pass.
						if( !( jd.aspect > 0.0 ) ) {
							char buf[64];
							std::snprintf( buf, sizeof(buf), "%g", jd.aspect );
							return Reject( "joint `" + jd.name + "`: aspect (" + buf + ") must be > 0 "
								"-- it is the bone cross-section's width-to-depth RATIO (1 = a round "
								"bone, 0.6 = a thigh), not a thickness" );
						}
						// Blend-domain-control slice: same `>= 0` rule the
						// chunk-level `blend` enforces above, and for the
						// identical reason -- 0 is a legal, INTENDED value
						// here (a hard union at this one bone, the whole
						// point of the override), a negative one is not.
						if( jd.hasBlend && !( jd.blend >= 0.0 ) ) {
							char buf[64];
							std::snprintf( buf, sizeof(buf), "%g", jd.blend );
							return Reject( "joint `" + jd.name + "`: blend (" + buf + ") must be >= 0" );
						}
						if( jd.parentIndex >= 0 ) {
							const JointDecl& par = joints[jd.parentIndex];
							const double dx = jd.x - par.x, dy = jd.y - par.y, dz = jd.z - par.z;
							if( !( dx*dx + dy*dy + dz*dz > 1e-18 ) ) {
								return Reject( "joint `" + jd.name + "` and its parent `" + par.name +
									"` are at coincident positions -- a zero-length bone has no direction" );
							}
						}

						nameToIndex[jd.name] = (int)joints.size();
						joints.push_back( jd );
					}

					// Children count per joint -- ONLY a parentless, childless
					// joint gets its own sphere part; see the struct-level
					// comment for why every other joint needs none.
					std::vector<int> childCount( joints.size(), 0 );
					for( std::size_t i = 0; i < joints.size(); ++i )
						if( joints[i].parentIndex >= 0 ) childCount[(std::size_t)joints[i].parentIndex]++;

					// F6(b): a large skeleton (dozens of bones) with a
					// systematically bad radius/length ratio would otherwise
					// emit one eLog_Warning line per degenerate bone with no
					// cap.  Follow the same first-N-then-summary shape as
					// SDFGeometry.cpp's missed-feature-cells diagnostic
					// (EnsureSamplingStructure): name the first few offenders
					// so an author can start fixing immediately, then fold
					// the rest into one summary line instead of flooding the
					// log.
					unsigned int degenerateBoneCount = 0;
					static const unsigned int kMaxNamedDegenerateBones = 3;
					// Same first-N-then-summary discipline for the one OTHER
					// non-rejecting diagnostic this chunk owns: an `aspect`
					// declared where nothing can consume it (see the emission
					// loop's `else if` below).
					unsigned int inertAspectCount = 0;
					static const unsigned int kMaxNamedInertAspects = 3;

					std::string parts;
					for( std::size_t i = 0; i < joints.size(); ++i ) {
						const JointDecl& j = joints[i];
						if( j.parentIndex >= 0 ) {
							const JointDecl& par = joints[(std::size_t)j.parentIndex];
							const double dx = j.x - par.x, dy = j.y - par.y, dz = j.z - par.z;
							const double len = std::sqrt( dx*dx + dy*dy + dz*dz );
							// F6(a): `dx*dx` etc. can overflow to inf for absurd joint
							// coordinates even though dx itself was finite -- see the
							// AllFiniteD comment above for why this chunk owns the check.
							if( !AllFiniteD( { dx, dy, dz, len } ) ) {
								return Reject( "joint `" + par.name + "` -> `" + j.name +
									"`: the synthesized bone length is not finite (joint "
									"coordinates are too large) -- reduce the joint positions" );
							}
							// Non-rejecting diagnostic (not a Reject -- the geometry is
							// still well-defined, see SDFGeometry.cpp primLocalAABB's
							// roundcone case): when one cap sphere's radius so exceeds
							// the other that it swallows it whole (|rp-rc| > bone
							// length), the bone's visible surface collapses to a plain
							// sphere at the larger joint -- the smaller joint and the
							// taper between them contribute no visible geometry.  Name
							// both joints so an author who wanted a visible taper learns
							// which bone degenerated, same warn-not-reject style as
							// glint_modifier's coverage/fill clamp diagnostics above.
							if( std::fabs( par.r - j.r ) > len ) {
								if( degenerateBoneCount < kMaxNamedDegenerateBones ) {
									GlobalLog()->PrintEx( eLog_Warning,
										"skeleton_geometry `%s`: bone `%s` -> `%s` is degenerate "
										"(|radius delta| %g > bone length %g) -- the smaller joint's "
										"sphere is entirely contained in the larger one's, so this "
										"bone collapses to a plain sphere at `%s` with no visible taper",
										name.c_str(), par.name.c_str(), j.name.c_str(),
										std::fabs( par.r - j.r ), len,
										( par.r > j.r ? par.name.c_str() : j.name.c_str() ) );
								}
								degenerateBoneCount++;
							}
							double exDeg = 0.0, eyDeg = 0.0, ezDeg = 0.0;
							DirectionToEulerDeg( dx/len, dy/len, dz/len, exDeg, eyDeg, ezDeg );
							// Blend-domain-control slice (2026-08-25): `j.blend`
							// (the CHILD joint's own override -- "shapes the
							// bone that arrives at it", same convention
							// `j.aspect` already uses two lines below) replaces
							// the chunk-level `blend` for THIS bone only when
							// the joint line carried one.  This is the
							// grammar's answer to "these parts are spatially
							// close but must not merge": since Map()'s fold is
							// sequential and GLOBAL (SDFGeometry.cpp's own
							// comment -- a part's smin step compares against
							// whatever the running field already is, not just
							// its graph-neighbour), a bone's OWN k is what
							// bounds how far THAT bone's own fold step can
							// bridge, regardless of what the running field
							// represents -- so overriding a specific bone's k
							// down to (or toward) 0 makes THAT bone's fold
							// step a hard union no matter what unrelated
							// geometry happens to sit nearby when a pose
							// brings two chain-distant regions close together.
							const double kBlend = j.hasBlend ? j.blend : blend;
							const double k = kBlend * std::min( par.r, j.r );
							if( !AllFiniteD( { exDeg, eyDeg, ezDeg, k } ) ) {
								return Reject( "joint `" + par.name + "` -> `" + j.name +
									"`: a synthesized bone value is not finite (blend * radius "
									"or the direction-to-Euler conversion overflowed) -- reduce "
									"`blend` or the joint radii" );
							}
							// `j.aspect`, not the parent's: a joint's aspect
							// shapes the bone that ARRIVES at it (struct
							// comment, "A JOINT'S ASPECT SHAPES THE BONE THAT
							// ARRIVES AT IT").  ezDeg is nonzero only in the
							// near-vertical band (DirectionToEulerDeg's
							// comment) -- everywhere else this is the same
							// literal 0.0 it always was.
							parts += PartLine( "roundcone", k, par.x, par.y, par.z,
								exDeg, eyDeg, ezDeg, j.aspect, par.r, j.r, len );
							parts += "\n";
						} else if( childCount[i] == 0 ) {
							// Blend-domain-control slice: an isolated joint's
							// OWN blend override applies here too (the sphere
							// IS the bone that "arrives" at it, in the sense
							// that no other part supplies its cap) -- same
							// `j.hasBlend ? j.blend : blend` rule as the bone
							// branch above, for consistency.
							const double kBlend = j.hasBlend ? j.blend : blend;
							const double k = kBlend * j.r;
							if( !AllFiniteD( { k } ) ) {
								return Reject( "joint `" + j.name +
									"`: the synthesized sphere blend width (blend * radius) is "
									"not finite -- reduce `blend` or the joint radius" );
							}
							parts += PartLine( "sphere", k, j.x, j.y, j.z, 0.0, 0.0, 0.0, j.aspect, j.r, 0.0, 0.0 );
							parts += "\n";
						}
						// else: root with children -- no part of its own (the
						// struct-level comment explains why none is needed).
						// Which is exactly why its `aspect` has nothing to
						// shape: no bone ARRIVES at a root, and it emits no
						// sphere either.  Say so rather than dropping the token
						// in silence -- warn-not-reject, matching the
						// degenerate-bone diagnostic above, because the
						// geometry the author asked for is still perfectly
						// well-defined.
						else if( j.aspect != 1.0 ) {
							if( inertAspectCount < kMaxNamedInertAspects ) {
								GlobalLog()->PrintEx( eLog_Warning,
									"skeleton_geometry `%s`: joint `%s` declares aspect %g, which shapes "
									"NOTHING -- `%s` is a root WITH children, so no bone arrives at it and "
									"it emits no part of its own. A joint's aspect shapes the bone that "
									"arrives at it FROM its parent, so move this aspect onto the child "
									"joint(s) the bone(s) run TO",
									name.c_str(), j.name.c_str(), j.aspect, j.name.c_str() );
							}
							inertAspectCount++;
						}
					}
					if( inertAspectCount > kMaxNamedInertAspects ) {
						GlobalLog()->PrintEx( eLog_Warning,
							"skeleton_geometry `%s`: ...and %u more joints whose `aspect` shapes nothing",
							name.c_str(), inertAspectCount - kMaxNamedInertAspects );
					}
					if( degenerateBoneCount > kMaxNamedDegenerateBones ) {
						GlobalLog()->PrintEx( eLog_Warning,
							"skeleton_geometry `%s`: ...and %u more degenerate bones",
							name.c_str(), degenerateBoneCount - kMaxNamedDegenerateBones );
					}

					return pJob.AddSDFGeometry( name.c_str(), "", parts.c_str(), maxSteps, eps, samplingDetail );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "skeleton_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "A JOINT GRAPH that expands into ONE sdf_geometry: a roundcone per bone (parent -> child), smin-blended -- creature flesh authored the way an animator thinks (joints and bones), not sdf_geometry's raw part-line grammar. Each `joint` line is `<name> <parent|none> <x> <y> <z> <radius> [aspect] [blend]`; every joint but a root must name an ALREADY-DECLARED joint as `parent` (declare-before-use), which is exactly what makes a parent cycle structurally impossible. A BONE NEED NOT BE A PIPE: the optional `aspect` is the bone cross-section's width-to-depth ratio, so a limb can be a FLATTENED strap rather than the tube one-radius-per-joint would otherwise force -- a thigh is aspect 0.6, a fin-arm 0.35, a wing-arm 1.0. REGION-AWARE BLEND CONTROL: the optional per-joint `blend` overrides the chunk-level `blend` for JUST the bone arriving at that joint -- because Map()'s smin fold is SEQUENTIAL and GLOBAL (every part blends against whatever the running field already is, not just its graph-neighbour), a bone's own k is what bounds how far THAT bone's own fold step can bridge, so a pose that brings two chain-distant regions into close proximity (a curled tail near a flank, a tucked muzzle near a haunch) can keep them visually distinct by giving the bone(s) near that seam a near-zero override, while every other bone keeps the chunk's normal, permissive blend -- no multi-chunk split needed. A bone's end caps ARE its two joints -- the roundcone's spherical caps sit exactly at the parent's and child's positions with exactly their radii -- so joints need NO separate sphere of their own; the one exception is a joint with neither parent nor children (otherwise invisible), which gets a `sphere` part instead. `blend` multiplies min(parent radius, child radius) to give each bone's smin blend width (0 = hard union, visible creases at every joint). Registers under this chunk's OWN `name`, exactly like a hand-authored sdf_geometry. COST: parts = bones + isolated joints, NOT one part per joint -- each bone (parent -> child edge) expands to one roundcone part, each parentless childless joint expands to one sphere part, and a joint with children contributes no part of its own (its caps are supplied by its outgoing bone(s)), and Map() is O(parts) per sphere-trace step with no acceleration structure over parts -- a skeleton is a render-time budget, not a free abstraction; a hand-authored sdf_geometry typically has a handful of parts, a skeleton invites 30-70.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";  p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "joint"; p.kind = ValueKind::String; p.repeatable = true; p.required = true;
						  p.description = "One joint (repeatable; at least one required): `<name> <parent|none> <x> <y> <z> <radius> [aspect] [blend]`. `parent` is `none` for a root, or the name of a joint declared on an EARLIER `joint` line -- forward references and unknown names are both rejected, and this ordering is what makes a cycle impossible. `radius` must be > 0, and a non-root joint may not sit exactly on top of its parent (a zero-length bone has no direction). OPTIONAL `aspect` (> 0, default 1 = a round bone) is the cross-section's WIDTH-to-DEPTH ratio: it NARROWS the bone SIDE-TO-SIDE in the horizontal plane, leaving `radius` exact across the perpendicular that lies in the vertical plane containing the bone (a vertical bone with aspect 0.5 is half as wide along X as along Z), and it never makes a bone THICKER than its radius. A joint's aspect shapes the bone that ARRIVES at it from its parent -- there is no inheritance, so set it per joint along a chain, and on a root WITH children it shapes nothing and is warned about. A bone leaning less than ~0.6 degrees off vertical flattens along world X, same as one that is exactly vertical (near that threshold the flatten axis stays endpoint-exact via a different construction than the exactly-vertical case, but the effect an author sees is identical). An isolated joint (no parent, no children) flattens its `sphere` part along world X unconditionally, the same canonical choice, since no bone direction exists to derive an axis from. Flatten a thigh to ~0.6, a fin or paddle-limb to ~0.35; a very small aspect wants a larger `maxsteps`. OPTIONAL `blend` (>= 0, REQUIRES `aspect` to also be spelled -- purely positional grammar, no gap) overrides the chunk-level `blend` for JUST this joint's incoming bone -- same 'shapes the bone that ARRIVES at it, no inheritance' rule as aspect. Use it to keep a POSE-INDUCED proximity from smin-bridging: give the bone(s) nearest a deliberate cross-region seam (e.g. a curled tail's root, a tucked muzzle's neck bone) a small or 0 override (0 = hard union at that one bone) while every other bone keeps the chunk default -- see the chunk's own description for why this works despite the fold being global. DECLARATION ORDER MATTERS: two spatially-close, graph-unrelated joints bridge under the LATER-DECLARED one's own blend -- override the later-declared joint of the pair, not the earlier one, or the override does nothing."; }
						{ auto& p = P(); p.name = "blend"; p.kind = ValueKind::Double;
						  p.description = "Multiplier (unit-free) on min(parent radius, child radius) giving each bone's smin blend width in world units. 0 = hard union (visible creases at every joint); must be >= 0"; p.defaultValueHint = "0.35"; }
						{ auto& p = P(); p.name = "maxsteps"; p.kind = ValueKind::UInt; p.description = "Sphere-trace step cap, passed through to the expanded sdf_geometry"; p.defaultValueHint = "256"; }
						{ auto& p = P(); p.name = "epsilon"; p.kind = ValueKind::Double; p.description = "Surface hit epsilon as a fraction of the bbox diagonal (0 = auto), passed through to the expanded sdf_geometry"; p.defaultValueHint = "0.0"; }
						{ auto& p = P(); p.name = "sampling_detail"; p.kind = ValueKind::UInt; p.description = "Tessellation cells along the longest bbox axis for area-light / SSS surface sampling (clamped 8..256), passed through to the expanded sdf_geometry"; p.defaultValueHint = "64"; }
						return cd;
					}();
					return d;
				}
			};

			struct CartesianDiskGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					const double radius = bag.GetDouble( "radius", 20.6 );
					const int meshN = (int)bag.GetUInt( "mesh_n", 560 );
					return pJob.AddCartesianDiskGeometry( name.c_str(), radius, meshN );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "cartesian_disk_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "A FLAT Cartesian-grid circular disk: linear Cartesian UV (u=(x+R)/2R), +Z normals, uniform world-space cell density everywhere (unlike a polar fan).  The general flat base for displacing an arbitrary 2D field -- an expression_function2d, a texture, noise -- onto a disk via displaced_geometry with uv_seam_fold FALSE.  (A guilloché dial = this base + a guilloché expression_function2d displacement.)";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";   p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "radius"; p.kind = ValueKind::Double; p.description = "Disk radius (world units)"; p.defaultValueHint = "20.6"; }
						{ auto& p = P(); p.name = "mesh_n"; p.kind = ValueKind::UInt;   p.description = "Grid samples across the diameter (tessellation density; clamped 2..4096)"; p.defaultValueHint = "560"; }
						return cd;
					}();
					return d;
				}
			};

			struct ExpressionFunction2DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					std::string finalExpr = bag.GetString( "expr", "" );

					// review-round unification (doc 88 sect. 7 decision 5): the
					// param/def parsing + ExpressionProgram::Builder orchestration
					// used to be reimplemented inline here (a plain `sscanf`
					// `<name> <number>` scanner, no ExpressionParamSpec metadata
					// grammar, its own copy of the def-splitting loop) instead of
					// calling the SAME BuildExpressionProgramFromChunkFields
					// (ExpressionPainter.h) helper `expression_painter` and
					// `scalar_painter{expression}` already share -- two
					// independently-maintained copies of the identical parsing
					// logic, exactly the duplication doc 88 flagged for a later
					// fold. Now calls the shared helper with
					// enableContextVars=false, autoRegisterSeed=false -- see
					// that function's own doc comment for why those two flags
					// (not the evaluation engine, which was ALREADY shared via
					// ExpressionProgram) are the one place this surface must
					// differ from expression_painter's.
					//
					// BIT-IDENTICAL FOR EVERY EXISTING BODY: ParseParamSpecLine
					// (ExpressionParamSpec.h) parses a plain `<name> <number>`
					// line (the only form any in-tree scene's expression_function2d
					// param line uses -- verified) to the SAME name/value the old
					// sscanf scanner produced; the ONLY behavior difference is
					// that trailing content past `<name> <number>` now has to be
					// a well-formed `min`/`max`/`step`/`label` clause (the old
					// scanner silently ignored trailing garbage) -- a stricter,
					// not looser, validation, and the richer grammar this brings
					// along is inert here (min/max/step/label are parsed but
					// never read by ExpressionFunction2DPainter, exactly as
					// undeclared-but-parsed metadata already sits inert on
					// expression_painter -- no NEW evaluation capability, no new
					// keyword, context vars still off).
					Implementation::ExpressionProgram prog = Implementation::ExpressionProgram::Invalid();
					std::vector<Implementation::ParamSpec> specs;   // discarded -- this surface doesn't carry S4 introspection metadata (frozen, not extended)
					const std::string context = std::string( "expression_function2d `" ) + name + "`";
					std::string exprErr;
					if( !Implementation::BuildExpressionProgramFromChunkFields(
							context, bag.GetRepeatable( "param" ), bag.GetRepeatable( "def" ), Scalar( 0 ), finalExpr, prog, specs,
							/*enableContextVars=*/false, /*autoRegisterSeed=*/false, &exprErr ) ) {
						// See Job::AddExpressionPainter's twin site -- thread the
						// specific compiler diagnostic into the CST sink instead
						// of leaving the caller with the generic apply-failed text.
						if( RISE::g_cstFinalizeDiagSink ) *RISE::g_cstFinalizeDiagSink = exprErr;
						return false;
					}

					IJobPriv* pPriv = dynamic_cast<IJobPriv*>( &pJob );
					if( !pPriv ) {
						GlobalLog()->PrintEx( eLog_Error, "expression_function2d `%s`: IJobPriv unavailable", name.c_str() );
						return false;
					}
					IPainter* painter = 0;
					if( !RISE_API_CreateExpressionFunction2D( &painter, prog ) ) {
						return false;
					}
					pPriv->GetPainters()->AddItem( painter, name.c_str() );
					pPriv->GetFunction2Ds()->AddItem( painter, name.c_str() );
					painter->release();
					return true;
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "expression_function2d"; cd.category = ChunkCategory::Painter;
						cd.description = "A procedural 2D field whose value is a MATH EXPRESSION authored in the scene -- the in-scene-scripted analogue of perlin2d / worley.  Usable as a displacement (displaced_geometry), a greyscale colour (a colour slot / blend_painter mask), or a physical scalar (scalar_painter { function2d <name> }).  Variables u, v (the UV); declare `param <name> <number>` constants and `def <name> <expr>` named sub-expressions (let-bindings, in order, referencing u/v/params/earlier-defs), then the final `expr`.  Functions: sin cos tan asin acos atan exp log sqrt abs floor ceil frac sign atan2 mod min max pow hypot step clamp smoothstep mix select; operators + - * / % ^ and comparisons (yield 1/0); constants pi tau e.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";  p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "param"; p.kind = ValueKind::String; p.repeatable = true; p.description = "Named numeric constant `<name> <number>` (repeatable); visible to every def and the final expr"; }
						{ auto& p = P(); p.name = "def";   p.kind = ValueKind::String; p.repeatable = true; p.description = "Named sub-expression `<name> <expr>` (repeatable, in order); a let-binding referencing u, v, params, and earlier defs"; }
						{ auto& p = P(); p.name = "expr";  p.kind = ValueKind::String; p.description = "The final value expression over u, v, params and defs"; }
						return cd;
					}();
					return d;
				}
			};

			struct ExpressionPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					std::string finalExpr = bag.GetString( "expr", "" );
					const double seed = bag.GetDouble( "seed", 0.0 );
					const double time = bag.GetDouble( "time", 0.0 );

					const std::vector<std::string>& params = bag.GetRepeatable( "param" );
					const std::vector<std::string>& defs = bag.GetRepeatable( "def" );

					std::vector<const char*> paramPtrs;
					paramPtrs.reserve( params.size() );
					for( std::size_t i = 0; i < params.size(); ++i ) paramPtrs.push_back( params[i].c_str() );
					std::vector<const char*> defPtrs;
					defPtrs.reserve( defs.size() );
					for( std::size_t i = 0; i < defs.size(); ++i ) defPtrs.push_back( defs[i].c_str() );

					return pJob.AddExpressionPainter(
						name.c_str(), finalExpr.c_str(),
						paramPtrs.empty() ? nullptr : &paramPtrs[0], (unsigned int)paramPtrs.size(),
						defPtrs.empty() ? nullptr : &defPtrs[0], (unsigned int)defPtrs.size(),
						seed, time );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "expression_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "A procedural COLOUR field authored as a MATH EXPRESSION with the FULL 3D shading context -- the doc-88 texture-expression VM on the colour pipe.  Unlike expression_function2d (UV-only), the body sees u, v, P (world position), Po (object position), N (shading normal), fw (world-space filter width at the hit; 0.0 where no footprint is available -- secondary bounces, non-pinhole cameras), fwo (the SAME footprint measured in Po's OBJECT space, so an object-space noise domain filters too -- see AUTOMATIC DOMAIN SCALE below), time, and curv / curvR (surface curvature; see below) -- plus the occlusion(radius) / convexity(radius) / thickness(radius) / proximity(radius) / interior(radius) BUILTINS (see below).  `curv` / `curvR` are SURFACE CURVATURE at the hit, the geometry-derived signal wear and grime masks key on: POSITIVE = convex (an edge), NEGATIVE = concave (a crevice), 0 = flat.  `curvR` is the raw signed MEAN curvature in 1/world-length; `curv` is that value normalized by the hit geometry's world bounding-box diagonal, so it reads O(1) at object scale and `clamp(curv,0,1)` is an edge-wear mask / `clamp(-curv,0,1)` a crevice mask on ANY scene scale -- prefer `curv` unless you genuinely want physical units.  Quality by geometry: EXACT on sdf_geometry / skeleton_geometry (a true differential quantity on an implicit surface) and on the analytic curved primitives (sphere, ellipsoid, torus, cylinder); FACETED on triangle meshes (it is only as good as the vertex normals, and jumps across shared edges); correctly 0 on planar primitives; and 0 as an ABSENCE, not a claim of flatness, on the bezier/bilinear patch stubs, which report no derivatives at all.  Under NON-UNIFORM object scale the normalization is a documented geometric-mean approximation (no single length is right for a `scale 4 0.05 4` panel).  It is fine-scale and RADIUS-FREE -- the local differential curvature, not a radius-sampled \"wear the 2cm edges, ignore the 2mm ones\" signal.  BUMP / NORMAL MAPS DO NOT MOVE IT: it comes from the geometric normal field, so `curv` and `N` legitimately disagree on a bump-mapped surface -- that is correct (a wear mask wants the form, not the texture, and a bump-perturbed curvature would double-count detail the bump map already shades).  GEOMETRY SIGNALS -- `occlusion(radius)`, `convexity(radius)` and `thickness(radius)`, the three ARG-TAKING builtins that ask the hit geometry about itself.  `occlusion` returns [0,1] with 1 = UNOCCLUDED (the universal convention -- white is open, dark is cavity), `convexity` returns [0,1] with 0 = FLAT-OR-CONCAVE and 1 = a knife edge, `thickness` returns [0,1] with 1 = THICK, normalized by the query radius.  `occlusion` AND `convexity` PARTITION ONE QUESTION -- how open is the surface at scale `radius`, measured against what a FLAT surface would read -- with occlusion reporting how much LESS open than flat and convexity how much MORE.  Neither is ever a residual of the other, and that is the thing to rely on while authoring.  (1) A MERELY CONVEX EDGE READS occlusion EXACTLY 1, the same as a flat face; occlusion darkens only for real cavities, so `1 - occlusion(r)` is a usable crevice mask with NO thresholding needed to keep it off the arrises.  (Before 2026-09-06 it was not: the old estimator returned 0.707 on any convex edge a CSG `intersect`/`subtract` produced, and the SAME 0.707 in a concave 90-degree valley, so the two were indistinguishable and every such scene needed a hand-tuned smoothstep.)  (2) A FLAT FACE AND EVERY CAVITY READ convexity EXACTLY 0, and every value above that has a FIXED GEOMETRIC MEANING on any object at any scene scale -- 0.5 IS a 90-degree arris, 0.75 IS a three-face corner -- which is precisely what `curv` cannot offer, since `curv` is an unbounded differential quantity whose useful thresholds have to be probe-measured per object.  Reach for `convexity(r)` for edge wear (\"wear the edges about r across, ignore the rest\"), and for `curv` when you want the form's signed bending at no particular scale.  THEY ARE MEASURED DIFFERENTLY, which matters in two places: occlusion sphere-traces 12 cosine-weighted directions over the outward hemisphere, the set spun about the normal per hit so the answer is an expectation rather than a multiple of 1/12 (the same integral -- and since 2026-09-07 the same per-hit spin -- a mesh's baked AO computes) and therefore SEES NARROW APERTURES a crack, a slot, a fold; convexity samples the query BALL and therefore sees smooth bulges too (on a convex sphere of radius rho it reads 3*radius/(8*rho), where a purely directional measure would read 0).  On edges, creases and corners the two agree exactly.  Occlusion is also the more expensive of the two by roughly 3x (~91 field evaluations against convexity's 32) -- neither costs anything unless the body calls it.  `radius` is NOT a world length: it is a FRACTION of the hit geometry's own characteristic size (its bounding-box diagonal), so `occlusion(0.05)` means \"5% of the object\" and reads identically at any scene scale and on any instance of that geometry.  A LITERAL radius <= 0 is a COMPILE error; a computed one that lands <= 0 returns the neutral value.  TWO GEOMETRY FAMILIES ANSWER THEM, by different means.  The SDF family (`sdf_geometry`, `skeleton_geometry`) evaluates them LIVE from its distance field and accepts a DYNAMIC radius (any expression, recomputed per hit).  INDEXED TRIANGLE MESHES answer from a per-vertex field BAKED LAZILY -- the first query at a given radius pays the bake (it announces itself and its millisecond cost in the log), every later sample is a barycentric read of it -- which makes the radius on a mesh necessarily a LITERAL: a computed one reads the NEUTRAL fallback rather than silently borrowing a table baked at a different scale, and at most 8 distinct literal radii per signal per mesh are baked before further ones read neutral too.  Everything else returns the NEUTRAL fallback: the analytic primitives, non-indexed meshes, and heightfield-mode `sdf_geometry` (whose global Lipschitz bound would make a local answer systematically wrong).  That fallback is deliberately the do-nothing end of each range -- occlusion 1 (unoccluded), thickness 1 (thick), convexity 0 (flat) -- so a mask built on an unsupported geometry lights NOTHING up rather than lighting everything up.  Note that convexity's neutral sits at the OPPOSITE end of its range from the other two, for exactly that reason: an absent edge-wear signal must mean \"no edge here\", never \"knife edge everywhere\".  They cost nothing unless the body actually calls them.  CROSS-OBJECT PROXIMITY -- `proximity(radius)` and `interior(radius)`, the FOURTH and FIFTH signal builtins and the only two that look at the REST OF THE SCENE.  The three above ask the hit geometry about ITSELF; this one returns [0,1] with 1 = TOUCHING and 0 = nothing within `radius`, computed as `1 - d/radius` for `d` the shortest distance from the hit to the surface of any OTHER object.  It is the quantity contact grime actually is -- dirt collecting where a nail rests on a plank, dust where a wall meets a floor -- and it is NOT an occlusion: a thin object lying on a plane subtends only grazing directions, so `1 - occlusion(r)` reads a two-to-ten-pixel band there rather than a seam, which is why this exists as its own builtin rather than as a wider radius on that one.  The analogue is Unreal's DistanceToNearestSurface node and Houdini's xyzdist(), not Substance's AO baker.  ITS `radius` IS A WORLD LENGTH -- `proximity(0.002)` is 2 mm -- deliberately UNLIKE the other three, whose radius is a fraction of the hit object's own size: a fraction of the RECEIVER cannot describe how far away a NEIGHBOUR is, and authors already reason in world units for `fw` and feature sizes.  A literal radius <= 0 is a compile error naming that unit; a computed one that lands <= 0 returns the neutral.  TWO THINGS TO KNOW BEFORE AUTHORING WITH IT.  (1) IT CANNOT DRIVE RELIEF: `relief_modifier` holds the signal channel FIXED across its four taps by documented design, so a `proximity`-driven height expression has zero gradient and produces no displacement -- the seam is a colour and roughness signal.  (2) EMISSIVE OBJECTS NEVER COUNT: a `rect_light` panel parked millimetres off a wall must not paint grime on it, so any object whose material emits is skipped -- which also means a DECORATIVE emitter (a lava pool, a glowing rune) will not collect contact dirt either.  `casts_shadows FALSE` does NOT exempt a neighbour (this is geometry presence, not light visibility), a CSG composite's operands never count separately, two INSTANCED COPIES of one geometry do count against each other, and a point INSIDE another object reads 1 (interpenetration is contact).  Unlike the other three it works on EVERY receiver, including ones that publish no signal provider at all (a `box_geometry`, an infinite plane) -- what varies is which NEIGHBOURS can answer: the analytic primitives (plane, sphere, box, capped and open cylinder, disk, torus, and a clipped plane whose four corners are COPLANAR AND CONVEX -- every rect_light and every hand-authored panel) are exact, INDEXED TRIANGLE MESHES are exact too (every loader but RAW, every tessellated primitive, and `displaced_geometry`'s baked mesh -- answered by a bounded closest-point traversal of the mesh's own BVH, identical to a brute-force minimum over every triangle), the ellipsoid and `sdf_geometry` / `skeleton_geometry` are upper bounds (so the signal may UNDER-paint a seam and can never paint one that is not there), CSG COMPOSITES ANSWER TOO (a `union` reports the nearer of its operands; an `intersection` or a `subtraction` brackets the composed field and reports an upper bound), refusing only when an operand refuses -- an `intersection` or `subtraction` with a SHEET operand (a plane, a disk, an open cylinder, a mesh) refuses outright, since it cannot tell inside from outside there -- and RAW (non-indexed) meshes, patches, hair, heightfield-mode SDFs and non-convex or non-coplanar clipped planes contribute nothing and say so ONCE PER REFUSING OBJECT in the log, naming the chunk and its kind.  A MESH IS A SHEET for this query and every solid family is not: a point INSIDE a sphere, box, cylinder or SDF reads 1 (interpenetration is contact), while a point inside a closed MESH reads its honest distance to the nearest triangle -- a triangle soup carries no inside test, and reporting a distance rather than inventing one is the direction that can only under-paint.  HOW LOOSE THE BOUNDS ARE, since it changes the radius you author: an ECCENTRIC ELLIPSOID over-reports by up to its semi-axis ratio -- a 4:1 ellipsoid at a true distance of 2.75 reports 11.0 -- so against one you want roughly ratio-times the radius you actually mean, and an anisotropically SCALED object is bounded the same way (it says so in the log, with its factor).  `interior(radius)` IS THE SIGNED SIBLING, and the two together cover the signed distance without a sign convention to remember: it returns [0,1] with 0 = INSIDE NO NEIGHBOUR and 1 = at least `radius` deep inside one, computed as `depth/radius` for `depth` the deepest containment over every other object.  Its `radius` is a WORLD LENGTH too, and mandatory.  Reach for it when what you want to paint is BURIAL rather than contact -- the sunk shank of a nail, the embedded flank of a stone in mortar.  ONLY THE SOLID FAMILIES CONTRIBUTE TO IT: a sphere, box, capped cylinder, torus, ellipsoid, SDF or CSG composite of those can say whether a point is inside it, while every SHEET -- a plane, a disk, an open cylinder, a TRIANGLE MESH, a patch, hair -- cannot and contributes 0 silently (it would otherwise print a refusal for every mesh in the scene).  So a receiver buried inside a MESH neighbour reads `interior` 0 and `proximity` its honest distance to the nearest triangle.  ALL SIX SIGNALS (curv, occlusion, convexity, thickness, proximity, interior) ARE FULLY CORRECT UNDER PATH TRACING (the default rasterizer family); BDPT / VCM / MLT currently evaluate them as their neutral fallback in PARTS of their transport (the forward walk's own per-bounce re-evaluation, connection/NEE, MIS reverse-pdf, and more -- see docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md's Phase-2 Known-residual paragraph for the full list), which the renderer now surfaces as a one-time warning when such a render begins.  `occlusion` / `convexity` complement `curv`: `curv` is fine-scale and radius-free (a sharp edge), `occlusion` is radius-sampled and sees real cavities (a deep fold two bumps wide), `convexity` is the radius-sampled edge mask whose thresholds port between objects.  A vec3-typed final expr is Rec.709 linear RGB; a scalar-typed expr broadcasts to grayscale.  Spectral rasterizers JH-uplift the evaluated RGB per-sample, same cost class as a texture lookup.  Declare `param <name> <number> [min <a>] [max <b>] [step <s>] [label \"text\"]` constants (repeatable; the min/max/step/label metadata is ignored by evaluation and exists for a future property-panel slider), `def <name> <expr>` named sub-expressions (repeatable, in order), then the final `expr`.  `seed` is auto-registered as a named scalar constant the body can reference for per-instance jitter without a separate param, e.g. `perlin(P + vec3(seed*17.0, seed*31.0, seed*13.0))`.  AUTOMATIC DOMAIN SCALE: fbm/turbulence/ridged fade the octaves the pixel footprint cannot resolve, and you never scale a width by hand -- the compiler differentiates each call's position argument with respect to BOTH P and Po and filters that call at scale_P*fw + scale_Po*fwo, so `fbm(P*40, ...)` fades at 40*fw and `fbm(Po*40, ...)` at 40*fwo.  It resolves anything affine (a scale carried through a `param` or `def` included); a domain warp keeps its affine part's scale; an argument with no provable relation to either position (one built from u/v) simply gets the unscaled fw.  Noise builtins: perlin/fbm/turbulence/ridged/worley_f1/f2/f2f1/id/cellhash/ramp (see ExpressionEval.h); geometry-signal builtins: occlusion(radius)/convexity(radius)/thickness(radius)/proximity(radius)/interior(radius); vec3 ops dot/cross/length/normalize/.x/.y/.z.  Registered ONLY as a colour painter -- NOT usable as a displaced_geometry `function` or any other IFunction2D slot (this is a 3D-context surface; use expression_function2d for a UV-only displacement/mask field).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";  p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "param"; p.kind = ValueKind::String; p.repeatable = true; p.description = "Named numeric constant `<name> <number> [min <a>] [max <b>] [step <s>] [label \"text\"]` (repeatable); visible to every def and the final expr"; }
						{ auto& p = P(); p.name = "def";   p.kind = ValueKind::String; p.repeatable = true; p.description = "Named sub-expression `<name> <expr>` (repeatable, in order); a let-binding referencing u, v, P, Po, N, fw, fwo, time, curv, curvR, the occlusion()/convexity()/thickness()/proximity()/interior() builtins, params, and earlier defs"; }
						{ auto& p = P(); p.name = "expr";  p.kind = ValueKind::String; p.required = true; p.description = "The final value expression (vec3 -> RGB colour; scalar -> grayscale broadcast)"; }
						{ auto& p = P(); p.name = "seed";  p.kind = ValueKind::Double; p.description = "Auto-registered named scalar constant `seed`, for deterministic per-instance variation"; p.defaultValueHint = "0.0"; }
						{ auto& p = P(); p.name = "time";  p.kind = ValueKind::Double; p.description = "Initial `time` value (keyframeable at the scene level, like gerstnerwave_painter's `time`)"; p.defaultValueHint = "0.0"; }
						return cd;
					}();
					return d;
				}
			};

			// ramp_painter -- the universal scalar -> colour remap (doc 88
			// P2.2, S3).  `input`'s channel at the hit drives `t`, which is
			// clamped to the authored stop range and interpolated between
			// the bracketing pair of `stop` lines per `interpolation`.
			struct RampPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					std::string input = bag.GetString( "input", "" );
					if( input.empty() ) {
						GlobalLog()->PrintEx( eLog_Error,
							"ramp_painter `%s`: missing `input` (the painter whose channel drives t)",
							name.c_str() );
						return false;
					}

					unsigned int channel = 0;	// R default
					if( bag.Has( "channel" ) ) {
						const std::string chs = bag.GetString( "channel" );
						if(      chs == "R" ) channel = 0;
						else if( chs == "G" ) channel = 1;
						else if( chs == "B" ) channel = 2;
						else if( chs == "A" ) channel = 3;
						else {
							GlobalLog()->PrintEx( eLog_Error,
								"ramp_painter `%s`: unknown channel `%s` (expected R, G, B, or A)",
								name.c_str(), chs.c_str() );
							return false;
						}
					}

					unsigned int interpolation = 0;	// linear default
					if( bag.Has( "interpolation" ) ) {
						const std::string interps = bag.GetString( "interpolation" );
						if(      interps == "linear" )   interpolation = 0;
						else if( interps == "constant" ) interpolation = 1;
						else if( interps == "smooth" )   interpolation = 2;
						else {
							GlobalLog()->PrintEx( eLog_Error,
								"ramp_painter `%s`: unknown interpolation `%s` (expected linear, constant, or smooth)",
								name.c_str(), interps.c_str() );
							return false;
						}
					}

					const std::vector<std::string>& stopLines = bag.GetRepeatable( "stop" );
					if( stopLines.size() < 2 ) {
						GlobalLog()->PrintEx( eLog_Error,
							"ramp_painter `%s`: needs at least 2 `stop` lines (got %u)",
							name.c_str(), (unsigned int)stopLines.size() );
						return false;
					}

					std::vector<double> positions;
					std::vector<double> colors;
					positions.reserve( stopLines.size() );
					colors.reserve( stopLines.size() * 3 );
					for( std::size_t i = 0; i < stopLines.size(); ++i ) {
						double pos = 0, r = 0, g = 0, b = 0;
						if( sscanf( stopLines[i].c_str(), "%lf %lf %lf %lf", &pos, &r, &g, &b ) != 4 ) {
							GlobalLog()->PrintEx( eLog_Error,
								"ramp_painter `%s`: stop %u (`%s`) must be `<pos> <r> <g> <b>`",
								name.c_str(), (unsigned int)i, stopLines[i].c_str() );
							return false;
						}
						if( !Implementation::ExpressionProgram::IsFinite( (Scalar)pos ) ||
							!Implementation::ExpressionProgram::IsFinite( (Scalar)r ) ||
							!Implementation::ExpressionProgram::IsFinite( (Scalar)g ) ||
							!Implementation::ExpressionProgram::IsFinite( (Scalar)b ) ) {
							GlobalLog()->PrintEx( eLog_Error,
								"ramp_painter `%s`: stop %u must be finite (nan/inf rejected)",
								name.c_str(), (unsigned int)i );
							return false;
						}
						if( i > 0 && pos < positions.back() ) {
							GlobalLog()->PrintEx( eLog_Error,
								"ramp_painter `%s`: stop %u position %g is less than stop %u position %g -- stop positions must be non-decreasing",
								name.c_str(), (unsigned int)i, pos, (unsigned int)(i - 1), positions.back() );
							return false;
						}
						positions.push_back( pos );
						colors.push_back( r ); colors.push_back( g ); colors.push_back( b );
					}

					std::string colorSpace = bag.GetString( "color_space", "Rec709RGB_Linear" );

					return pJob.AddRampPainter(
						name.c_str(), input.c_str(), channel, interpolation,
						&positions[0], &colors[0], (unsigned int)positions.size(),
						colorSpace.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "ramp_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "The universal scalar -> COLOUR remap (doc 88 P2.2) -- multi-stop colour ramp driven by `input`'s channel at each hit.  `input` can be ANY colour painter (a perlin3d_painter, worley3d_painter, expression_painter fbm field, etc.); its selected `channel` (R/G/B, or A via GetAlpha) supplies `t`, clamped to [first stop pos, last stop pos], then interpolated between the bracketing pair of `stop <pos> <r> <g> <b>` lines (>= 2 required, positions non-decreasing) per `interpolation`: `linear` (lerp), `constant` (step -- holds the lower stop's exact colour until the next stop's position), or `smooth` (smoothstep-eased lerp).  Every mode returns the exact authored stop colour AT that stop's position.  Stop colours are eagerly JH-uplifted once at construction (`color_space`-aware, like uniformcolor_painter) -- no per-sample uplift.  The composition-boundary idiom from doc 88 P5: put the FIELD in an expression/noise painter, put the COLOUR in the ramp -- e.g. `expression_painter` computing an fbm height field -> `ramp_painter` colourizing it deep-blue/sand/grass/snow for a terrain material.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";          p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "input";         p.kind = ValueKind::Reference;  p.required = true; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Named colour painter whose channel drives t (ANY painter kind)"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "channel";       p.kind = ValueKind::Enum;       p.enumValues = {"R","G","B","A"}; p.description = "Which channel of `input` sources t (A reads GetAlpha)"; p.defaultValueHint = "R"; }
						{ auto& p = P(); p.name = "interpolation"; p.kind = ValueKind::Enum;       p.enumValues = {"linear","constant","smooth"}; p.description = "Blend shape between bracketing stops"; p.defaultValueHint = "linear"; }
						{ auto& p = P(); p.name = "stop";          p.kind = ValueKind::String;     p.repeatable = true; p.description = "Colour stop `<pos> <r> <g> <b>` (repeatable, in order; positions must be non-decreasing; at least 2 required)"; }
						{ auto& p = P(); p.name = "color_space";   p.kind = ValueKind::Enum;       p.enumValues = {"sRGB","Rec709RGB_Linear","ROMMRGB_Linear","ProPhotoRGB"}; p.description = "Interpretation of each stop's r g b (linear default; same value set as uniformcolor_painter's `colorspace`)"; p.defaultValueHint = "Rec709RGB_Linear"; }
						return cd;
					}();
					return d;
				}
			};

			// mapping_painter -- the author-facing scale/rotate/translate/
			// reproject wrapper (doc 88 P2.3).  See MappingPainter.h for the
			// full design rationale; NOT the glTF KHR_texture_transform
			// bridge (UVTransformPainter stays that).
			struct MappingPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name   = bag.GetString( "name", "noname" );
					std::string source = bag.GetString( "source", "" );
					if( source.empty() ) {
						GlobalLog()->PrintEx( eLog_Error,
							"mapping_painter `%s`: missing `source` (the painter whose domain is transformed)",
							name.c_str() );
						return false;
					}

					unsigned int projection = 0;	// uv default
					if( bag.Has( "projection" ) ) {
						const std::string projStr = bag.GetString( "projection" );
						if(      projStr == "uv" )        projection = 0;
						else if( projStr == "world" )     projection = 1;
						else if( projStr == "object" )    projection = 2;
						else if( projStr == "triplanar" ) projection = 3;
						else {
							GlobalLog()->PrintEx( eLog_Error,
								"mapping_painter `%s`: unknown projection `%s` (expected uv, world, object, or triplanar)",
								name.c_str(), projStr.c_str() );
							return false;
						}
					}

					double scale[3] = { 1.0, 1.0, 1.0 };
					bag.GetVec3( "scale", scale );	// leaves the 1,1,1 default untouched if absent

					double rotateDeg[3] = { 0.0, 0.0, 0.0 };
					bag.GetVec3( "rotate", rotateDeg );

					double translate[3] = { 0.0, 0.0, 0.0 };
					bag.GetVec3( "translate", translate );

					const double blendSharpness = bag.GetDouble( "blend_sharpness", 4.0 );

					return pJob.AddMappingPainter( name.c_str(), source.c_str(), projection,
						scale, rotateDeg, translate, blendSharpness );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "mapping_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Wraps `source` and transforms the DOMAIN it is evaluated at before delegating (doc 88 P2.3) -- the author-facing scale/rotate/translate/reproject tool (NOT the glTF KHR_texture_transform bridge; that stays UVTransformPainter, importer-only).  `projection uv` (default) transforms the surface UV (`ptCoord`) -- 2D semantics: only `scale.x/y`, `rotate.z`, and `translate.x/y` apply, `scale.z`/`rotate.x`/`rotate.y`/`translate.z` are IGNORED.  `projection world`/`object` transform the WORLD (`ptIntersection`) / OBJECT (`ptObjIntersec`) position respectively before delegating to a 3D-domain source (perlin3d, worley3d, voronoi3d, ...) -- wrapping a UV-domain source in world/object is a silent no-op, since that source never reads the field being transformed.  `projection triplanar` is for a UV-CONSUMING source (an image painter, checker_painter, ...) on UV-LESS geometry: it samples `source` three times with ptCoord derived from (y,z)/(x,z)/(x,y) of the TRS-transformed WORLD position, blended by |N.axis|^blend_sharpness (world shading normal, normalized) -- always world position/normal, no object-space triplanar variant; the blend normal itself is used AS-IS, not rotated by `rotate` (axis dominance is a geometric fact about the surface, not part of the retiling -- rotating it too would double-apply the rotation on curved geometry).  TRS composes as scale, then rotate (Z then Y then X, matching standard_object's `orientation`), then translate.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";             p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "source";           p.kind = ValueKind::Reference;  p.required = true; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Named source painter whose domain is transformed"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "projection";       p.kind = ValueKind::Enum;       p.enumValues = {"uv","world","object","triplanar"}; p.description = "Which domain field to transform: uv (ptCoord), world (ptIntersection), object (ptObjIntersec), or triplanar (three ptCoord samples blended by normal)"; p.defaultValueHint = "uv"; }
						{ auto& p = P(); p.name = "scale";            p.kind = ValueKind::DoubleVec3; p.description = "Per-axis scale, applied FIRST.  uv projection uses x/y only"; p.defaultValueHint = "1 1 1"; }
						{ auto& p = P(); p.name = "rotate";           p.kind = ValueKind::DoubleVec3; p.description = "Per-axis rotation in DEGREES, applied SECOND (Z then Y then X, matching standard_object orientation).  uv projection uses z only"; p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "translate";        p.kind = ValueKind::DoubleVec3; p.description = "Per-axis translation, applied LAST.  uv projection uses x/y only"; p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "blend_sharpness";  p.kind = ValueKind::Double;     p.description = "Triplanar-only: exponent on |N.axis| before normalizing the three per-axis blend weights.  Higher = sharper axis-aligned transitions"; p.defaultValueHint = "4.0"; }
						return cd;
					}();
					return d;
				}
			};

			// stochastic_tile_painter -- hex-tiling with histogram-
			// preserving blending over `source` (doc 88 P3.1, S8).  See
			// StochasticTilePainter.h for the full algorithm (Heitz &
			// Neyret 2018; Burley JCGT 2019; Mikkelsen JCGT 2022 for the
			// blend_gamma default).
			struct StochasticTilePainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					std::string source = bag.GetString( "source", "" );
					if( source.empty() ) {
						GlobalLog()->PrintEx( eLog_Error,
							"stochastic_tile_painter `%s`: missing `source` (the painter to tile)",
							name.c_str() );
						return false;
					}

					const double tileScale = bag.GetDouble( "tile_scale", 4.0 );
					if( !( tileScale > 0.0 ) || !Implementation::ExpressionProgram::IsFinite( (Scalar)tileScale ) ) {
						GlobalLog()->PrintEx( eLog_Error,
							"stochastic_tile_painter `%s`: tile_scale %g must be finite and > 0",
							name.c_str(), tileScale );
						return false;
					}

					const unsigned int seed = bag.GetUInt( "seed", 0 );

					double mean[3] = { 0.5, 0.5, 0.5 };
					bag.GetVec3( "mean", mean );
					for( int k = 0; k < 3; ++k ) {
						if( !Implementation::ExpressionProgram::IsFinite( (Scalar)mean[k] ) ) {
							GlobalLog()->PrintEx( eLog_Error,
								"stochastic_tile_painter `%s`: mean must be finite (nan/inf rejected)",
								name.c_str() );
							return false;
						}
					}

					const double blendGamma = bag.GetDouble( "blend_gamma", 7.0 );
					if( !Implementation::ExpressionProgram::IsFinite( (Scalar)blendGamma ) || blendGamma < 0.0 || blendGamma > 64.0 ) {
						GlobalLog()->PrintEx( eLog_Error,
							"stochastic_tile_painter `%s`: blend_gamma %g must be in [0, 64] (negative would divide-by-zero on a zero barycentric weight; anything past the literature's range buys nothing but underflow risk)",
							name.c_str(), blendGamma );
						return false;
					}

					std::string colorSpace = bag.GetString( "color_space", "Rec709RGB_Linear" );

					return pJob.AddStochasticTilePainter( name.c_str(), source.c_str(), tileScale, seed, mean, blendGamma, colorSpace.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "stochastic_tile_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Hex-tiling with histogram-preserving blending over `source` (doc 88 P3.1) -- breaks up the visible tile-grid repetition of a photographic source (one photo of bark/plaster/rust -> unbounded non-repeating cover) with no extra authored content (Heitz & Neyret 2018; Burley, JCGT 2019).  Partitions `ptCoord * tile_scale` into a triangular lattice; each of the 3 lattice vertices bounding the query point gets a deterministic (`seed`-derived) random UV offset, `source` is sampled at all three, and the samples are combined with sharpened barycentric weights (`blend_gamma`) via the variance-preserving formula out = mean + sum(w_i*(x_i-mean))/sqrt(sum(w_i^2)) -- this restores the source's original value spread, which a plain weighted average would blur toward `mean`.  `mean` is AUTHOR-SUPPLIED (not estimated from `source` -- painters have no statistics prepass in this codebase); pick it to match the source image/painter's actual average value for the sharpest restoration.  UV domain only (`ptCoord`) -- wrap in an outer `mapping_painter { projection triplanar }` for world-space / UV-less tiling rather than a separate projection parameter here.  GetAlpha uses the same sharpened weights but WITHOUT the variance restore (alpha is coverage, not a histogram).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "source";      p.kind = ValueKind::Reference;  p.required = true; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Named painter to tile (typically an image painter)"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "tile_scale";  p.kind = ValueKind::Double;     p.description = "Lattice density -- larger means smaller, more numerous tiles"; p.defaultValueHint = "4.0"; }
						{ auto& p = P(); p.name = "seed";        p.kind = ValueKind::UInt;       p.description = "Hash seed for per-vertex UV offsets"; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "mean";        p.kind = ValueKind::DoubleVec3; p.description = "Source's mean value (r,g,b) -- author-supplied, used by the variance-preserving blend"; p.defaultValueHint = "0.5 0.5 0.5"; }
						{ auto& p = P(); p.name = "blend_gamma"; p.kind = ValueKind::Double;     p.description = "Barycentric-weight sharpening exponent (higher = crisper triangle seams); [0, 64]"; p.defaultValueHint = "7.0"; }
						{ auto& p = P(); p.name = "color_space"; p.kind = ValueKind::Enum;       p.enumValues = {"sRGB","Rec709RGB_Linear","ROMMRGB_Linear","ProPhotoRGB"}; p.description = "Interpretation of `mean`'s r g b"; p.defaultValueHint = "Rec709RGB_Linear"; }
						return cd;
					}();
					return d;
				}
			};

			// scatter_painter -- texture-bombing / FX-map-lite (doc 88
			// P3.2, S8).  See ScatterPainter.h for the full algorithm and
			// the neighbourhood-reach proof behind the stamp_scale /
			// jitter_scale bound enforced below.
			struct ScatterPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					std::string source = bag.GetString( "source", "" );
					if( source.empty() ) {
						GlobalLog()->PrintEx( eLog_Error,
							"scatter_painter `%s`: missing `source` (the stamp painter)",
							name.c_str() );
						return false;
					}
					std::string background = bag.GetString( "background", "" );
					if( background.empty() ) {
						GlobalLog()->PrintEx( eLog_Error,
							"scatter_painter `%s`: missing `background`",
							name.c_str() );
						return false;
					}

					const double cellScale = bag.GetDouble( "cell_scale", 4.0 );
					if( !( cellScale > 0.0 ) || !Implementation::ExpressionProgram::IsFinite( (Scalar)cellScale ) ) {
						GlobalLog()->PrintEx( eLog_Error,
							"scatter_painter `%s`: cell_scale %g must be finite and > 0",
							name.c_str(), cellScale );
						return false;
					}

					const double stampScale = bag.GetDouble( "stamp_scale", 0.7 );
					if( !( stampScale > 0.0 ) || !Implementation::ExpressionProgram::IsFinite( (Scalar)stampScale ) ) {
						GlobalLog()->PrintEx( eLog_Error,
							"scatter_painter `%s`: stamp_scale %g must be finite and > 0",
							name.c_str(), stampScale );
						return false;
					}

					const double jitterPosition = bag.GetDouble( "jitter_position", 0.5 );
					if( jitterPosition < 0.0 || jitterPosition > 1.0 ) {
						GlobalLog()->PrintEx( eLog_Error,
							"scatter_painter `%s`: jitter_position %g must be in [0, 1]",
							name.c_str(), jitterPosition );
						return false;
					}

					const double jitterRotationDeg = bag.GetDouble( "jitter_rotation", 15.0 );
					if( jitterRotationDeg < 0.0 || jitterRotationDeg > 360.0 || !Implementation::ExpressionProgram::IsFinite( (Scalar)jitterRotationDeg ) ) {
						GlobalLog()->PrintEx( eLog_Error,
							"scatter_painter `%s`: jitter_rotation %g must be in [0, 360] degrees",
							name.c_str(), jitterRotationDeg );
						return false;
					}

					const double jitterScale = bag.GetDouble( "jitter_scale", 0.2 );
					if( jitterScale < 0.0 || jitterScale >= 1.0 ) {
						GlobalLog()->PrintEx( eLog_Error,
							"scatter_painter `%s`: jitter_scale %g must be in [0, 1)",
							name.c_str(), jitterScale );
						return false;
					}

					const double probability = bag.GetDouble( "probability", 1.0 );
					if( probability < 0.0 || probability > 1.0 ) {
						GlobalLog()->PrintEx( eLog_Error,
							"scatter_painter `%s`: probability %g must be in [0, 1]",
							name.c_str(), probability );
						return false;
					}

					// Neighbourhood-reach bound (see ScatterPainter.h file
					// header point 3): the 3x3 neighbourhood search is only
					// provably sufficient when stamp_scale*(1+jitter_scale)
					// <= sqrt(2).  Reject rather than silently clamp -- a
					// silently-shrunk stamp would render smaller than
					// authored with no visible diagnostic.
					const double reach = stampScale * ( 1.0 + jitterScale );
					const double kMaxReach = 1.4142135623730951;	// sqrt(2)
					if( reach > kMaxReach ) {
						GlobalLog()->PrintEx( eLog_Error,
							"scatter_painter `%s`: stamp_scale * (1 + jitter_scale) = %g exceeds the 3x3-neighbourhood-search bound of sqrt(2) (%g) -- the stamp could extend past the immediately adjacent cell and go unfound; reduce stamp_scale or jitter_scale",
							name.c_str(), reach, kMaxReach );
						return false;
					}

					const unsigned int seed = bag.GetUInt( "seed", 0 );

					return pJob.AddScatterPainter( name.c_str(), source.c_str(), background.c_str(),
						cellScale, stampScale, jitterPosition, jitterRotationDeg, jitterScale, probability, seed );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "scatter_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Texture-bombing / FX-map-lite (doc 88 P3.2): stamps `source` over `background` on a jittered square lattice -- the discrete-element richness (rivets, leaves, scratches, stains, screws) that noise cannot produce.  Each lattice cell may host one stamp instance (per-cell `probability` roll), jittered in position (`jitter_position`, stays within its owning cell), rotation (`jitter_rotation`, +/- degrees), and size (`stamp_scale` +/- `jitter_scale` relative).  A 3x3-cell neighbourhood search finds stamps that cross into an adjacent cell; among covering candidates the nearest stamp CENTER wins.  The winning stamp composites over `background` via `source`'s own alpha (Porter-Duff over) -- an RGBA stamp with alpha 0 at that texel shows background, the standard texture-bombing cutout idiom.  All jitter is deterministic (`seed`-hashed per cell), never random-per-render.  UV domain only; wrap in an outer `mapping_painter { projection triplanar }` for world-space scattering.  `stamp_scale * (1 + jitter_scale)` is capped at sqrt(2) (~1.414) -- the provable bound under which the 3x3 neighbourhood search can never miss a stamp; the parser rejects chunks that exceed it.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";               p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "source";             p.kind = ValueKind::Reference;  p.required = true; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Named stamp painter, sampled in its own local [0,1]^2 frame"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "background";         p.kind = ValueKind::Reference;  p.required = true; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Named background painter, shown outside every stamp"; p.semantics.pipe = ParameterPipe::Color; }
						{ auto& p = P(); p.name = "cell_scale";         p.kind = ValueKind::Double;     p.description = "Lattice density -- larger means smaller, more numerous cells"; p.defaultValueHint = "4.0"; }
						{ auto& p = P(); p.name = "stamp_scale";        p.kind = ValueKind::Double;     p.description = "Base stamp size within a cell, as a fraction of the cell width"; p.defaultValueHint = "0.7"; }
						{ auto& p = P(); p.name = "jitter_position";    p.kind = ValueKind::Double;     p.description = "Position jitter, [0, 1] (1 = center may reach the cell edge)"; p.defaultValueHint = "0.5"; }
						{ auto& p = P(); p.name = "jitter_rotation";    p.kind = ValueKind::Double;     p.description = "Max rotation jitter, +/- degrees, [0, 360]"; p.defaultValueHint = "15.0"; }
						{ auto& p = P(); p.name = "jitter_scale";       p.kind = ValueKind::Double;     p.description = "Relative +/- size jitter, [0, 1)"; p.defaultValueHint = "0.2"; }
						{ auto& p = P(); p.name = "probability";        p.kind = ValueKind::Double;     p.description = "Per-cell occupancy probability, [0, 1] (0 = no stamps, 1 = every cell occupied)"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "seed";               p.kind = ValueKind::UInt;       p.description = "Hash seed for all per-cell jitter draws"; p.defaultValueHint = "0"; }
						return cd;
					}();
					return d;
				}
			};

			struct Function2DColorPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					std::string func = bag.GetString( "function2d", "" );
					if( func.empty() ) {
						GlobalLog()->PrintEx( eLog_Error,
							"function2d_painter `%s`: missing `function2d` (the named IFunction2D to wrap)", name.c_str() );
						return false;
					}
					const double scale = bag.GetDouble( "scale", 1.0 );
					const double bias  = bag.GetDouble( "bias",  0.0 );
					return pJob.AddFunction2DColorPainter( name.c_str(), func.c_str(), scale, bias );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "function2d_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Wraps a named IFunction2D as a greyscale COLOUR painter (out = bias + scale * f(u,v) on all channels).  The colour analogue of scalar_painter { function2d }: lets any procedural 2D field (Perlin, Worley, polynomial, composite, guilloché) feed a colour slot or a blend_painter mask -- e.g. the guilloché spall mask driving the matte oxide-scale blend.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";       p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "function2d"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter, ChunkCategory::Function}; p.description = "Named IFunction2D to wrap as greyscale colour -- resolved via pFunc2DManager (Job::AddFunction2DColorPainter), which holds `function`-category IFunction2D chunks (piecewise_linear_function2d) PLUS every dual-registered colour painter except expression_painter/scalar_painter, hence the {Painter, Function} pair"; p.semantics.pipe = ParameterPipe::Function2D; }
						{ auto& p = P(); p.name = "scale";      p.kind = ValueKind::Double;    p.description = "Output scale"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "bias";       p.kind = ValueKind::Double;    p.description = "Output bias (out = bias + scale * f)"; p.defaultValueHint = "0.0"; }
						return cd;
					}();
					return d;
				}
			};

			struct SweepGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				//! Expand ONE profile-source family -- the point / circle / rect
				//! trio -- into a flat CCW (x, h) point list.  Both the primary
				//! `profile_*` slot and the loft's `profile2_*` slot go through
				//! this ONE function (doc 89 slice A), so the three forms cannot
				//! drift apart between the two slots, and the factory only ever
				//! sees plain point lists -- which is what collapses the nine
				//! profile x profile2 authoring combinations to a single morph
				//! code path.  `required` FALSE makes an unauthored family a
				//! no-op (`present` FALSE) instead of an error.
				static bool ExpandProfileFamily(
						const ParseStateBag& bag, const std::string& name,
						const char* pointKey, const char* circleKey, const char* rectKey,
						const char* label, const bool required,
						std::vector<double>& prof, bool& present )
				{
					present = false;
					// A profile family is authored EXACTLY one of three ways:
					// repeatable point lines (hand-authored polygon), OR the
					// circle convenience (regular n-gon), OR the rect
					// convenience (box, optionally rounded) -- named here by
					// pointKey / circleKey / rectKey so this one body serves
					// both the `profile_*` and the `profile2_*` family.
					const std::vector<std::string>& profLines = bag.GetRepeatable( pointKey );
					const bool hasCircle = bag.Has( circleKey );
					const bool hasRect   = bag.Has( rectKey );
					const int profileFormCount = ( profLines.empty() ? 0 : 1 ) + ( hasCircle ? 1 : 0 ) + ( hasRect ? 1 : 0 );
					present = ( profileFormCount > 0 );
					if( profileFormCount == 0 && !required ) {
						return true;	// this family is OPTIONAL and was not authored
					}
					if( profileFormCount == 0 ) {
						return SweepReject(
							"sweep_geometry `%s`: missing %s -- supply repeatable `%s <x> <h>` lines, OR `%s <r> [n]`, OR `%s <w> <h> [r]`",
							name.c_str(), label, pointKey, circleKey, rectKey );
					}
					if( profileFormCount > 1 ) {
						return SweepReject(
							"sweep_geometry `%s`: %s, %s, and %s are mutually exclusive -- author exactly ONE %s source",
							name.c_str(), pointKey, circleKey, rectKey, label );
					}

					prof.clear();
					if( !profLines.empty() ) {
						if( profLines.size() < 3 ) {
							return SweepReject(
								"sweep_geometry `%s`: need at least 3 repeatable `%s <x> <h>` entries (a closed polygon; got %u)",
								name.c_str(), pointKey, (unsigned int)profLines.size() );
						}
						prof.reserve( profLines.size() * 2 );
						for( std::size_t i = 0; i < profLines.size(); ++i ) {
							// TEXT-validate first, exactly as every sibling
							// branch here (profile_circle / profile_rect) and
							// lathe_geometry's own profile_point loop already
							// do.  A bare sscanf conversion count is NOT that
							// gate: sscanf happily converts `nan` and `inf`,
							// so `profile_point inf 0` returned 2 and sailed
							// through -- and a non-finite profile coordinate
							// poisons the shoelace area, the arc-length
							// parameters and every vertex, while each
							// downstream gate is a COMPARISON that a NaN
							// silently passes.
							int nTok = 0;
							if( !AllTokensAreFiniteNumbers( profLines[i].c_str(), &nTok ) || nTok != 2 ) {
								return SweepReject(
									"sweep_geometry `%s`: %s %u (`%s`) must be exactly two FINITE numbers `<x> <h>` "
									"(no trailing garbage, no nan/inf)",
									name.c_str(), pointKey, (unsigned int)i, profLines[i].c_str() );
							}
							double x = 0, h = 0;
							sscanf( profLines[i].c_str(), "%lf %lf", &x, &h );
							prof.push_back( x );
							prof.push_back( h );
						}
					} else if( hasCircle ) {
						// profile_circle <r> [n]: a regular CCW n-gon of radius r
						// centred at the profile origin -- the same winding /
						// (x,h)=(r cos, r sin) convention a hand-authored circular
						// profile_point ring already uses elsewhere in the codebase.
						const std::string cs = bag.GetString( circleKey, "" );
						// profile_circle has OPTIONAL arity (`<r>` or `<r> <n>`), so a
						// bare sscanf conversion count can't tell "n legitimately
						// omitted" from "garbage glued onto r" (`1.0abc` stops %lf at
						// `1.0`, leaving `abc` for the next %lf to fail on -- sscanf
						// halts there instead of erroring, silently dropping the
						// glued text).  AllTokensAreFiniteNumbers TEXT-validates the
						// whole string first: exact whitespace-separated token count,
						// no trailing garbage, no nan/inf spellings.
						int nTok = 0;
						if( !AllTokensAreFiniteNumbers( cs.c_str(), &nTok ) || nTok < 1 || nTok > 2 ) {
							return SweepReject(
								"sweep_geometry `%s`: %s `%s` must be `<r>` or `<r> <n>` (finite numbers, no trailing garbage)",
								name.c_str(), circleKey, cs.c_str() );
						}
						double r = 0, nD = 24;
						const int nf = sscanf( cs.c_str(), "%lf %lf", &r, &nD );
						if( !( r > 0 ) ) {
							return SweepReject(
								"sweep_geometry `%s`: %s radius (%g) must be > 0", name.c_str(), circleKey, r );
						}
						// nD is already finite (AllTokensAreFiniteNumbers above), but an
						// absurdly large magnitude is still UB to (int)-cast -- reject
						// outright rather than clamp-and-warn like the in-range case.
						if( nf == 2 && fabs( nD ) >= 1e9 ) {
							return SweepReject(
								"sweep_geometry `%s`: %s n (%g) is absurdly out of range", name.c_str(), circleKey, nD );
						}
						int n = ( nf == 2 ) ? (int)( nD + 0.5 ) : 24;
						if( n < 3 || n > 512 ) {
							const int clamped = n < 3 ? 3 : 512;
							GlobalLog()->PrintEx( eLog_Warning,
								"sweep_geometry `%s`: %s n (%d) clamped to %d", name.c_str(), circleKey, n, clamped );
							n = clamped;
						}
						prof.reserve( (std::size_t)n * 2 );
						for( int k = 0; k < n; ++k ) {
							const double a = TWO_PI * k / n;
							prof.push_back( r * cos( a ) );
							prof.push_back( r * sin( a ) );
						}
					} else {
						// profile_rect <w> <h> [r]: a CCW box of width w (binormal
						// axis) and height h (normal axis) centred at the profile
						// origin, with an optional corner radius r.  r > 0 rounds
						// each corner with a 5-point (4-interval) quarter-circle arc;
						// the four corners are emitted in CCW order BR -> TR -> TL ->
						// BL, matching the sharp box's corner order at r == 0.
						const std::string rs = bag.GetString( rectKey, "" );
						// Same optional-arity trailing-garbage trap as profile_circle
						// above (`2 2extra` -> w=2, h=2, the "extra" silently dropped
						// because the 3rd %lf's parse failure halts sscanf before it
						// ever reaches a trailing %s catch-all) -- TEXT-validate first.
						int nTok = 0;
						if( !AllTokensAreFiniteNumbers( rs.c_str(), &nTok ) || nTok < 2 || nTok > 3 ) {
							return SweepReject(
								"sweep_geometry `%s`: %s `%s` must be `<w> <h>` or `<w> <h> <r>` (finite numbers, no trailing garbage)",
								name.c_str(), rectKey, rs.c_str() );
						}
						double w = 0, h = 0, r = 0;
						const int nf = sscanf( rs.c_str(), "%lf %lf %lf", &w, &h, &r );
						if( nf == 2 ) { r = 0; }
						if( !( w > 0 ) || !( h > 0 ) ) {
							return SweepReject(
								"sweep_geometry `%s`: %s width/height (%g, %g) must both be > 0", name.c_str(), rectKey, w, h );
						}
						const double halfMin = ( w < h ? w : h ) * 0.5;
						// C2 fix round (comment corrected 2026-08-14): a NaN corner
						// radius (e.g. `profile_rect 2 2 nan`) is actually rejected
						// ABOVE by AllTokensAreFiniteNumbers -- the TEXT-layer gate,
						// since "nan" is not a finite-number token, never lets a NaN
						// literal reach this line at all.  This negated-idiom range
						// check (`!( r >= 0.0 && r <= halfMin )`, vs. the naive
						// `r < 0 || r > halfMin` which passes a NaN r through both
						// branches) is DEFENSE IN DEPTH, not the primary guard.
						if( !( r >= 0.0 && r <= halfMin ) ) {
							return SweepReject(
								"sweep_geometry `%s`: %s corner radius (%g) must be in [0, min(w,h)/2] = [0, %g]",
								name.c_str(), rectKey, r, halfMin );
						}
						const double hw = w * 0.5, hh = h * 0.5;
						if( r <= 0 ) {
							prof.push_back(  hw ); prof.push_back( -hh );
							prof.push_back(  hw ); prof.push_back(  hh );
							prof.push_back( -hw ); prof.push_back(  hh );
							prof.push_back( -hw ); prof.push_back( -hh );
						} else {
							const double cx[4] = {  hw-r,  hw-r, -(hw-r), -(hw-r) };
							const double cy[4] = { -(hh-r), hh-r,  hh-r, -(hh-r) };
							const double startDeg[4] = { -90, 0, 90, 180 };
							prof.reserve( 4 * 5 * 2 );
							for( int c = 0; c < 4; ++c ) {
								for( int k = 0; k <= 4; ++k ) {
									const double a = ( startDeg[c] + 22.5 * k ) * DEG_TO_RAD;
									prof.push_back( cx[c] + r * cos( a ) );
									prof.push_back( cy[c] + r * sin( a ) );
								}
							}
							// A corner arc's own closing point (its true 90-degree end,
							// computed off ITS OWN centre) coincides EXACTLY with the
							// following corner's opening point (computed off THAT
							// corner's own, different, centre) only when the straight
							// edge between them has collapsed to zero length -- which
							// happens per-SIDE, not globally: at r == min(w,h)/2 the two
							// sides along the SHORTER dimension collapse (a capsule),
							// and if w == h too (a full circle from 4 quarter arcs
							// sharing one centre) all four sides collapse.  A blanket
							// "drop every corner's last sample" would also cut the real,
							// still-finite sides on a capsule (e.g. profile_rect 4 2 1),
							// so dedup CONDITIONALLY: drop a sample only when it is
							// actually coincident with its predecessor, checked cyclically
							// so the wrap edge (corner 3 -> corner 0) is covered too.
							const double eps = 1e-9 * ( ( hw > hh ? hw : hh ) + r );
							std::vector<double> deduped;
							deduped.reserve( prof.size() );
							for( std::size_t k = 0; k < prof.size(); k += 2 ) {
								const double x = prof[k], y = prof[k+1];
								if( !deduped.empty() ) {
									const double px = deduped[ deduped.size() - 2 ];
									const double py = deduped[ deduped.size() - 1 ];
									if( fabs( x - px ) < eps && fabs( y - py ) < eps ) {
										continue;
									}
								}
								deduped.push_back( x );
								deduped.push_back( y );
							}
							if( deduped.size() >= 4 ) {
								const double x0 = deduped[0], y0 = deduped[1];
								const double xn = deduped[ deduped.size() - 2 ], yn = deduped[ deduped.size() - 1 ];
								if( fabs( x0 - xn ) < eps && fabs( y0 - yn ) < eps ) {
									deduped.pop_back();
									deduped.pop_back();
								}
							}
							prof.swap( deduped );
						}
					}
					return true;
				}

				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );

					// The profile is authored EXACTLY one of three ways: repeatable
					// `profile_point` lines (hand-authored polygon), OR the
					// `profile_circle` convenience (regular n-gon), OR the
					// `profile_rect` convenience (box, optionally rounded).  The
					// OPTIONAL second profile (`profile2_*`, doc 89 slice A) offers
					// the identical trio with the identical mutual exclusion, and
					// runs through the same expander.
					std::vector<double> prof;
					bool haveProfile = false;
					if( !ExpandProfileFamily( bag, name, "profile_point", "profile_circle", "profile_rect",
							"profile", true, prof, haveProfile ) ) {
						return false;
					}
					std::vector<double> prof2;
					bool haveProfile2 = false;
					if( !ExpandProfileFamily( bag, name, "profile2_point", "profile2_circle", "profile2_rect",
							"second profile", false, prof2, haveProfile2 ) ) {
						return false;
					}

					const std::vector<std::string>& pointLines = bag.GetRepeatable( "point" );
					if( pointLines.size() < 2 ) {
						return SweepReject(
							"sweep_geometry `%s`: need at least 2 repeatable `point <x> <y> <z>` path control points (got %u)",
							name.c_str(), (unsigned int)pointLines.size() );
					}
					std::vector<double> pts;
					pts.reserve( pointLines.size() * 3 );
					for( std::size_t i = 0; i < pointLines.size(); ++i ) {
						double x = 0, y = 0, z = 0;
						char trailing[8] = {0};
						if( sscanf( pointLines[i].c_str(), "%lf %lf %lf %7s", &x, &y, &z, trailing ) != 3 ) {
							return SweepReject(
								"sweep_geometry `%s`: point %u (`%s`) must be exactly three numbers `<x> <y> <z>`",
								name.c_str(), (unsigned int)i, pointLines[i].c_str() );
						}
						pts.push_back( x ); pts.push_back( y ); pts.push_back( z );
					}

					// OPTIONAL repeatable per-control-point width (x-axis) multipliers,
					// one per `point` (the rest pad with 1.0).  A NON-linear width profile.
					const std::vector<std::string>& widthLines = bag.GetRepeatable( "point_width" );
					std::vector<double> widths;
					if( !widthLines.empty() ) {
						if( widthLines.size() > pointLines.size() ) {
							return SweepReject(
								"sweep_geometry `%s`: %u `point_width` entries exceed %u `point` path control points (one width per point; pad with 1.0)",
								name.c_str(), (unsigned int)widthLines.size(), (unsigned int)pointLines.size() );
						}
						widths.reserve( widthLines.size() );
						for( std::size_t i = 0; i < widthLines.size(); ++i ) {
							double w = 0;
							char trailing[8] = {0};
							if( sscanf( widthLines[i].c_str(), "%lf %7s", &w, trailing ) != 1 ) {
								return SweepReject(
									"sweep_geometry `%s`: point_width %u (`%s`) must be exactly one number `<sx>`",
									name.c_str(), (unsigned int)i, widthLines[i].c_str() );
							}
							if( !( w > 0 ) ) {
								return SweepReject(
									"sweep_geometry `%s`: point_width %u (%g) must be > 0", name.c_str(), (unsigned int)i, w );
							}
							widths.push_back( w );
						}
					}

					// OPTIONAL repeatable per-control-point scale multipliers,
					// one per `point` (the rest pad with 1.0).  TWO arities:
					//   `point_scale <s>`       -- UNIFORM on both profile axes
					//                              (the historical form; a round
					//                              varying radius)
					//   `point_scale <sx> <sy>` -- ANISOTROPIC (doc 89 slice A):
					//                              sx along the profile's local x
					//                              (binormal), sy along its local
					//                              y (frame normal), so a section
					//                              can be flatter than it is wide
					//                              and change that ratio station
					//                              to station.
					// A 1-arg line inside an otherwise 2-arg track simply means
					// sx == sy at that station, so the two forms mix freely; the
					// y track is only handed to the factory when at least one
					// line actually used the 2-arg form, which keeps every
					// pre-slice-A scene on the byte-identical uniform path.
					// Same optional-arity trailing-garbage trap profile_circle /
					// profile_rect have, so TEXT-validate the token count first.
					const std::vector<std::string>& scaleLines = bag.GetRepeatable( "point_scale" );
					std::vector<double> scales, scalesY;
					bool anyAnisotropic = false;
					if( !scaleLines.empty() ) {
						if( scaleLines.size() > pointLines.size() ) {
							return SweepReject(
								"sweep_geometry `%s`: %u `point_scale` entries exceed %u `point` path control points (one scale per point; pad with 1.0)",
								name.c_str(), (unsigned int)scaleLines.size(), (unsigned int)pointLines.size() );
						}
						scales.reserve( scaleLines.size() );
						scalesY.reserve( scaleLines.size() );
						for( std::size_t i = 0; i < scaleLines.size(); ++i ) {
							int nTok = 0;
							if( !AllTokensAreFiniteNumbers( scaleLines[i].c_str(), &nTok ) || nTok < 1 || nTok > 2 ) {
								return SweepReject(
									"sweep_geometry `%s`: point_scale %u (`%s`) must be `<s>` (uniform) or `<sx> <sy>` (anisotropic) -- finite numbers, no trailing garbage",
									name.c_str(), (unsigned int)i, scaleLines[i].c_str() );
							}
							double sx = 0, sy = 0;
							const int nf = sscanf( scaleLines[i].c_str(), "%lf %lf", &sx, &sy );
							if( nf < 2 ) { sy = sx; } else { anyAnisotropic = true; }
							if( !( sx > 0 ) || !( sy > 0 ) ) {
								return SweepReject(
									"sweep_geometry `%s`: point_scale %u (%g %g) must be > 0 on both axes", name.c_str(), (unsigned int)i, sx, sy );
							}
							scales.push_back( sx );
							scalesY.push_back( sy );
						}
					}
					if( !anyAnisotropic ) {
						scalesY.clear();
					}

					// OPTIONAL repeatable per-control-point MORPH blend factors
					// (doc 89 slice A), one per `point` (the rest pad with 1.0 --
					// "finish the morph").  t = 0 is `profile`, t = 1 is
					// `profile2`; the two halves of the loft are useless apart,
					// so authoring either one alone is an error that NAMES the
					// missing half.
					const std::vector<std::string>& morphLines = bag.GetRepeatable( "point_morph" );
					std::vector<double> morphs;
					if( !morphLines.empty() && !haveProfile2 ) {
						return SweepReject(
							"sweep_geometry `%s`: `point_morph` needs a SECOND profile to morph toward -- add profile2_point / profile2_circle / profile2_rect (or drop the point_morph lines)",
							name.c_str() );
					}
					if( !morphLines.empty() ) {
						if( morphLines.size() > pointLines.size() ) {
							return SweepReject(
								"sweep_geometry `%s`: %u `point_morph` entries exceed %u `point` path control points (one morph per point; pad with 1.0)",
								name.c_str(), (unsigned int)morphLines.size(), (unsigned int)pointLines.size() );
						}
						morphs.reserve( morphLines.size() );
						for( std::size_t i = 0; i < morphLines.size(); ++i ) {
							int nTok = 0;
							if( !AllTokensAreFiniteNumbers( morphLines[i].c_str(), &nTok ) || nTok != 1 ) {
								return SweepReject(
									"sweep_geometry `%s`: point_morph %u (`%s`) must be exactly one finite number `<t>` (no trailing garbage, no nan/inf)",
									name.c_str(), (unsigned int)i, morphLines[i].c_str() );
							}
							double t = 0;
							sscanf( morphLines[i].c_str(), "%lf", &t );
							// negated idiom: refuse (rather than clamp) an
							// authored value outside [0, 1] -- past either end
							// there is no profile left to blend toward, so the
							// author meant something else and should be told.
							if( !( t >= 0.0 && t <= 1.0 ) ) {
								return SweepReject(
									"sweep_geometry `%s`: point_morph %u (%g) must be in [0, 1] (0 = profile, 1 = profile2)",
									name.c_str(), (unsigned int)i, t );
							}
							morphs.push_back( t );
						}
					}

					SweepDescriptor d;
					d.profilePoints    = &prof[0];
					d.numProfilePoints = (unsigned int)( prof.size() / 2 );
					d.pathPoints       = &pts[0];
					d.numPathPoints    = (unsigned int)pointLines.size();
					d.nLen             = (int)bag.GetUInt( "n_len", (unsigned int)d.nLen );
					d.endScaleX        = bag.GetDouble( "end_scale_x", d.endScaleX );
					d.endScaleY        = bag.GetDouble( "end_scale_y", d.endScaleY );
					d.capStart         = bag.GetBool( "cap_start", d.capStart );
					d.capEnd           = bag.GetBool( "cap_end",   d.capEnd );
					d.pathClosed       = bag.GetBool( "path_closed", d.pathClosed );
					if( d.pathClosed ) {
						// cap_start / cap_end are ignored for a closed loop (there is
						// no start/end cross-section) -- bag.Has() distinguishes an
						// explicitly-authored TRUE from the untouched TRUE default, so
						// silently drop the default but reject an explicit contradiction
						// rather than silently ignoring authored intent.
						if( bag.Has( "cap_start" ) && bag.GetBool( "cap_start", true ) ) {
							return SweepReject(
								"sweep_geometry `%s`: cap_start TRUE is incompatible with path_closed (a closed loop has no start cross-section)",
								name.c_str() );
						}
						if( bag.Has( "cap_end" ) && bag.GetBool( "cap_end", true ) ) {
							return SweepReject(
								"sweep_geometry `%s`: cap_end TRUE is incompatible with path_closed (a closed loop has no end cross-section)",
								name.c_str() );
						}
					}
					if( !widths.empty() ) {
						d.pointWidths    = &widths[0];
						d.numPointWidths = (unsigned int)widths.size();
					}
					if( !scales.empty() ) {
						d.pointScales    = &scales[0];
						d.numPointScales = (unsigned int)scales.size();
					}
					if( !scalesY.empty() ) {
						d.pointScalesY    = &scalesY[0];
						d.numPointScalesY = (unsigned int)scalesY.size();
					}
					if( !prof2.empty() ) {
						d.profile2Points    = &prof2[0];
						d.numProfile2Points = (unsigned int)( prof2.size() / 2 );
					}
					if( !morphs.empty() ) {
						d.pointMorphs    = &morphs[0];
						d.numPointMorphs = (unsigned int)morphs.size();
					}
					{
						const std::string fh = bag.GetString( "frame_hint", "" );
						if( !fh.empty() ) {
							double hx = 0, hy = 0, hz = 0;
							char trailing[8] = {0};
							if( sscanf( fh.c_str(), "%lf %lf %lf %7s", &hx, &hy, &hz, trailing ) != 3 ) {
								return SweepReject(
									"sweep_geometry `%s`: frame_hint must be three numbers `<x> <y> <z>`", name.c_str() );
							}
							d.frameHintX = hx; d.frameHintY = hy; d.frameHintZ = hz;
						}
					}
					return pJob.AddSweepGeometry( name.c_str(), d );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "sweep_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Sweeps (LOFTS) a CLOSED 2D profile along a 3D Catmull-Rom path (point lines) with rotation-minimizing frames.  x -> binormal, h -> normal; UV = (profile arc frac, path frac), U wraps seamlessly.  Optional ear-clipped caps.  The section is NOT stuck round and NOT stuck at one shape: `point_scale <sx> <sy>` scales the two profile axes INDEPENDENTLY per station -- a TORSO, fin, strap, hull, keel, seat rail: flatter than it is wide, and changing that ratio along the spine.  A second profile (profile2_*) plus per-station `point_morph <t>` changes the section's OUTLINE along the path -- a SNOUT (round skull -> narrow muzzle), a round-to-square furniture leg, a duct meeting a rectangular vent.  Order: morph shapes the section, THEN point_scale / point_width (x only) / end_scale size it, all multiplicative.  path_closed TRUE: seamless loop (periodic sampling, holonomy-corrected frame, no caps; end_scale forced 1.0; a loop may morph only with EXPLICIT point_morph values).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";          p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "profile_point"; p.kind = ValueKind::String; p.repeatable = true; p.description = "Closed-profile vertex `<x> <h>` (repeatable, >= 3, CCW = outward normals).  Exclusive with profile_circle/profile_rect"; }
						{ auto& p = P(); p.name = "profile_circle"; p.kind = ValueKind::String; p.description = "Convenience profile `<r> [n]`: CCW n-gon, r > 0, n default 24 (clamped 3..512).  Exclusive with profile_point/profile_rect"; }
						{ auto& p = P(); p.name = "profile_rect";  p.kind = ValueKind::String; p.description = "Convenience profile `<w> <h> [r]`: CCW rect, w/h > 0, optional corner radius r (default 0, <= min(w,h)/2).  Exclusive with profile_point/profile_circle"; }
						{ auto& p = P(); p.name = "profile2_point"; p.kind = ValueKind::String; p.repeatable = true; p.description = "OPTIONAL SECOND profile vertex `<x> <h>` (repeatable, >= 3, same CCW rule).  Its presence LOFTS the sweep: the section morphs profile -> profile2 along the path.  Exclusive with the other profile2_*"; }
						{ auto& p = P(); p.name = "profile2_circle"; p.kind = ValueKind::String; p.description = "OPTIONAL second profile, circle form `<r> [n]` (as profile_circle).  Exclusive with the other profile2_*"; }
						{ auto& p = P(); p.name = "profile2_rect";  p.kind = ValueKind::String; p.description = "OPTIONAL second profile, rect form `<w> <h> [r]` (as profile_rect).  Exclusive with the other profile2_*"; }
						{ auto& p = P(); p.name = "point";         p.kind = ValueKind::String; p.repeatable = true; p.description = "Path control point `<x> <y> <z>` (repeatable; >= 2, or >= 3 when path_closed).  Catmull-Rom spline through these"; }
						{ auto& p = P(); p.name = "point_width";   p.kind = ValueKind::String; p.repeatable = true; p.description = "OPTIONAL per-point x-axis width `<sx>` (repeatable, > 0, missing padded 1.0; periodic when path_closed).  HISTORICAL x-only control -- prefer `point_scale <sx> <sy>`, which states both axes.  Composed with end_scale_x and point_scale.  Omit = uniform 1.0"; }
						{ auto& p = P(); p.name = "point_scale";   p.kind = ValueKind::String; p.repeatable = true; p.description = "OPTIONAL per-point section scale: `<s>` (UNIFORM, a round taper) or `<sx> <sy>` (ANISOTROPIC -- x = binormal axis, y = normal axis; the RECOMMENDED form for any non-circular body, e.g. a torso 1.0 0.55 flattening to 0.6 0.4).  Repeatable, > 0, missing padded 1.0, periodic when path_closed; arities mix freely.  Composed with point_width and end_scale, applied AFTER the morph.  Omit = uniform 1.0"; }
						{ auto& p = P(); p.name = "point_morph";   p.kind = ValueKind::String; p.repeatable = true; p.description = "OPTIONAL per-point morph blend `<t>` in [0, 1]: 0 = profile, 1 = profile2 (repeatable).  A SHORT track is padded with 1.0 -- i.e. FULLY profile2 -- so the remaining stations finish the morph rather than holding the last authored value.  The morphed section is anchored at profile's VERTEX MEAN (not its area centroid), so a profile2 whose vertices are clustered along one side anchors off its visual centre.  Requires a profile2_* source.  Omit = a linear 0 -> 1 ramp along the stations, which path_closed refuses (a ramp jumps at the seam) -- give a loop explicit values like 0/1/0 instead"; }
						{ auto& p = P(); p.name = "n_len";         p.kind = ValueKind::UInt;   p.description = "Requested samples along the path (clamped 2..4096)"; p.defaultValueHint = "64"; }
						{ auto& p = P(); p.name = "end_scale_x";   p.kind = ValueKind::Double; p.description = "Profile x scale at path end (taper from 1).  Must stay 1.0 when path_closed"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "end_scale_y";   p.kind = ValueKind::Double; p.description = "Profile h scale at path end (taper from 1).  Must stay 1.0 when path_closed"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "cap_start";     p.kind = ValueKind::Bool;   p.description = "Close the start cross-section (ear-clipped).  Must not be explicitly TRUE when path_closed"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "cap_end";       p.kind = ValueKind::Bool;   p.description = "Close the end cross-section (ear-clipped).  Must not be explicitly TRUE when path_closed"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "frame_hint";    p.kind = ValueKind::String; p.description = "Initial binormal hint `<x> <y> <z>` (omit = world axis most perpendicular to the start tangent)"; }
						{ auto& p = P(); p.name = "path_closed";   p.kind = ValueKind::Bool;   p.description = "Sweep a seamless CLOSED loop: periodic path sampling, holonomy-corrected frame (no seam twist), cyclic ring stitching, no caps.  Needs >= 3 point entries, first/last not coincident.  end_scale must stay 1.0; cap_start/cap_end must not be TRUE"; p.defaultValueHint = "FALSE"; }
						return cd;
					}();
					return d;
				}
			};

			struct LatheGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				//! Same one-place-both-diagnostics pattern as
				//! SkeletonGeometryAsciiChunkParser::Reject -- the specific
				//! reason reaches the CST diag sink AND the log, so an author
				//! (or the agent surface) is told which profile_point is at
				//! fault rather than "the chunk failed".
				static bool Reject( const std::string& why )
				{
					if( RISE::g_cstFinalizeDiagSink ) *RISE::g_cstFinalizeDiagSink = why;
					GlobalLog()->PrintEx( eLog_Error, "lathe_geometry:: %s", why.c_str() );
					return false;
				}

				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					const std::string name = bag.GetString( "name", "noname" );

					const std::vector<std::string>& profLines = bag.GetRepeatable( "profile_point" );
					if( profLines.size() < 2 ) {
						char buf[32];
						std::snprintf( buf, sizeof(buf), "%u", (unsigned int)profLines.size() );
						return Reject( "`" + name + "`: need at least 2 repeatable `profile_point <r> <h>` "
							"entries (the silhouette to revolve) -- got " + buf );
					}

					std::vector<double> prof;
					prof.reserve( profLines.size() * 2 );
					bool anyPositiveRadius = false;
					for( std::size_t i = 0; i < profLines.size(); ++i ) {
						// TEXT-validate first: an exact 2-token all-finite-numeric
						// check catches wrong arity, non-numeric fields, glued
						// trailing garbage (`0.35abc`, which a bare sscanf would
						// silently truncate), and nan/inf spellings in one shot --
						// the same gate sweep_geometry's optional-arity
						// profile_circle/profile_rect were moved onto in the C2
						// fix round.
						int nTok = 0;
						if( !AllTokensAreFiniteNumbers( profLines[i].c_str(), &nTok ) || nTok != 2 ) {
							char buf[32];
							std::snprintf( buf, sizeof(buf), "%u", (unsigned int)i );
							return Reject( "`" + name + "`: profile_point " + buf + " (`" + profLines[i] +
								"`) must be exactly two finite numbers `<r> <h>` (no trailing garbage, no nan/inf)" );
						}
						double r = 0, h = 0;
						std::sscanf( profLines[i].c_str(), "%lf %lf", &r, &h );
						if( !( r >= 0.0 ) ) {
							char buf[96];
							std::snprintf( buf, sizeof(buf), "%u (%g)", (unsigned int)i, r );
							return Reject( "`" + name + "`: profile_point " + buf + " has a NEGATIVE radius -- "
								"the profile is a silhouette in the HALF-plane, so r must be >= 0 "
								"(r = 0 puts the point ON the axis, which is how a vase closes)" );
						}
						if( r > 0.0 ) {
							anyPositiveRadius = true;
						}
						prof.push_back( r );
						prof.push_back( h );
					}
					if( !anyPositiveRadius ) {
						return Reject( "`" + name + "`: every profile_point has radius 0 -- the whole profile "
							"lies ON the axis and revolves to zero area; at least one point needs r > 0" );
					}

					LatheDescriptor d;

					{
						// LOWER-CASE ONLY, exactly matching the `enumValues`
						// this parameter advertises: the descriptor IS the
						// accepted set (that is the whole point of the
						// descriptor-driven parsers), so accepting `Y` while
						// the schema, autocomplete and read_schema all say
						// `y` would understate what parses and let the two
						// drift.
						const std::string axisStr = bag.GetString( "axis", "y" );
						if     ( axisStr == "x" ) d.axis = 0;
						else if( axisStr == "y" ) d.axis = 1;
						else if( axisStr == "z" ) d.axis = 2;
						else {
							return Reject( "`" + name + "`: axis `" + axisStr + "` must be x, y or z (lower case)" );
						}
					}

					d.sweepDegrees = bag.GetDouble( "sweep_degrees", d.sweepDegrees );
					// Negated idiom on purpose (matches sweep_geometry's
					// profile_rect range check): a NaN fails BOTH halves and
					// is rejected, where `< 0 || > 360` would pass it through.
					// Defense in depth -- the dispatcher's numeric ValueKind
					// gate already rejects a `nan` token before Finalize runs.
					if( !( d.sweepDegrees > 0.0 && d.sweepDegrees <= 360.0 ) ) {
						char buf[64];
						std::snprintf( buf, sizeof(buf), "%g", d.sweepDegrees );
						return Reject( std::string( "`" ) + name + "`: sweep_degrees (" + buf +
							") must be in (0, 360] -- 360 is a full turn, less is a section/cutaway" );
					}

					d.profilePoints    = &prof[0];
					d.numProfilePoints = (unsigned int)( prof.size() / 2 );
					d.nRadial          = (int)bag.GetUInt( "n_radial", (unsigned int)d.nRadial );
					d.smooth           = bag.GetBool( "smooth", d.smooth );

					return pJob.AddLatheGeometry( name.c_str(), d );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "lathe_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "A SURFACE OF REVOLUTION: an open 2D profile polyline (repeatable `profile_point <r> <h>`, r = radius from the axis, h = height along it) spun about a world axis.  Vases, bottles, goblets, turned table/chair legs, lamp bases, pedestals, finials, knobs -- the furniture-and-vessel vocabulary, authored as the one thing a silhouette already is: an outline.  A profile point with r = 0 sits ON the axis, so its ring collapses to a single POLE vertex fanned to its neighbour -- a profile that starts and ends at r = 0 is a closed, watertight vessel with NO caps needed and no zero-area triangles.  `sweep_degrees` below 360 makes a section/cutaway and adds two flat ear-clipped caps on the cut half-planes; at exactly 360 the last radial column stitches straight back to the first (no duplicated seam column).  UV = (angle fraction, normalized ARC LENGTH along the profile), so texture density stays even ALONG the profile however unevenly it is sampled -- with one exception, the single wrap-around band of a full 360 turn, where U runs the whole range backwards across that one band (the same seam convention sweep_geometry uses for its wrapped profile).  Outward orientation is DERIVED from the profile's signed volume of revolution, so a profile authored bottom-to-top or top-to-bottom both face outward.  A cutaway's cap is the profile closed back through the AXIS, which is the body's cross-section for a SOLID silhouette; for a TUBE/shell profile whose two ends sit at DIFFERENT heights that cap also fills the bore (when the two ends are level the cross-section is instead closed directly, so no zero-area spur is emitted).  The baked mesh is DOUBLE-SIDED (as sweep_geometry's is), so a luminaire material on a lathe radiates from the inside face as well as the outside -- worth knowing when a vase or a lamp shade IS the light.  One interaction to plan around: displaced_geometry over a PINCHED (waisted/hourglass) lathe TEARS at the waist, because the two pinch poles are coincident but carry opposite normals and displacement separates them by twice disp_scale -- inherent to split normals (sweep_geometry's hard-edge idiom has it too), and sharing one vertex instead would put one band's shading normal in the wrong half-space.  Displace an unpinched profile instead.  Contrast sweep_geometry, which moves a CLOSED profile polygon along an arbitrary 3D path; a lathe spins an OPEN polyline about one fixed straight axis.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";          p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "profile_point"; p.kind = ValueKind::String; p.repeatable = true; p.required = true;
						  p.description = "One silhouette vertex `<r> <h>` (repeatable; at least 2 required).  `r` is the radius from the axis and must be >= 0; r = 0 puts the point ON the axis, collapsing that ring to a single pole vertex (how a vase closes at top and bottom).  A radius within 1e-9 of the profile's own bounding extent is SNAPPED to the axis, so a GENERATED silhouette (whose `sin(pi)` endpoint is 1.5e-16, not 0) still reads as a pole rather than as a ring of sliver triangles.  `h` is the height along the axis.  Not every point may be r = 0.  Duplicate a point to HARDEN that edge (the zero-length segment splits the two normals), exactly as in sweep_geometry"; }
						{ auto& p = P(); p.name = "axis";          p.kind = ValueKind::Enum;   p.enumValues = {"x","y","z"};
						  p.description = "World axis to revolve about.  `y` matches cylinder_geometry and the SDF roundcone/capsule local-Y convention, so a lathe drops straight into a scene alongside them"; p.defaultValueHint = "y"; }
						{ auto& p = P(); p.name = "sweep_degrees"; p.kind = ValueKind::Double;
						  p.description = "Angular extent of the revolution, in (0, 360].  360 = a full closed turn (no duplicated seam column, no radial caps); anything less is a section/cutaway and gets a flat ear-clipped cap on each cut half-plane"; p.defaultValueHint = "360"; }
						{ auto& p = P(); p.name = "n_radial";      p.kind = ValueKind::UInt;
						  p.description = "Radial SEGMENTS around the axis (clamped 3..2048, with a warning when clamped).  A full turn emits n_radial columns; a partial sweep emits n_radial + 1"; p.defaultValueHint = "48"; }
						{ auto& p = P(); p.name = "smooth";        p.kind = ValueKind::Bool;
						  p.description = "TRUE: one row per profile point carrying the AVERAGE of its two adjacent segment normals -- a straight-sided section still shades flat, a curved one reads like a turned surface, and duplicating a profile point hardens just that edge.  FALSE: two rows per profile segment carrying that segment's flat normal (fully faceted)"; p.defaultValueHint = "TRUE"; }
						return cd;
					}();
					return d;
				}
			};

			struct SkinGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				//! Same one-place-both-diagnostics pattern as
				//! LatheGeometryAsciiChunkParser::Reject -- the specific
				//! reason reaches the CST diag sink AND the log, so an author
				//! (or the agent surface) is told WHICH rail point is at fault
				//! rather than "the chunk failed".
				static bool Reject( const std::string& why )
				{
					if( RISE::g_cstFinalizeDiagSink ) *RISE::g_cstFinalizeDiagSink = why;
					GlobalLog()->PrintEx( eLog_Error, "skin_geometry:: %s", why.c_str() );
					return false;
				}

				//! One rail's repeatable `<x> <y> <z>` lines -> a flat triple
				//! list.  TEXT-validates first: an exact 3-token all-finite
				//! numeric check catches wrong arity, non-numeric fields,
				//! glued trailing garbage (`0.35abc`, which a bare sscanf
				//! silently truncates) and every nan/inf spelling in one shot.
				//! That gate is the C2-round lesson applied from the start
				//! here rather than retrofitted: sweep_geometry's `point`
				//! lines still reach the factory through a bare sscanf, which
				//! is exactly the hole this avoids.
				static bool ReadRail( const ParseStateBag& bag, const std::string& name,
				                      const char* key, std::vector<double>& out )
				{
					const std::vector<std::string>& lines = bag.GetRepeatable( key );
					if( lines.size() < 2 ) {
						char buf[32];
						std::snprintf( buf, sizeof(buf), "%u", (unsigned int)lines.size() );
						return Reject( "`" + name + "`: need at least 2 repeatable `" + key +
							" <x> <y> <z>` entries (one of the sheet's two boundary curves) -- got " + buf );
					}
					out.reserve( lines.size() * 3 );
					for( std::size_t i = 0; i < lines.size(); ++i ) {
						int nTok = 0;
						if( !AllTokensAreFiniteNumbers( lines[i].c_str(), &nTok ) || nTok != 3 ) {
							char buf[32];
							std::snprintf( buf, sizeof(buf), "%u", (unsigned int)i );
							return Reject( std::string( "`" ) + name + "`: " + key + " " + buf + " (`" + lines[i] +
								"`) must be exactly three finite numbers `<x> <y> <z>` (no trailing garbage, no nan/inf)" );
						}
						double x = 0, y = 0, z = 0;
						std::sscanf( lines[i].c_str(), "%lf %lf %lf", &x, &y, &z );
						out.push_back( x ); out.push_back( y ); out.push_back( z );
					}
					return true;
				}

				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					const std::string name = bag.GetString( "name", "noname" );

					std::vector<double> railA, railB;
					if( !ReadRail( bag, name, "rail_a", railA ) ) { return false; }
					if( !ReadRail( bag, name, "rail_b", railB ) ) { return false; }

					SkinDescriptor d;
					d.railAPoints    = &railA[0];
					d.numRailAPoints = (unsigned int)( railA.size() / 3 );
					d.railBPoints    = &railB[0];
					d.numRailBPoints = (unsigned int)( railB.size() / 3 );
					d.nLen           = (int)bag.GetUInt( "n_len",    (unsigned int)d.nLen );
					d.nAcross        = (int)bag.GetUInt( "n_across", (unsigned int)d.nAcross );
					d.billow         = bag.GetDouble( "billow", d.billow );
					// Defense in depth -- the dispatcher's numeric ValueKind
					// gate already rejects a `nan` token before Finalize runs,
					// and the factory re-checks for the direct-API caller.
					// The negated idiom is deliberate: a NaN fails the compare
					// where `== HUGE_VAL` would let it through.
					if( !( fabs( d.billow ) < 1e300 ) ) {
						char buf[64];
						std::snprintf( buf, sizeof(buf), "%g", d.billow );
						return Reject( std::string( "`" ) + name + "`: billow (" + buf +
							") must be a finite number (no nan, no inf)" );
					}

					return pJob.AddSkinGeometry( name.c_str(), d );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "skin_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "A SKIN: the open sheet stretched between TWO boundary curves (`rail_a` and `rail_b`, each a repeatable `<x> <y> <z>` polyline).  WINGS and wing membranes, FINS, WEBBING between fingers or toes, SAILS, LEAVES and petals, AWNINGS and tarps, LAMPSHADE PANELS, kites, capes -- the whole \"thin surface bounded by curves\" family, which is the one shape class neither sweep_geometry (a closed section along a path) nor lathe_geometry (a silhouette about an axis) can state at all.  Pair it with skeleton_geometry to finish a limb: the bones are the skeleton, the membrane between them is the skin.  The two rails need NOT have the same number of points -- they are resampled onto the UNION of their arc-length parameters, so every authored vertex of BOTH rails appears in the mesh verbatim and an authored kink stays a kink instead of being chamfered away.  The base surface is RULED (each station is a straight span from rail_a to rail_b); `billow` then inflates the INTERIOR along the sheet's own normal with a falloff that is exactly zero at both rails, so the rails themselves never move -- that is the difference between a flat tarp and a wind-filled sail.  UV = (arc length ALONG the rails, 0 at rail_a to 1 at rail_b ACROSS), so a feather or vein texture lines up with the rails without a mapping chunk.  The bake is ONE double-sided sheet with no thickness: each face shades correctly from ITS OWN side, so the order the two rails are named -- which is what decides which way the surface normal points -- never creates a black side (verified per-ray: the same sheet authored rail_a-first and rail_b-first takes the SAME ray to a front-face hit, with the double-sided flip firing on exactly one of the two).  An opaque membrane lit only from the FAR side is dark, which is correct and is exactly what a thin solid slab would also do; thickness is not what makes a backlit wing glow, a transmitting MATERIAL is.  Rails that MEET at one or both ends are supported and are how a leaf or a pinched sail is authored: the meeting point collapses to ONE shared vertex and the tip becomes a connected fan, so there are no slivers, no duplicate vertices and no torn perimeter there.  Rails that merely come CLOSE (within 1e-9 of the sheet's own extent, the tolerance a generated rail needs) are snapped to that exact meeting first.  Rails that describe the SAME curve are refused: a skin needs two distinct boundaries to span.  A rail that only follows a companion chain's joint positions traces the BONE, not the body, and leaves the sheet touching its neighbor at one point instead of growing from it; run the attaching rail INTO the adjacent mass, not just up to its surface.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";     p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "rail_a";   p.kind = ValueKind::String; p.repeatable = true; p.required = true;
						  p.description = "One point `<x> <y> <z>` of the FIRST boundary curve (repeatable, in order along the curve; at least 2 required).  This rail is v = 0 in the UVs.  For a wing this is typically the LEADING edge, traced root to tip.  Duplicate a point to HARDEN that crease (the zero-length segment splits the two normals), exactly as in sweep_geometry and lathe_geometry"; }
						{ auto& p = P(); p.name = "rail_b";   p.kind = ValueKind::String; p.repeatable = true; p.required = true;
						  p.description = "One point `<x> <y> <z>` of the SECOND boundary curve (repeatable, at least 2; the count need NOT match rail_a).  This rail is v = 1.  For a wing this is the TRAILING edge, traced in the SAME direction as rail_a -- tracing it backwards spans the sheet as an hourglass"; }
						{ auto& p = P(); p.name = "n_len";    p.kind = ValueKind::UInt;
						  p.description = "Requested stations ALONG the rails (clamped 2..4096, with a warning when clamped).  A MINIMUM, not an exact count: every authored rail vertex is a station whether or not n_len asked for one, so the real count is at least the number of DISTINCT arc-length parameters across the two rails (a parameter both rails share is one station carrying both their points), and never fewer than n_len.  Raise it only to smooth a billowed or strongly curved sheet"; p.defaultValueHint = "32"; }
						{ auto& p = P(); p.name = "n_across"; p.kind = ValueKind::UInt;
						  p.description = "Vertex rows ACROSS the sheet, rail_a to rail_b (clamped 2..1024, with a warning when clamped).  2 is the two rails alone -- a flat ruled sheet with no interior, so `billow` has nothing to displace and is reported inert.  8 is plenty for a gently billowed membrane; raise it for a deep sail"; p.defaultValueHint = "8"; }
						{ auto& p = P(); p.name = "billow";   p.kind = ValueKind::Double;
						  p.description = "Inflation of the sheet's INTERIOR, as a FRACTION of each station's own rail-to-rail span (so a tapering wing billows proportionally and a pinched corner stays pinched), along the ruled sheet's normal, with a sin^2 falloff that is exactly zero and tangent at BOTH rails.  0 = a flat ruled surface (a tarp pulled tight).  0.1-0.25 = a filled sail or a wind-caught membrane.  POSITIVE inflates toward the sheet's own normal -- the direction of `d/du x d/dv`, i.e. rail-direction crossed into the rail_a-to-rail_b direction; NEGATIVE inflates the other way, which is the one-character fix when the bulge comes out on the wrong side.  A LARGE billow across a SHARP authored crease pinches at the crease (inflating a folded sheet folds it further -- that is the surface, not an artifact), and if it folds the sheet THROUGH itself the bake still happens but a warning names how many faces folded and where the first one is: keep billow modest on a kinked rail, round the kink, or raise n_len near it"; p.defaultValueHint = "0.0"; }
						return cd;
					}();
					return d;
				}
			};

			struct PathInstancesGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					std::string tmpl = bag.GetString( "geometry", "" );
					if( tmpl.empty() ) {
						GlobalLog()->PrintEx( eLog_Error,
							"path_instances_geometry `%s`: missing `geometry` (the named template geometry to instance)", name.c_str() );
						return false;
					}

					const std::vector<std::string>& pointLines = bag.GetRepeatable( "point" );
					if( pointLines.size() < 2 ) {
						GlobalLog()->PrintEx( eLog_Error,
							"path_instances_geometry `%s`: need at least 2 repeatable `point <x> <y> <z>` path control points (got %u)",
							name.c_str(), (unsigned int)pointLines.size() );
						return false;
					}
					std::vector<double> pts;
					pts.reserve( pointLines.size() * 3 );
					for( std::size_t i = 0; i < pointLines.size(); ++i ) {
						double x = 0, y = 0, z = 0;
						char trailing[8] = {0};
						if( sscanf( pointLines[i].c_str(), "%lf %lf %lf %7s", &x, &y, &z, trailing ) != 3 ) {
							GlobalLog()->PrintEx( eLog_Error,
								"path_instances_geometry `%s`: point %u (`%s`) must be exactly three numbers `<x> <y> <z>`",
								name.c_str(), (unsigned int)i, pointLines[i].c_str() );
							return false;
						}
						pts.push_back( x ); pts.push_back( y ); pts.push_back( z );
					}

					PathInstancesDescriptor d;
					d.pathPoints    = &pts[0];
					d.numPathPoints = (unsigned int)pointLines.size();
					d.nLen          = (int)bag.GetUInt( "n_len", (unsigned int)d.nLen );
					d.pitch         = bag.GetDouble( "pitch", d.pitch );
					d.phase         = bag.GetDouble( "phase", d.phase );
					d.slantDeg      = bag.GetDouble( "slant", d.slantDeg );
					d.scale         = bag.GetDouble( "scale", d.scale );
					d.detail        = bag.GetUInt( "detail", d.detail );
					{
						const std::string fh = bag.GetString( "frame_hint", "" );
						if( !fh.empty() ) {
							double hx = 0, hy = 0, hz = 0;
							char trailing[8] = {0};
							if( sscanf( fh.c_str(), "%lf %lf %lf %7s", &hx, &hy, &hz, trailing ) != 3 ) {
								GlobalLog()->PrintEx( eLog_Error,
									"path_instances_geometry `%s`: frame_hint must be three numbers `<x> <y> <z>`", name.c_str() );
								return false;
							}
							d.frameHintX = hx; d.frameHintY = hy; d.frameHintZ = hz;
						}
					}
					return pJob.AddPathInstancesGeometry( name.c_str(), tmpl.c_str(), d );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "path_instances_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Along-path instancing: a named TEMPLATE geometry (tessellated once through the universal TessellateToMesh contract -- any first-class geometry works, e.g. an sdf_geometry capsule) stamped along a 3D Catmull-Rom path at arc-length pitch with optional slant and scale.  Fence posts, rivets, beads, stitching, chain links.  Template +Y aligns with the path tangent (positive slant rotates the tangent toward the binormal about the frame normal), +Z with the frame normal, +X with the binormal.  Instances BANK with the path (no world-up mode); steer roll with frame_hint.  Alternating twists (chain links) = two chunks at 2x pitch with offset phase and pre-rotated templates.  Verified (doc 89 slice D) against a lathe_geometry template (a turned bead stamped along a curved path with the documented +Y/+Z/+X alignment holding) and against a displaced_geometry template (the template is Realize()'d before tessellation, so a row of dimpled/bark-textured beads is one displaced_geometry chunk plus this one, not N hand-placed copies) -- both derive and render with zero diagnostics.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";     p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "geometry"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Geometry}; p.description = "Named template geometry to instance (must support TessellateToMesh -- every first-class RISE geometry does)"; }
						{ auto& p = P(); p.name = "point";    p.kind = ValueKind::String;    p.repeatable = true; p.description = "Path control point `<x> <y> <z>` (repeatable, path order; at least 2)"; }
						{ auto& p = P(); p.name = "n_len";    p.kind = ValueKind::UInt;      p.description = "Arc-length walk resolution (path samples; clamped 2..8192)"; p.defaultValueHint = "256"; }
						{ auto& p = P(); p.name = "pitch";    p.kind = ValueKind::Double;    p.description = "Arc-length distance between instances (> 0)"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "phase";    p.kind = ValueKind::Double;    p.description = "Arc-length distance before the first instance (omit = pitch/2, the centred default)"; p.defaultValueHint = "pitch/2"; }
						{ auto& p = P(); p.name = "slant";    p.kind = ValueKind::Double;    p.description = "Template rotation about the frame normal, degrees (e.g. saddle-stitch slant; sign mirrors)"; p.defaultValueHint = "0.0"; }
						{ auto& p = P(); p.name = "scale";    p.kind = ValueKind::Double;    p.description = "Uniform template scale"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "detail";   p.kind = ValueKind::UInt;      p.description = "TessellateToMesh detail for the template (clamped 3..256; cost is O(detail^2) for most templates)"; p.defaultValueHint = "16"; }
					{ auto& p = P(); p.name = "frame_hint"; p.kind = ValueKind::String;  p.description = "Initial binormal hint `<x> <y> <z>` steering instance roll about the path (omit = world axis most perpendicular to the start tangent)"; }
						return cd;
					}();
					return d;
				}
			};

			struct BilinearPatchGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name",        "noname" );
					std::string file = bag.GetString( "file",        "none" );
					unsigned int maxPoly  = bag.GetUInt( "maxpolygons", 10 );
					unsigned int maxRecur = bag.GetUInt( "maxdepth",    8 );
					bool bsp              = bag.GetBool( "bsp",         false );
					return pJob.AddBilinearPatchGeometry( name.c_str(), file.c_str(), maxPoly, maxRecur, bsp );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "bilinearpatch_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Bilinear patch surface from file.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;   p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "file";        p.kind = ValueKind::Filename; p.description = "Patch file"; }
						{ auto& p = P(); p.name = "maxpolygons"; p.kind = ValueKind::UInt;     p.description = "Max polygons per BSP leaf"; p.defaultValueHint = "10"; }
						{ auto& p = P(); p.name = "maxdepth";    p.kind = ValueKind::UInt;     p.description = "Max BSP depth"; p.defaultValueHint = "8"; }
						{ auto& p = P(); p.name = "bsp";         p.kind = ValueKind::Bool;     p.description = "Build BSP"; p.defaultValueHint = "FALSE"; }
						return cd;
					}();
					return d;
				}
			};

			struct DisplacedGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name          = bag.GetString( "name",          "noname" );
					std::string base_geometry = bag.GetString( "base_geometry", "" );
					unsigned int detail       = bag.GetUInt(   "detail",        32 );
					std::string displacement  = bag.GetString( "displacement",  "none" );
					std::string height        = bag.GetString( "height",        "none" );
					double disp_scale         = bag.GetDouble( "disp_scale",    1.0 );
					bool double_sided         = bag.GetBool(   "double_sided",  false );
					bool face_normals         = bag.GetBool(   "face_normals",  false );
					bool seam_fold            = bag.GetBool(   "uv_seam_fold",  true );
					// Legacy maxpolygons/maxdepth/bsp keys accepted but ignored (Tier A2).

					if( base_geometry.empty() ) {
						GlobalLog()->Print( eLog_Error, "DisplacedGeometry:: `base_geometry` is required" );
						return false;
					}

					// The two height routes' mutual exclusion, the scalar-pipe
					// resolution of `height` and the Function2D resolution of
					// `displacement` all live in Job::AddDisplacedGeometryWith-
					// Height -- one home, so the CLI, the agent verbs and any
					// future caller get the same wording.
					return pJob.AddDisplacedGeometryWithHeight(
						name.c_str(),
						base_geometry.c_str(),
						detail,
						displacement == "none" ? 0 : displacement.c_str(),
						height == "none" ? 0 : height.c_str(),
						disp_scale,
						double_sided,
						face_normals,
						seam_fold );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "displaced_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Tessellated geometry with painter-driven vertex displacement.  TWO HEIGHT ROUTES, spell exactly one (naming both is a parse error): `displacement` samples an `IFunction2D` as `f(u,v)` (the original UV route, unchanged), while `height` evaluates a `scalar_painter` as a 3D FIELD at each vertex's OBJECT-space position -- which is what lets ONE `scalar_painter` drive `displaced_geometry { height F  disp_scale S }` for the coarse shape AND `relief_modifier { height F }` on the same object for the fine shading-normal relief (docs/RELIEF_MODIFIER_DESIGN.md section 5.3).  `disp_scale` applies to either.  `base_geometry` is a plain reference to ANY already-declared geometry -- doc 89 slice D audited every builder output against it: analytic primitives (sphere/box/torus/...), `sdf_geometry` (including a `skeleton_geometry`-expanded creature body and a `superellipsoid` part), `lathe_geometry`, `sweep_geometry` and `skin_geometry` all DERIVE, BAKE and RENDER cleanly with zero diagnostics, and `path_instances_geometry` can itself take a `displaced_geometry` as its template.  Scales/dimples/hammered-metal detail over any of those is one composition away.  Two verified caveats, neither a failure: (1) a `lathe_geometry`/`sweep_geometry`/`skin_geometry` base goes visibly FACETED under any non-zero displacement -- their bake is already a plain mesh, and the generic mesh-to-mesh tessellation path re-emits every triangle corner as its own unshared vertex, so the post-displacement normal recompute has nothing to average across (an `sdf_geometry` base's own dual-contoured topology keeps real shared vertices, so it stays smooth); raise the base's own tessellation density if the facets need to hide under the texture's wavelength.  (2) an INTERIOR-pinch `lathe_geometry` profile (waisted to `r 0` partway along, not just at an end) TEARS at the pinch under displacement -- its two coincident, oppositely-normalled pole vertices get pushed apart by `2 * disp_scale`; this is deliberate (see `LatheDescriptor`'s own comment) rather than a bug, since welding them would put one band's shading normal in the wrong half-space instead.  A `skeleton_geometry`/multi-part `sdf_geometry` base has NO per-limb UV: every part shares one cylindrical wrap around the WHOLE body's own bounding box (`u` = angle about local Y, `v` = normalized height), not a per-part frame, so scale the displacement painter for the whole silhouette rather than one limb.  See object-modeling-recipes Recipe 6.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";          p.kind = ValueKind::String;    p.description = "Unique name"; p.required = true; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "base_geometry"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Geometry}; p.required = true; p.description = "Geometry to displace"; }
						{ auto& p = P(); p.name = "detail";        p.kind = ValueKind::UInt;      p.description = "Subdivision detail level"; p.defaultValueHint = "32"; }
						{ auto& p = P(); p.name = "displacement";  p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter, ChunkCategory::Function}; p.description = "UV ROUTE (one of two, mutually exclusive with `height` -- see `height` below for the FIELD route, a 3D `scalar_painter` alternative).  An `IFunction2D` -- `expression_function2d`, `piecewise_linear_function2d`, `heightfield_function`, a dual-registered colour painter -- sampled as `f(u, v)` once per vertex.  Use it when the field is genuinely a function of texcoords, or for any pre-2026-09-06 scene (this route is unchanged).  Naming BOTH this and `height` is a parse error.  Resolved via pFunc2DManager (Job::AddDisplacedGeometry), which holds `function`-category IFunction2D chunks (piecewise_linear_function2d) PLUS every dual-registered colour painter except expression_painter/scalar_painter -- declared Function2D-piped with {Painter, Function} since 2026-09-06 (was declared {Painter} only, missing the plf2d/Function half of its real accepted set -- ChunkDescriptor.h's ParameterPipe doc comment)."; p.semantics.pipe = ParameterPipe::Function2D; }
						{ auto& p = P(); p.name = "height";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter};
						  p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true;
						  p.description = "FIELD ROUTE (one of two, mutually exclusive with `displacement`).  A `scalar_painter` evaluated as a 3D FIELD at each vertex -- so `expression`, `voronoi3d`, ramps, noise and every other 3D painter can drive displacement, which `displacement`'s UV sampling cannot (a 3D painter bound THERE goes through a fake hit and is a CONSTANT).  THE POINT: bind ONE `scalar_painter` to this AND to a `relief_modifier { height ... }` on the same object, and the same field gives coarse silhouette-changing displacement plus fine shading-normal relief -- docs/RELIEF_MODIFIER_DESIGN.md section 5.3.  That sharing is of the field's STATISTICS (same look, same scale, same seed), NOT point-for-point registration: this route samples the field at the PRE-displacement vertex while `relief_modifier` samples it at the POST-displacement hit, up to `disp_scale` times the field's own range apart along the normal -- see section 5.3 for the measured gap on the shipped fixture.  OBJECT SPACE: the mesh is baked BEFORE the geometry is bound to an object, so the geometry does not know its own transform; the synthetic hit carries the vertex's OBJECT-space position in BOTH `P` and `Po` (they COINCIDE here), and a `P`-authored field therefore does NOT follow the object's placement -- author against `Po`.  `u`/`v` ARE available and get the same `uv_seam_fold` treatment as the UV route, so a UV-domain field reads identically through either.  No pixel footprint exists at bake time (`fw` = 0), so noise octaves all resolve -- correct for a mesh built once at no particular viewing distance.  The synthetic bake-time hit also has no derivatives (`derivatives.valid` = FALSE) and no signal state: `curv`/`curvR` read 0 and the `occlusion()`/`thickness()` builtins read their neutral fallback (1/1), so a field that keys on them (e.g. `mix(a, b, clamp(curv,0,1))`) displaces FLAT through this route even where a `relief_modifier` bound to the same field would see real curvature/occlusion/thickness and tilt the normal.  A COLOUR painter bound here is refused with the standing scalar-pipe diagnostic: wrap it as `scalar_painter { name X_h  painter X  channel R }`.  Height is a LENGTH, not a colour, so it must never pass through JH spectral uplift -- which is exactly what the scalar pipe guarantees."; }
						{ auto& p = P(); p.name = "disp_scale";    p.kind = ValueKind::Double;    p.description = "Displacement scale, applied identically to whichever of `displacement` / `height` is bound"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "double_sided";  p.kind = ValueKind::Bool;      p.description = "Render both sides"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "face_normals";  p.kind = ValueKind::Bool;      p.description = "Flat per-face normals"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "uv_seam_fold";  p.kind = ValueKind::Bool;     p.description = "Tent-fold UV before displacement -- keeps a wrapped field continuous across the u=0/u=1 seam of CLOSED surfaces (sphere/torus/cylinder).  FALSE for an OPEN field on a non-wrapping Cartesian UV (e.g. a guilloché expression on a flat disk).  Applies to BOTH height routes: the `height` route's synthetic hit carries the folded (u,v) in `ptCoord`, so a UV-reading field folds identically whichever route it arrives by"; p.defaultValueHint = "TRUE"; }
						// Retired: accepted for backward compat with pre-A2 scene files; ignored.
						{ auto& p = P(); p.name = "maxpolygons";   p.kind = ValueKind::UInt;      p.description = "Retired (BVH is sole accelerator)"; }
						{ auto& p = P(); p.name = "maxdepth";      p.kind = ValueKind::UInt;      p.description = "Retired (BVH is sole accelerator)"; }
						{ auto& p = P(); p.name = "bsp";           p.kind = ValueKind::Bool;      p.description = "Retired (BVH is sole accelerator)"; }
						return cd;
					}();
					return d;
				}
			};

			// `hair_geometry` (slice D, docs/HAIR_FUR_DESIGN.md section
			// 5.3) -- a painter-driven GROOM: strands generated on the
			// surface of another named geometry.  The chunk is a RECIPE,
			// not a strand list: generation runs later, once,
			// single-threaded, inside IGeometry::Realize(), so a groom
			// bound to no rendered object is never generated at all.
			//
			// GROW-ON-A-SURFACE ONLY: this chunk never accepts a strand
			// POSITION.  Explicit per-strand authoring lives in the
			// separate `hair_guides` chunk (below), which authors SHAPE
			// -- polylines the generator interpolates onto its own
			// area-sampled roots.  Authoring an individual rendered
			// strand's position remains a C++-only path (HairGeometry's
			// explicit-strand constructor, for tools and tests).
			//! The `hair_geometry` grow-mode-only parameters, in one list:
			//! everything that describes how to GENERATE strands, and
			//! therefore has nothing to say about strands read out of a
			//! file.  Refused (not ignored) in `file` mode -- a silently
			//! ignored `count` or `comb` is the failure mode this list
			//! exists to prevent.  `name`, `file`, `width_root`,
			//! `width_tip` and `root_uv_mode` are the only parameters
			//! file mode accepts, so they are exactly the ones absent
			//! here.
			//!
			//! A FREE FUNCTION, not a private struct static (residual
			//! wave 2 item C, 2026-08-27, closing a documented drift-
			//! guard gap): declared in ChunkParserRegistry.h so
			//! HairFileImportTest's drift guard can call the REAL
			//! production list directly instead of maintaining its own
			//! byte-copied mirror that could silently drift from it.
			const char* const* HairGeometryGrowOnlyParameters( unsigned int& n )
			{
				static const char* const kGrowOnly[] = {
					"base_geometry", "count", "length", "segments", "seed", "base_detail",
					"density", "length_painter", "comb", "guides",
					"gravity", "frizz", "clump", "clump_size", "curl_radius", "curl_step"
				};
				n = (unsigned int)( sizeof(kGrowOnly) / sizeof(kGrowOnly[0]) );
				return kGrowOnly;
			}

			struct HairGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name          = bag.GetString( "name",          "noname" );
					std::string base_geometry  = bag.GetString( "base_geometry", "" );
					std::string file           = bag.GetString( "file",          "" );

					// The scene-language "not bound" spellings, matching
					// Job::AddHairGeometry's own `bound` lambda.
					auto bound = []( const std::string& s ) -> bool {
						return !s.empty() && s != "none";
					};

					const bool bFileMode = bound( file );
					const bool bGrowMode = bound( base_geometry );

					if( bFileMode && bGrowMode ) {
						GlobalLog()->PrintEx( eLog_Error,
							"hair_geometry:: `%s`: `file` and `base_geometry` are mutually exclusive -- a groom is "
							"either GROWN on a surface (`base_geometry` + `count` + `length`) or IMPORTED from a "
							".hair file (`file`), never both",
							name.c_str() );
						return false;
					}
					if( !bFileMode && !bGrowMode ) {
						GlobalLog()->PrintEx( eLog_Error,
							"hair_geometry:: `%s`: needs a strand source -- either `base_geometry` (grow the groom "
							"on a surface) or `file` (import a Cem Yuksel .hair file)",
							name.c_str() );
						return false;
					}

					if( bFileMode ) {
						// Every grow-mode parameter is refused rather than
						// ignored, so an author who imported a file and then
						// typed `count 50000` learns that it does nothing
						// instead of wondering why it had no effect.
						unsigned int nGrowOnly = 0;
						const char* const* growOnly = HairGeometryGrowOnlyParameters( nGrowOnly );
						for( unsigned int i = 0; i < nGrowOnly; ++i ) {
							if( bag.Has( growOnly[i] ) ) {
								GlobalLog()->PrintEx( eLog_Error,
									"hair_geometry:: `%s`: `%s` describes how to GENERATE a groom and has no meaning "
									"in `file` mode -- an imported .hair file supplies its own strands.  File mode "
									"accepts only `name`, `file`, `width_root`, `width_tip` and `root_uv_mode`",
									name.c_str(), growOnly[i] );
								return false;
							}
						}

						HairFileGroomDescriptor fd;
						fd.file           = file.c_str();
						// MULTIPLIERS in file mode, not absolute widths --
						// see the parameter descriptions below.
						fd.widthRootScale = bag.GetDouble( "width_root", 1.0 );
						fd.widthTipScale  = bag.GetDouble( "width_tip",  1.0 );

						const std::string rootUVModeStr = bag.GetString( "root_uv_mode", "zero" );
						if( rootUVModeStr == "zero" ) {
							fd.rootUVMode = HairFileRootUVMode::Zero;
						} else if( rootUVModeStr == "scatter" ) {
							fd.rootUVMode = HairFileRootUVMode::Scatter;
						} else {
							GlobalLog()->PrintEx( eLog_Error,
								"hair_geometry:: `%s`: unknown root_uv_mode `%s` (expected zero or scatter)",
								name.c_str(), rootUVModeStr.c_str() );
							return false;
						}

						return pJob.AddHairGeometryFromFile( name.c_str(), fd );
					}

					// `root_uv_mode` selects how an IMPORTED groom's
					// per-strand UV is assigned (see the `file` branch
					// above) -- a GROWN strand's root UV already comes from
					// the base surface's own UV map, so grow mode has
					// nothing to select between and refuses it symmetrically
					// with `HairGeometryGrowOnlyParameters` above, rather than silently
					// ignoring it.
					if( bag.Has( "root_uv_mode" ) ) {
						GlobalLog()->PrintEx( eLog_Error,
							"hair_geometry:: `%s`: `root_uv_mode` describes how an IMPORTED groom's per-strand UV is "
							"assigned and has no meaning in grow mode -- a grown strand's root UV already comes from "
							"the base surface's own UV map",
							name.c_str() );
						return false;
					}

					if( !bag.Has( "count" ) ) {
						GlobalLog()->Print( eLog_Error, "hair_geometry:: `count` is required (the strand budget)" );
						return false;
					}
					if( !bag.Has( "length" ) ) {
						GlobalLog()->Print( eLog_Error, "hair_geometry:: `length` is required (nominal strand length in scene units)" );
						return false;
					}

					std::string density        = bag.GetString( "density",        "none" );
					std::string length_painter = bag.GetString( "length_painter", "none" );
					std::string comb           = bag.GetString( "comb",           "none" );
					std::string guides         = bag.GetString( "guides",         "none" );

					HairGroomDescriptor desc;
					desc.baseGeometry  = base_geometry.c_str();
					desc.density       = density.c_str();
					desc.lengthPainter = length_painter.c_str();
					desc.comb          = comb.c_str();
					desc.guides        = guides.c_str();

					desc.p.count       = bag.GetUInt(   "count" );
					desc.p.segments    = bag.GetUInt(   "segments",    8 );
					desc.p.seed        = bag.GetUInt(   "seed",        1 );
					desc.p.baseDetail  = bag.GetUInt(   "base_detail", 32 );
					desc.p.length      = bag.GetDouble( "length" );
					desc.p.widthRoot   = bag.GetDouble( "width_root",  0.0001 );
					desc.p.widthTip    = bag.GetDouble( "width_tip",   0.00003 );
					desc.p.gravity     = bag.GetDouble( "gravity",     0.0 );
					desc.p.frizz       = bag.GetDouble( "frizz",       0.0 );
					desc.p.clump       = bag.GetDouble( "clump",       0.0 );
					desc.p.clumpSize   = bag.GetDouble( "clump_size",  0.0 );
					desc.p.curlRadius  = bag.GetDouble( "curl_radius", 0.0 );
					desc.p.curlStep    = bag.GetDouble( "curl_step",   0.0 );

					return pJob.AddHairGeometry( name.c_str(), desc );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "hair_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "A HAIR / FUR GROOM: many thin curve strands GROWN on the surface of another named geometry, held in one primitive with its own segment BVH (docs/HAIR_FUR_DESIGN.md section 5.2E / 5.3).  Bind it to a `standard_object` with a `hair_material` for Chiang-model fibre shading.  "
							"THE CHUNK IS A RECIPE, NOT A STRAND LIST: nothing is generated at parse time.  The base is tessellated and roots are AREA-WEIGHT sampled on it during the render's realize pass, so a groom nothing renders costs nothing, and everything below is deterministic from `seed` -- the same scene reproduces the same groom on every frame and every re-render, and to floating-point tolerance on every machine.  Explicit per-strand authoring is not accepted here; this chunk is grow-on-a-surface only.  "
							"AUTHORING IN ONE LINE: `count` roots at `length` long along the surface normal, then style them with the optional fields -- `gravity` droops, `comb` combs, `clump`/`clump_size` gather, `curl_radius`/`curl_step` curl, `frizz` roughens.  Each is inert at its default, so start with count/length/width and add one at a time.  "
							"UNITS ARE SCENE UNITS throughout (metres by default, see `scene_options scene_unit`): the width defaults are real human-hair numbers (0.1 mm root, 0.03 mm tip), so a groom authored on a scene-scale head needs no width tuning, and one on a stylised 1-unit sphere will want widths raised until the strands are visible at all.  "
							"NOT AN AREA LIGHT and NOT TESSELLATABLE: a groom cannot emit (emissive fur is out of scope) and cannot be a `displaced_geometry` base -- both refuse it with a diagnostic rather than approximating.  "
							"TWO SOURCES, EXACTLY ONE OF THEM: either GROW mode (`base_geometry` + `count` + `length`, everything described above) or IMPORT mode (`file` -- a Cem Yuksel `.hair` groom exported from Blender / Houdini / XGen, or one of the published research hair models).  Authoring both is an error, and in `file` mode every grow-mode parameter is REFUSED rather than quietly ignored: an imported file brings its own strands, so `count`, `length`, `segments`, `seed`, `density`, `comb`, `guides`, `gravity`, `frizz`, `clump*` and `curl*` have nothing to act on.  File mode accepts `name`, `file`, `width_root`, `width_tip` and `root_uv_mode` only, and the two widths change meaning there (they become MULTIPLIERS on the file's own thickness).  `root_uv_mode` is symmetrically refused in grow mode, where a strand's root UV already comes from the base surface.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";           p.kind = ValueKind::String;    p.required = true; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "base_geometry";  p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Geometry}; p.description = "Geometry to grow the hair ON.  It must be TESSELLATABLE (any analytic primitive, mesh, sdf_geometry, lathe/sweep/skin, or a displaced_geometry -- displace a scalp, then grow on the displaced surface).  An infinite plane, or another hair_geometry, is refused.  REQUIRED IN GROW MODE, AND REFUSED IN `file` MODE: an imported groom brings its own strands and grows on nothing"; }
						{ auto& p = P(); p.name = "count";          p.kind = ValueKind::UInt;      p.description = "Strand budget, BEFORE the density mask.  The `density` painter only ever removes candidates, so the realised strand count is <= this.  Capped at 2000000 with an error (a groom that large is gigabytes of control points); a full human scalp is ~100000-150000, convincing fur is ~50000+ per patch, and a few thousand is enough to judge a look.  Grow mode only -- refused in `file` mode"; }
						{ auto& p = P(); p.name = "length";         p.kind = ValueKind::Double;    p.description = "Nominal strand length in SCENE UNITS, multiplied per-strand by `length_painter`.  Must be > 0.  Grow mode only -- refused in `file` mode, where each strand's length is whatever the file says it is"; p.unitLabel = "scene units"; }
						{ auto& p = P(); p.name = "file";           p.kind = ValueKind::Filename;  p.description = "IMPORT MODE: a Cem Yuksel `.hair` binary groom to read strands from verbatim (path resolved against $RISE_MEDIA_PATH, like every other `file` parameter).  This is how a production groom gets in -- a Blender / Houdini / XGen export or a downloaded hair model -- since a six-figure strand count is megabytes of control points and belongs in a binary file next to the scene rather than inline in it.  Mutually exclusive with `base_geometry`, and it refuses every grow-mode parameter.  "
							"WHAT IS HONOURED: per-strand segment counts, control points, and per-point thickness (the format's thickness is read as a FULL WIDTH -- the published specification never says whether it means a radius or a diameter, so no conversion factor is invented; use `width_root` / `width_tip` to correct it).  Only each strand's FIRST and LAST thickness are used, because a RISE strand tapers linearly from one root width to one tip width.  "
							"WHAT IS NOT: per-point transparency and per-point colours are read for their length and discarded with a warning (fibre colour comes from the bound `hair_material`, and per-strand opacity is not modelled), and the format carries no root UVs -- so every imported strand's rootUV comes from `root_uv_mode` instead (default `zero`, every strand at (0,0), so a `hair_material` painter driven by the root UV will NOT vary across the groom unless `root_uv_mode scatter` is set).  "
							"UNITS AND AXES ARE THE FILE'S OWN: `.hair` declares neither, so nothing is rescaled or reoriented -- place the groom with the `standard_object`'s `scale` / `orientation` / `matrix`, exactly as for an imported mesh"; }
						{ auto& p = P(); p.name = "root_uv_mode";   p.kind = ValueKind::Enum;      p.enumValues = {"zero","scatter"}; p.description = "IMPORT MODE ONLY (refused in grow mode, where a strand's root UV already comes from the base surface's own UV map): how each imported strand's `ptCoord1` root UV is assigned, since the `.hair` format carries none of its own.  `zero` (default, and the ONLY behaviour before this parameter existed) assigns (0,0) to every strand, so a `hair_material` painter driven by the root UV (a scalp-space tint or roughness map) evaluates at one constant point and does not vary across the groom.  `scatter` assigns each strand its OWN deterministic pseudo-random UV in [0,1)^2, keyed on the strand's position in the file and a fixed internal seed -- the SAME file always scatters to the SAME per-strand UVs, but the values do not reconstruct the file's real scalp position (the format doesn't carry one); use it to give a root-UV-driven painter (a calico/patch pattern, a per-strand tint noise) something to vary against on an imported groom"; p.defaultValueHint = "zero"; }
						{ auto& p = P(); p.name = "segments";       p.kind = ValueKind::UInt;      p.description = "Control points per strand (>= 2).  2 is a perfectly straight quill; a curl or a strong comb needs 8-16 to read as a smooth curve rather than a polyline.  Cost is linear in this.  Grow mode only -- refused in `file` mode, where the file's own segment counts decide"; p.defaultValueHint = "8"; }
						{ auto& p = P(); p.name = "width_root";     p.kind = ValueKind::Double;    p.description = "GROW MODE: the FULL fibre width (not radius) at the root, in scene units; the default is real human hair (0.1 mm).  FILE MODE: a MULTIPLIER on the imported file's own thickness at each strand's first point, defaulting to 1.0 = verbatim -- write 2 if the file's thickness means a radius, or 0.001 to bring a millimetre-scale file into a metre-scale scene.  (If the file carries no thickness at all -- no array and a non-positive header default -- the groom falls back to these same human-hair numbers, with a warning, and the multiplier then scales THOSE)"; p.defaultValueHint = "0.0001 (grow) / 1.0 (file)"; p.unitLabel = "scene units"; }
						{ auto& p = P(); p.name = "width_tip";      p.kind = ValueKind::Double;    p.description = "GROW MODE: the FULL fibre width at the tip, linearly interpolated from the root in arc-length fraction; tapering to about a third of the root width is what reads as hair rather than as wire.  FILE MODE: a MULTIPLIER on the imported file's own thickness at each strand's LAST point, defaulting to 1.0 = verbatim"; p.defaultValueHint = "0.00003 (grow) / 1.0 (file)"; p.unitLabel = "scene units"; }
						{ auto& p = P(); p.name = "seed";           p.kind = ValueKind::UInt;      p.description = "Seeds every random draw (root placement, density rejection, frizz, curl phase).  Change it to re-roll the same groom recipe into a different arrangement; keep it fixed and the groom is identical on every frame and every re-render (and identical to floating-point tolerance across machines -- the random stream is bit-exact everywhere, the growth arithmetic that consumes it is not).  Placement and jitter are keyed PER CANDIDATE, so editing `density` or dropping one strand never re-rolls the strands around it"; p.defaultValueHint = "1"; }
						{ auto& p = P(); p.name = "base_detail";    p.kind = ValueKind::UInt;      p.description = "Tessellation detail requested from the base geometry.  Roots are sampled on THAT mesh, so this quantises where hair can grow and how faithfully the interpolated normals follow the real surface -- raise it for a curved base whose hair looks faceted at the silhouette"; p.defaultValueHint = "32"; }
						{ auto& p = P(); p.name = "density";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.description = "Optional [0,1] REJECTION MASK evaluated at each candidate root over the base surface's UV (physical SCALAR: a scalar_painter name or an inline numeric).  1 = keep every candidate here, 0 = bare skin.  This is where a bald patch, a hairline, an eyebrow shape or a fur pattern comes from -- any of the procedural painters (perlin, voronoi, expression, an image) works"; p.defaultValueHint = "none"; }
						{ auto& p = P(); p.name = "length_painter"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Scalar; p.description = "Optional per-root MULTIPLIER on `length` (physical SCALAR).  Short around the ears, long on top; a value of 0 drops the strand entirely"; p.defaultValueHint = "none"; }
						{ auto& p = P(); p.name = "comb";           p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.semantics.pipe = ParameterPipe::Color; p.description = "Optional COMB DIRECTION FIELD -- a colour painter whose RGB encodes a TANGENT-SPACE direction by d = 2*rgb - 1, in the frame {surface tangent, bitangent, normal}.  Neutral grey (0.5 0.5 0.5) is exactly no comb.  The vector's MAGNITUDE is the push expressed as a fraction of the strand's own length -- so `1 0.5 0.5` (d = (1,0,0)) sweeps the tip a full strand-length along the surface tangent.  The bend is weighted t^2 along the strand, so the root always leaves along the normal.  "
								"IT IS A FLOW MAP, NOT A NORMAL MAP: only the part TANGENT to the surface is used (a comb must not push hair into the scalp), so the BLUE channel is projected straight back out and has NO effect -- a normal map's `flat` pixel 0.5 0.5 1.0 means NO COMB here, not `comb upward`.  Author it as a two-channel direction field in red/green.  "
								"THE TANGENT FRAME IS DERIVED FROM THE BASE'S UV MAP (dP/du, orthogonalised against the shading normal), so it is continuous wherever the UV map is: a constant comb painter sweeps the whole surface ONE way, and a hand-painted comb map keeps meaning the same thing when `base_detail` re-tessellates the base.  A base with NO texture coordinates has no parameterization to derive from and falls back to each triangle's first edge, which is PER-TRIANGLE -- adjacent triangles disagree by tens of degrees, so a comb field on a UV-less base combs incoherently.  Give the base UVs before relying on `comb`"; p.defaultValueHint = "none"; }
						{ auto& p = P(); p.name = "gravity";        p.kind = ValueKind::Double;    p.description = "Downward (world -Y) droop at the TIP, as a fraction of the strand's own length; weighted t^2 so the root stays normal-aligned.  0 = no droop, 1 = the tip falls a full strand-length.  This is a styling knob, not a simulation -- it does not know about collisions"; p.defaultValueHint = "0.0"; }
						{ auto& p = P(); p.name = "frizz";          p.kind = ValueKind::Double;    p.description = "Per-control-point random jitter, as a fraction of the strand's own control-point spacing, weighted LINEARLY along the strand.  Small values (0.05-0.3) break up the machine-perfect look; large values shred the strand into noise.  Deterministic from `seed`, per strand, so editing an unrelated parameter does not re-roll it"; p.defaultValueHint = "0.0"; }
						{ auto& p = P(); p.name = "clump";          p.kind = ValueKind::Double;    p.description = "Clump attraction strength in [0,1]: how far a strand's TIP is pulled toward its clump centre's tip (weighted t^2, so roots never move).  Needs `clump_size` > 0 to do anything.  This is what turns an even fur coat into wet-looking or styled locks"; p.defaultValueHint = "0.0"; }
						{ auto& p = P(); p.name = "clump_size";     p.kind = ValueKind::Double;    p.description = "Clump CELL SIZE in scene units.  Roots are quantised onto a grid of this side and each occupied cell's first strand becomes that cell's clump centre; 0 disables clumping entirely.  Cell-shaped clumps are a Phase-1 simplification of a true nearest-centre-within-radius search -- two strands either side of a cell boundary join different clumps"; p.defaultValueHint = "0.0"; p.unitLabel = "scene units"; }
						{ auto& p = P(); p.name = "curl_radius";    p.kind = ValueKind::Double;    p.description = "Helix radius in scene units, superposed on the strand in the root's tangent frame and weighted t^2 so the curl opens out of the follicle instead of lifting the root off the surface.  0 = straight.  Requires `curl_step` > 0; each strand gets its own random starting phase so curls do not line up into a corduroy pattern"; p.defaultValueHint = "0.0"; p.unitLabel = "scene units"; }
						{ auto& p = P(); p.name = "curl_step";      p.kind = ValueKind::Double;    p.description = "Helix PITCH: arc length per full turn, in scene units.  Small relative to `length` = tight ringlets, large = a lazy wave.  Must be > 0 whenever `curl_radius` is > 0 (a helix with no pitch is not a curve)"; p.defaultValueHint = "0.0"; p.unitLabel = "scene units"; }
						// THE ONE PORT A `hair_guides` NAME MAY BIND.  `referenceCategories`
						// is `{ChunkCategory::HairGuides}` -- the category `hair_guides`
						// registers under -- so the legality checker, the reference graph
						// and jump-to-definition all agree that a guide set, and ONLY a
						// guide set, resolves here.  The `keywordAllowlist` below is kept
						// as defence-in-depth (it is what the pre-category-split build
						// relied on, and it still guards a future second chunk registered
						// under HairGuides that this slot would not want).
						{ auto& p = P(); p.name = "guides";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::HairGuides}; p.semantics.pipe = ParameterPipe::Other; p.semantics.keywordAllowlist = {"hair_guides"}; p.semantics.note = "Resolves against the Job's `hair_guides` table, not the geometry manager -- guides are data, not a scene entity"; p.description = "Optional named `hair_guides` set.  BOUND, IT REPLACES THE STRAIGHT-ALONG-THE-NORMAL GROWTH and nothing else: every strand's SHAPE is interpolated from its THREE NEAREST guides (nearest by root-to-guide-root distance; weights are inverse distance, normalised; K drops to the guide count when the set has fewer than three, so a ONE-guide set means every strand copies that guide).  "
								"GUIDES ARE SHAPES, NOT ROOTS.  `count`, `density` and the base surface still decide where hair grows and how much of it; a guide never places a strand, and a guide's own point count never decides `segments` -- each guide is resampled by ARC-LENGTH FRACTION onto this groom's control-point count.  Nor does a guide's own SIZE carry over: the interpolated shape is normalised by the guide's arc length and re-scaled by `length` / `length_painter`, so lengthening a guide restyles the groom without lengthening it.  "
								"THE TRANSPORT IS RIGID.  A guide is read in the base surface's frame at the point of the base CLOSEST TO THE GUIDE'S ROOT -- the same {tangent, bitangent, normal} frame a strand uses -- and replayed in the strand's own frame at the strand's own root.  So a guide swept `back along the scalp's UV` stays swept back everywhere it is used, around a curved base as much as a flat one; author guides ON or NEAR the surface they groom, because that projection is what attaches them.  A guide that is perfectly straight along its root normal reproduces the unguided groom exactly.  "
								"STYLING STILL COMPOSES: `comb`, `gravity`, `curl_radius`/`curl_step`, `frizz` and `clump` all apply ON TOP of the interpolated shape, and no random draw is spent on the interpolation -- a guided groom and an unguided one at the same `seed` share their roots and their jitter exactly, so binding or unbinding `guides` restyles without re-rolling.  An unknown name is an error, not a silent fall-back to straight growth"; p.defaultValueHint = "none"; }
						return cd;
					}();
					return d;
				}
			};

			// `hair_guides` (Phase 2, docs/HAIR_FUR_DESIGN.md section 5.3)
			// -- an AUTHORED GUIDE-STRAND SET, and the one place in the
			// hair system where explicit per-strand points are accepted.
			//
			// FLAT GRAMMAR, ONE GUIDE PER LINE.  The design sketches a
			// nested `strand { point ... }` block, but the scene language
			// binds parameters only at brace depth 1 (Cst.cpp's chunk
			// scan) -- a nested block survives a round-trip as opaque
			// tokens and never reaches a descriptor at all.  Repeating a
			// whole polyline on one `guide` line is the shape the language
			// already has for exactly this (skin_geometry's `rail_a` /
			// `rail_b`, sweep_geometry's `point`), so guides use it rather
			// than growing the grammar a nesting level for one chunk.
			struct HairGuidesAsciiChunkParser : public IAsciiChunkParser
			{
				static bool Reject( const std::string& msg )
				{
					GlobalLog()->PrintEx( eLog_Error, "hair_guides:: %s", msg.c_str() );
					return false;
				}

				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					const std::string name = bag.GetString( "name", "noname" );

					const std::vector<std::string>& lines = bag.GetRepeatable( "guide" );
					if( lines.empty() ) {
						return Reject( "`" + name + "`: needs at least one repeatable `guide <x> <y> <z> <x> <y> <z> ...` line" );
					}

					std::vector<double>       points;
					std::vector<unsigned int> counts;
					counts.reserve( lines.size() );

					for( std::size_t i = 0; i < lines.size(); ++i ) {
						int nTok = 0;
						if( !AllTokensAreFiniteNumbers( lines[i].c_str(), &nTok ) ) {
							char buf[32];
							std::snprintf( buf, sizeof(buf), "%u", (unsigned int)i );
							return Reject( "`" + name + "`: guide " + buf + " (`" + lines[i] +
								"`) must be finite numbers only (no trailing garbage, no nan/inf)" );
						}
						if( nTok % 3 != 0 || nTok < 6 ) {
							char buf[64];
							std::snprintf( buf, sizeof(buf), "%u", (unsigned int)i );
							char cnt[32];
							std::snprintf( cnt, sizeof(cnt), "%d", nTok );
							return Reject( std::string( "`" ) + name + "`: guide " + buf + " has " + cnt +
								" numbers -- a guide is a whole polyline on one line, `<x> <y> <z>` per point, "
								"and needs at least 2 points (6 numbers)" );
						}

						// Re-scan the line for the values themselves; the
						// gate above already proved every token parses.
						const char* s = lines[i].c_str();
						for( int t = 0; t < nTok; ++t ) {
							char* end = 0;
							const double v = std::strtod( s, &end );
							points.push_back( v );
							s = end;
						}
						counts.push_back( (unsigned int)( nTok / 3 ) );
					}

					HairGuidesDescriptor d;
					d.points      = &points[0];
					d.pointCounts = &counts[0];
					d.numGuides   = (unsigned int)counts.size();
					return pJob.AddHairGuides( name.c_str(), d );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						// ChunkCategory::HairGuides, NOT ::Geometry.  A guide set is
						// DATA a `hair_geometry` reads, never a renderable entity --
						// it is registered into `Job::hairGuidesMap`, never into the
						// IGeometryManager.  Its own category is what makes a guide
						// name structurally ineligible for an ordinary Geometry-typed
						// port (`standard_object.geometry` declares
						// `referenceCategories = {ChunkCategory::Geometry}` and a
						// HairGuides candidate is simply not in that set), instead of
						// that wiring only failing late in Job::AddHairGeometry.
						cd.keyword = "hair_guides"; cd.category = ChunkCategory::HairGuides;
						cd.description = "AN AUTHORED GUIDE-STRAND SET for `hair_geometry` (docs/HAIR_FUR_DESIGN.md section 5.3).  A handful of hand-placed strands that state the STYLE -- the sweep of a fringe, the lie of a coat, the flick of a tail -- which the groom then interpolates across its thousands of generated strands.  This is the chunk to reach for when `comb` / `gravity` / `curl` cannot say what you mean: those are FIELDS, uniform in their own terms, whereas a guide states one exact curve at one exact place.  "
							"IT IS DATA, NOT A SHAPE.  A guide set renders nothing, intersects nothing and is never bound to a `standard_object`; its only consumer is a `hair_geometry`'s `guides` field, and a set nothing references costs nothing.  "
							"AUTHOR GUIDES ON OR NEAR THE SURFACE THEY GROOM.  Each guide is attached to the base by projecting its ROOT (its first point) onto the base surface, and it is read in the surface's frame there -- that projection is what lets the same guide mean `swept back along the scalp` at every strand it later steers, including around curvature.  A guide floating far off the surface still attaches (to the nearest point), but it pays TWICE for the distance, not once: the projection makes a worse guess at the local frame the farther it is from the surface it's meant to describe, AND -- separately -- every strand's nearest-three SELECTION is by raw root-to-root distance (the guide's OWN authored root, not its surface projection), so a lofted guide's inverse-distance weight shrinks UNIFORMLY across the whole groom, diluting its influence everywhere rather than only where the framing is locally poor.  "
							"SCALE AND SHAPE ARE SEPARATE: a guide's own length is normalised away (the groom's `length` / `length_painter` set the real length), so guides may be authored at whatever size is convenient to type.  Point counts likewise -- a guide is resampled by arc-length fraction onto the groom's `segments`, so a 3-point guide and a 30-point guide both work and neither changes the strand's control-point count.  "
							"HOW MANY: tens to a few hundred, placed where the style CHANGES (crown, part, nape, ear).  Three guides already give a groom a real haircut; a hundred give it a hairstyle.  Capped at 4096, because every strand scans the whole set for its nearest three.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";  p.kind = ValueKind::String; p.description = "Unique name, referenced by a `hair_geometry`'s `guides` field"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "guide"; p.kind = ValueKind::String; p.repeatable = true; p.required = true;
						  p.description = "ONE WHOLE GUIDE on one line: `<x> <y> <z>` per point, root FIRST and tip LAST, at least 2 points (6 numbers) and at most 4096.  Repeatable -- one line per guide.  Points are in the same space as the base geometry.  The root is what gets projected onto the base to attach the guide, so put it on (or just above) the surface; every later point is free to go wherever the style does.  A guide whose points all coincide is refused: it has no direction to align and no length to normalise by.  Adding a mid-strand point is how a guide gets a kink -- but what SURVIVES resampling is the guide's SHAPE and a kink's position as a FRACTION of the guide's total arc length, not the literal spacing you typed between points: the polyline is resampled at UNIFORM arc-length fractions onto the groom's own control-point count, so a kink authored a third of the way along stays a third of the way along regardless of how its neighbouring points are spaced, while two guides with the same points but different spacing between them produce the identical interpolated strand"; }
						return cd;
					}();
					return d;
				}
			};

			//////////////////////////////////////////
			// Modifiers
			//////////////////////////////////////////

			struct NormalMapModifierAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name    = bag.GetString( "name",       "noname" );
					std::string painter = bag.GetString( "normal_map", "none" );
					double scale        = bag.GetDouble( "scale",      1.0 );
					return pJob.AddNormalMapModifier( name.c_str(), painter.c_str(), scale );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "normal_map_modifier"; cd.category = ChunkCategory::Modifier;
						cd.description = "Tangent-space normal-map modifier.  Samples a normal-map "
							"painter at the hit's TEXCOORD_0, decodes (2*RGB - 1) -> tangent-space "
							"normal, and reorients ri.vNormal using the imported per-vertex TANGENT "
							"(via the v3 ITriangleMeshGeometryIndexed3 storage) plus its bitangent "
							"sign.  Falls back to UV-derived dpdu/dpdv (silent, qualitatively "
							"correct on connected UV charts) when the source mesh has no TANGENT, "
							"and to ONB-derived tangents (one-time warning) when neither TANGENT "
							"nor valid derivatives are available.  Designed for glTF 2.0 "
							"normalTexture but works with any linear-RGB normal map.  IMPORTANT: "
							"the referenced painter MUST be loaded with NO colour-matrix conversion "
							"-- post Stage B colour-space migration (RISEPel == Rec709RGBPel) that "
							"means `color_space Rec709RGB_Linear`, which stores PNG bytes verbatim "
							"in the engine working space.  ROMMRGB_Linear would apply a Rec.709 → "
							"ROMM colour matrix that warps the encoded vector; sRGB would gamma-"
							"decode and break the [0,1] vector domain.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";       p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "normal_map"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Normal-map painter (load with `color_space Rec709RGB_Linear` for verbatim store; see chunk description)"; }
						{ auto& p = P(); p.name = "scale";      p.kind = ValueKind::Double;    p.description = "glTF normalTexture.scale (xy multiplier)"; p.defaultValueHint = "1.0"; }
						return cd;
					}();
					return d;
				}
			};

			struct ReliefModifierAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name   = bag.GetString( "name",   "noname" );
					std::string height = bag.GetString( "height", "none" );
					double scale       = bag.GetDouble( "scale",  1.0 );
					std::string domain = bag.GetString( "domain", "surface" );
					double step        = bag.GetDouble( "step",   0.0 );
					double maxSlope    = bag.GetDouble( "max_slope", 0.0 );

					// Domain validation, the `max_slope` range check, the
					// height resolution and its three-way scalar-pipe
					// diagnostic all live in Job::AddReliefModifierEx --
					// one home, so the CLI, the agent verbs and any future
					// caller get the same wording.  The `Ex` entry point,
					// not the plain one: IJob's vtable is append-only, so
					// the clamp arrived as a new tail virtual and the plain
					// `AddReliefModifier` now forwards here with 0.
					return pJob.AddReliefModifierEx( name.c_str(), height.c_str(), scale,
						domain.c_str(), step, maxSlope );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "relief_modifier"; cd.category = ChunkCategory::Modifier;
						cd.description = "Painter-driven micro-relief: perturbs the shading normal "
							"from the gradient of ANY scalar height field, by central difference in "
							"the hit's tangent plane.  Works on ANY geometry that has a normal -- "
							"analytic primitives, SDFs, meshes, displaced meshes -- and in the "
							"default `surface` domain needs NO TEXCOORDS at all, so the same 3D "
							"field that drives a colour ramp and a roughness slot can also emboss "
							"the surface (the fix for authored variation that reads as paint on "
							"plastic).  `height` is a scalar_painter, not a colour painter: wrap a "
							"colour painter with `scalar_painter { name X_h  painter X  channel R }` "
							"and bind that.  POSITIVE HEIGHT RISES ALONG +N (Blinn / PBRT-v4) -- the "
							"OPPOSITE sign of the REMOVED `bumpmap_modifier` (removed 2026-09-06), which treated its "
							"field as depth; negate `scale` to sink instead of raise.  On a FINE field, raise "
							"`scale` for legibility and set `max_slope` (0.5-1.0) to hold the tilt -- an unbounded "
							"tilt shades BLACK, see that parameter.  Attach via "
							"the object's `modifier` parameter.  See "
							"docs/RELIEF_MODIFIER_DESIGN.md.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";   p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "height"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter};
						  p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true;
						  p.description = "Height field -- a `scalar_painter` (expression / voronoi / ramp / texture / function2d), or an inline numeric literal (which is constant, hence flat and pointless).  A COLOUR painter bound here is refused with the standing scalar-pipe diagnostic: wrap it as `scalar_painter { name X_h  painter X  channel R }`.  Height is a LENGTH, not a colour, so it must never pass through JH spectral uplift -- which is exactly what the scalar pipe guarantees.  NOT SUPPORTED: a height parameterised on the SECOND UV set (`texcoord1_painter`, TEXCOORD_1).  Neither domain moves `ptCoord1` -- the surface domain's chain rule has only TEXCOORD_0's dpdu/dpdv to work with -- so such a field reads FLAT and produces no relief in either domain.  Author the height against TEXCOORD_0, or use a 3D field in `surface`."; }
						{ auto& p = P(); p.name = "scale";  p.kind = ValueKind::Double;    p.description = "Amplitude: field units -> world units in `surface` domain, UV units in `uv`.  Positive raises along +N; NEGATIVE sinks (cracks, pores, engraving).  0 makes the modifier inert."; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "domain"; p.kind = ValueKind::Enum;      p.enumValues = {"surface","uv"};
						  p.description = "`surface` (RECOMMENDED, and the default): the height is a function of the 3D hit and the step is taken in the tangent plane in world units -- any geometry with a normal, texcoords NOT required, and the result does not depend on which tangent the frame happened to pick.  `uv`: the height is a function of (u,v) and the step is taken in texture units along the ONB tangents -- for scenes migrated off the removed `bumpmap_modifier` (tools/migrate_scenes_relief.py) and for image heightfields authored in UV, and it inherits that path's dependence on the surface's UV parameterisation."; p.defaultValueHint = "surface"; }
						{ auto& p = P(); p.name = "step";   p.kind = ValueKind::Double;    p.description = "Central-difference HALF-step.  In `uv` it is used as given (default 0.01, matching the removed `bumpmap_modifier`'s windowsize, so a migrated scene that omitted the window lands on the same span).  In `surface` the rule DEFERS to the pixel footprint whenever the hit has one (2026-09-06 fix): the step is max(step, HALF the hit's pixel footprint), so an EXPLICIT step is a FLOOR that is raised whenever half the footprint is larger, and `step 0` resolves to exactly half the footprint -- however small that is, with NO 1e-3 minimum.  Half, because this is the HALF-step and the difference spans twice it: the stencil then spans exactly ONE footprint.  Differencing over at least a footprint measures the footprint-averaged slope, so relief fades toward flat at distance instead of sparkling -- but only where a footprint exists.  EVERY geometry populates one -- analytic primitives, SDFs, boxes, disks, planes and meshes alike -- but only on PRIMARY hits, since no ray carries screen-space differentials after a scatter.  So the fade is universal on directly-visible surfaces; on a hit reached through a bounce the footprint is unknown (not just small -- ABSENT), and there the rule is `step > 0 ? step : 1e-3` with no distance fade -- the 1e-3 floor applies ONLY in this no-footprint case, never as a competitor to a real, smaller footprint.  Practical effect: a close-up primary hit whose footprint is below 2e-3 world units used to still floor at a 2mm-wide stencil regardless of how tight the footprint was; now `step 0` tracks the footprint down to whatever it measures, so millimetre-scale relief on a close-up shot no longer needs an explicit sub-1e-3 `step` to avoid being smeared -- author it only for secondary/bounce hits, or to raise the floor above what the footprint alone would give."; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "max_slope"; p.kind = ValueKind::Double; p.description = "Upper bound on the tangent-plane tilt |scale*grad h| (a SLOPE: 1.0 = 45 degrees, 0.577 = 30).  When the scaled gradient exceeds it, the gradient is rescaled to this magnitude with its DIRECTION PRESERVED -- the relief keeps facing the way the field points and only stops leaning further, which is what makes this different from lowering `scale` (that flattens the shallow parts of the field along with the steep ones).  WHAT IT PREVENTS: an unbounded tilt takes the shading normal past the GEOMETRIC HORIZON as seen from the ray or the light -- it never crosses the surface plane itself, the perturbation is perpendicular to N -- and the materials' geometric-horizon gate then rejects nearly every sampled direction, so the surface shades BLACK (bands and speckle on a fine field).  So: RAISE `scale` for legibility and let this hold the tilt, rather than dialling `scale` down until the black goes away, which puts the detail back below the pixel footprint.  0.5-1.0 is the useful band on a near-face-on surface; on a surface seen at grazing angle phi the tilt budget before N' faces away from the ray shrinks to roughly tan(phi) -- weathered_workbench's grazing-viewed bench top needed 0.30, not 0.5-1.0 (docs/RELIEF_MODIFIER_DESIGN.md section 12 addendum).  0 = no clamp (the legacy/migration behaviour, and the default).  A negative or non-finite value is a parse-time error naming this parameter."; p.defaultValueHint = "0"; }
						return cd;
					}();
					return d;
				}
			};

			struct ModifierStackAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );

					// GetRepeatable's precedent is standard_shader's `shaderop` --
					// same ValueKind::Reference + repeatable=true pattern, same
					// "all values, in input order" accessor.  An empty result
					// (no `modifier` lines authored) is diagnosed by
					// Job::AddModifierStack, one home for the wording so the
					// CLI, the agent verbs and any future caller agree.
					const std::vector<std::string>& mods = bag.GetRepeatable( "modifier" );
					const unsigned int num = static_cast<unsigned int>( mods.size() );

					char* modmem = new char[num > 0 ? num*256 : 1];
					if( num > 0 ) { memset( modmem, 0, num*256 ); }
					char** modptrs = new char*[num > 0 ? num : 1];

					for( unsigned int i = 0; i < num; i++ ) {
						modptrs[i] = &modmem[i*256];
						strncpy( modptrs[i], mods[i].c_str(), 255 );
					}

					bool bRet = pJob.AddModifierStack( name.c_str(), (const char**)modptrs, num );

					delete [] modptrs;
					delete [] modmem;

					return bRet;
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "modifier_stack"; cd.category = ChunkCategory::Modifier;
						cd.description = "ORDERED COMPOSITION of other modifiers: `Object::pModifier` is a single "
							"pointer, so a single object could bind `normal_map_modifier` OR `relief_modifier` OR "
							"`glint_modifier` but never a combination -- this chunk is the smallest fix.  Each "
							"member is applied in AUTHORED ORDER and sees the PREVIOUS member's vNormal/onb, "
							"exactly as if the object's modifier slot held a hand-written chain of `Modify` calls.  "
							"ORDER SEMANTICS: `normal_map` then `relief` -- relief perturbs the NORMAL-MAPPED "
							"frame (fine procedural detail on top of a baked map, the usual case).  `relief` then "
							"`normal_map` -- the map is decoded in the RELIEF-TILTED frame; rarely wanted.  "
							"`... then glint` -- glint should always be LAST: it replaces the normal with a facet "
							"normal drawn about the CURRENT one, so it must see the final smooth frame.  `relief` "
							"twice with different height fields is legitimate (a coarse+fine two-frequency split).  "
							"NESTING is allowed -- a stack may name another stack -- and is algebraically flat: "
							"`stack{A, stack{B,C}}` applies A, B, C in that order, identically to `stack{A,B,C}`.  "
							"SELF-REFERENCE IS IMPOSSIBLE: member names resolve through the modifier manager at "
							"PARSE time, before this stack itself is registered, so there is no cycle to detect.  "
							"An EMPTY stack (no `modifier` lines) is a parse-time ERROR, not a no-op -- matching "
							"`glint_modifier`'s stance that an authored no-op chunk is a mistake.  Attach via the "
							"object's `modifier` parameter, exactly like any other modifier.  See "
							"docs/RELIEF_MODIFIER_DESIGN.md section 4.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";     p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "modifier"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Modifier}; p.repeatable = true; p.required = true;
						  p.description = "Modifier to apply, in order (repeatable) -- at least one is required; an empty stack is a parse error."; }
						return cd;
					}();
					return d;
				}
			};

			struct GlintModifierAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					double density   = bag.GetDouble( "density",  5.0 );
					double coverage  = bag.GetDouble( "coverage", 0.15 );
					double fill      = bag.GetDouble( "fill",     0.6 );
					double spread    = bag.GetDouble( "spread",   2.0 );
					double scale[3]  = { 1.0, 1.0, 1.0 };
					double shift[3]  = { 0.0, 0.0, 0.0 };
					bag.GetVec3( "scale", scale );
					bag.GetVec3( "shift", shift );
					unsigned int seed = bag.GetUInt( "seed", 0 );

					// The descriptor layer already hard-rejects non-finite /
					// non-numeric tokens (AllTokensAreFiniteNumbers) -- the
					// load-bearing gate for this chunk (the modifier ctor's
					// laundered backstop only goes silently inert).  Here we
					// add DOMAIN diagnostics: values that make the modifier
					// pointless fail LOUDLY (an authored no-op chunk is a
					// mistake, not an intent), and out-of-range fractions
					// warn about the clamp they will receive.
					if( !(density > 0.0) || !(coverage > 0.0) || !(fill > 0.0) || !(spread > 0.0) ) {
						GlobalLog()->PrintEx( eLog_Error,
							"glint_modifier `%s`: density/coverage/fill/spread must all be > 0 (got %g/%g/%g/%g) -- these values would make the modifier inert",
							name.c_str(), density, coverage, fill, spread );
						return false;
					}
					if( coverage > 1.0 ) {
						GlobalLog()->PrintEx( eLog_Warning,
							"glint_modifier `%s`: coverage %g clamps to 1.0", name.c_str(), coverage );
					}
					if( fill > 1.0 ) {
						GlobalLog()->PrintEx( eLog_Warning,
							"glint_modifier `%s`: fill %g clamps to 1.0 (facet radius may not exceed the half-cell)", name.c_str(), fill );
					}
					if( !(scale[0] > 0.0) || !(scale[1] > 0.0) || !(scale[2] > 0.0) ) {
						GlobalLog()->PrintEx( eLog_Error,
							"glint_modifier `%s`: scale components must be > 0 (got %g %g %g)",
							name.c_str(), scale[0], scale[1], scale[2] );
						return false;
					}

					return pJob.AddGlintModifier( name.c_str(), density, coverage, fill, spread, scale, shift, seed );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "glint_modifier"; cd.category = ChunkCategory::Modifier;
						cd.description = "Discrete-facet glint modifier: an object-space cell hash "
							"places sparse mirror-like micro-facets (jittered centre + radius test, "
							"per-cell existence probability); a hit landing on a facet has its "
							"shading normal replaced by the facet's Rayleigh-tilted normal, so the "
							"object's EXISTING material twinkles as the view/light sweeps -- enamel "
							"flecks, metallic-flake paint, snow sparkle, glitter.  Facets are pinned "
							"to the surface (object-space hash): they flash because the geometry "
							"moves, never because the pattern swims.  Attach via the object's "
							"`modifier` parameter.  See docs/ENAMEL_SPARKLE_BRDF.md.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";     p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "density";  p.kind = ValueKind::Double;     p.description = "Cells per object-space unit; facet pitch = 1/density units.  Anchor to the physical fleck pitch (e.g. 180um flecks on a dial with ~1.08 units/mm => density ~5)"; p.defaultValueHint = "5.0"; }
						{ auto& p = P(); p.name = "coverage"; p.kind = ValueKind::Double;     p.description = "Per-cell facet existence probability (0,1]; with `fill` this sets the areal facet fraction"; p.defaultValueHint = "0.15"; }
						{ auto& p = P(); p.name = "fill";     p.kind = ValueKind::Double;     p.description = "Facet disc radius as a fraction of the half-cell (0,1]; the sphere-slice test varies apparent facet sizes naturally"; p.defaultValueHint = "0.6"; }
						{ auto& p = P(); p.name = "spread";   p.kind = ValueKind::Double;     p.description = "Facet tilt Rayleigh scale in DEGREES (single-digit typical; the twinkle's angular sensitivity)"; p.defaultValueHint = "2.0"; }
						{ auto& p = P(); p.name = "scale";    p.kind = ValueKind::DoubleVec3; p.description = "Anisotropic cell stretch (Worley convention pt*scale+shift); elongated cells give a streak lay"; p.defaultValueHint = "1 1 1"; }
						{ auto& p = P(); p.name = "shift";    p.kind = ValueKind::DoubleVec3; p.description = "Cell-space offset"; p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "seed";     p.kind = ValueKind::UInt;       p.description = "Hash seed -- distinct fleck fields on otherwise identical objects"; p.defaultValueHint = "0"; }
						return cd;
					}();
					return d;
				}
			};

			//////////////////////////////////////////
			// Participating media
			//////////////////////////////////////////

			struct HomogeneousMediumAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					double sigma_a[3] = {0,0,0};
					double sigma_s[3] = {0,0,0};
					bag.GetVec3( "absorption", sigma_a );
					bag.GetVec3( "scattering", sigma_s );

					// Optional per-wavelength coefficient curves (G1,
					// vitreous enamel): names of registered IFunction1D
					// (e.g. piecewise_linear_function) curves for the
					// spectral (NM) path.  BOTH empty => RGB-only (luminance
					// fallback), byte-identical to pre-G1.  If EITHER is
					// bound the NM path is curve-driven (an unbound
					// coefficient is zero in NM; Job warns on a non-zero
					// RGB sibling).
					std::string absorption_spectral = bag.GetString( "absorption_spectral", "" );
					std::string scattering_spectral = bag.GetString( "scattering_spectral", "" );

					// Composite phase token: "isotropic" or "hg <g>".
					std::string phase_type = "isotropic";
					double      phase_g    = 0.0;
					if( bag.Has( "phase" ) ) {
						std::string raw = bag.GetString( "phase" );
						char ptype[64] = {0};
						double g = 0.0;
						int n = sscanf( raw.c_str(), "%63s %lf", ptype, &g );
						if( n >= 1 ) phase_type = ptype;
						if( n >= 2 ) phase_g    = g;
					}

					if( !absorption_spectral.empty() || !scattering_spectral.empty() ) {
						const double emission[3] = { 0, 0, 0 };
						return pJob.AddHomogeneousMediumSpectral( name.c_str(), sigma_a, sigma_s, emission,
							absorption_spectral.c_str(), scattering_spectral.c_str(),
							phase_type.c_str(), phase_g );
					}

					return pJob.AddHomogeneousMedium( name.c_str(), sigma_a, sigma_s, phase_type.c_str(), phase_g );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "homogeneous_medium"; cd.category = ChunkCategory::Medium;
						cd.description = "Uniform participating medium.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";       p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "absorption"; p.kind = ValueKind::DoubleVec3; p.description = "Absorption coefficient (R G B), RGB/preview path"; p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "scattering"; p.kind = ValueKind::DoubleVec3; p.description = "Scattering coefficient (R G B), RGB/preview path"; p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "absorption_spectral"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Function}; p.description = "Name of a sigma_a(lambda) IFunction1D curve (e.g. piecewise_linear_function) for the spectral NM path.  Once EITHER spectral curve is bound the NM path is curve-driven: an unbound coefficient is ZERO in NM (bind both curves, or accept zero).  Only when BOTH are empty does NM fall back to RGB luminance"; }
						{ auto& p = P(); p.name = "scattering_spectral"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Function}; p.description = "Name of a sigma_s(lambda) IFunction1D curve for the spectral NM path.  Once EITHER spectral curve is bound the NM path is curve-driven: an unbound coefficient is ZERO in NM (bind both curves, or accept zero).  Only when BOTH are empty does NM fall back to RGB luminance"; }
						{ auto& p = P(); p.name = "phase";      p.kind = ValueKind::String;     p.description = "Phase function: either `isotropic` or `hg <g>` (Henyey-Greenstein with asymmetry g)"; p.tupleKinds = {ValueKind::Enum, ValueKind::Double}; p.enumValues = {"isotropic","hg"}; p.defaultValueHint = "isotropic"; }
						return cd;
					}();
					return d;
				}
			};

			struct GlobalMediumAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					return pJob.SetGlobalMedium( bag.GetString( "medium", "" ).c_str() );
				}
				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "global_medium"; cd.category = ChunkCategory::Medium;
						cd.description = "Sets the scene's global participating medium to a previously-added medium (the v7 form of `> set global_medium`).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "medium"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Medium}; p.description = "Name of a previously-added medium"; }
						return cd;
					}();
					return d;
				}
			};

			struct LightRRThresholdAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					return pJob.SetLightSampleRRThreshold( bag.GetDouble( "threshold", 0.0 ) );
				}
				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "light_rr_threshold"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "Sets the light-sample Russian-roulette threshold (the v7 form of `> set light_rr_threshold`).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "threshold"; p.kind = ValueKind::Double; p.description = "Light-sample RR threshold (0 disables RR)"; }
						return cd;
					}();
					return d;
				}
			};

			struct HeterogeneousMediumAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					double max_sigma_a[3] = {0,0,0};
					double max_sigma_s[3] = {0,0,0};
					double emission[3]    = {0,0,0};
					bag.GetVec3( "absorption", max_sigma_a );
					bag.GetVec3( "scattering", max_sigma_s );
					bag.GetVec3( "emission",   emission );

					// Composite phase token: "isotropic" or "hg <g>".
					std::string phase_type = "isotropic";
					double      phase_g    = 0.0;
					if( bag.Has( "phase" ) ) {
						std::string raw = bag.GetString( "phase" );
						char ptype[64] = {0};
						double g = 0.0;
						int n = sscanf( raw.c_str(), "%63s %lf", ptype, &g );
						if( n >= 1 ) phase_type = ptype;
						if( n >= 2 ) phase_g    = g;
					}

					std::string volume_pattern = bag.GetString( "volume_pattern", "" );
					unsigned int vol_width  = bag.GetUInt( "volume_width",  0 );
					unsigned int vol_height = bag.GetUInt( "volume_height", 0 );
					unsigned int vol_startz = bag.GetUInt( "volume_startz", 0 );
					unsigned int vol_endz   = bag.GetUInt( "volume_endz",   0 );

					std::string accStr = bag.GetString( "accessor", "t" );
					char accessor = accStr.empty() ? 't' : accStr[0];

					double bbox_min[3] = {0,0,0};
					double bbox_max[3] = {0,0,0};
					bag.GetVec3( "bbox_min", bbox_min );
					bag.GetVec3( "bbox_max", bbox_max );

					if( volume_pattern.empty() || vol_width == 0 || vol_height == 0 ) {
						GlobalLog()->PrintEasyError( "HeterogeneousMedium:: volume_pattern, volume_width, and volume_height are required" );
						return false;
					}

					if( vol_endz < vol_startz ) {
						GlobalLog()->PrintEasyError( "HeterogeneousMedium:: volume_endz must be >= volume_startz" );
						return false;
					}

					return pJob.AddHeterogeneousMedium( name.c_str(),
						max_sigma_a, max_sigma_s, emission, phase_type.c_str(), phase_g,
						volume_pattern.c_str(), vol_width, vol_height, vol_startz, vol_endz,
						accessor, bbox_min, bbox_max );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "heterogeneous_medium"; cd.category = ChunkCategory::Medium;
						cd.description = "Voxel-grid heterogeneous participating medium loaded from a raw volume pattern.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";           p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "absorption";     p.kind = ValueKind::DoubleVec3; p.description = "Max absorption coefficient (R G B)"; }
						{ auto& p = P(); p.name = "scattering";     p.kind = ValueKind::DoubleVec3; p.description = "Max scattering coefficient (R G B)"; }
						{ auto& p = P(); p.name = "emission";       p.kind = ValueKind::DoubleVec3; p.description = "Volumetric emission (R G B)"; }
						{ auto& p = P(); p.name = "phase";          p.kind = ValueKind::String;     p.description = "Phase function: either `isotropic` or `hg <g>` (Henyey-Greenstein with asymmetry g)"; p.tupleKinds = {ValueKind::Enum, ValueKind::Double}; p.enumValues = {"isotropic","hg"}; p.defaultValueHint = "isotropic"; }
						{ auto& p = P(); p.name = "volume_pattern"; p.kind = ValueKind::String;     p.description = "Volume file pattern (printf-style)"; }
						{ auto& p = P(); p.name = "volume_width";   p.kind = ValueKind::UInt;       p.description = "Volume width"; }
						{ auto& p = P(); p.name = "volume_height";  p.kind = ValueKind::UInt;       p.description = "Volume height"; }
						{ auto& p = P(); p.name = "volume_startz";  p.kind = ValueKind::UInt;       p.description = "Start slice index"; }
						{ auto& p = P(); p.name = "volume_endz";    p.kind = ValueKind::UInt;       p.description = "End slice index"; }
						{ auto& p = P(); p.name = "accessor";       p.kind = ValueKind::String;     p.description = "Voxel accessor type (first character: 'n', 't', or 'c')"; p.defaultValueHint = "t"; }
						{ auto& p = P(); p.name = "bbox_min";       p.kind = ValueKind::DoubleVec3; p.description = "World-space bbox min"; }
						{ auto& p = P(); p.name = "bbox_max";       p.kind = ValueKind::DoubleVec3; p.description = "World-space bbox max"; }
						return cd;
					}();
					return d;
				}
			};


			struct PainterHeterogeneousMediumAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					double max_sigma_a[3] = {0,0,0};
					double max_sigma_s[3] = {0,0,0};
					double emission[3]    = {0,0,0};
					bag.GetVec3( "absorption", max_sigma_a );
					bag.GetVec3( "scattering", max_sigma_s );
					bag.GetVec3( "emission",   emission );

					// Composite phase token: "isotropic" or "hg <g>".
					std::string phase_type = "isotropic";
					double      phase_g    = 0.0;
					if( bag.Has( "phase" ) ) {
						std::string raw = bag.GetString( "phase" );
						char ptype[64] = {0};
						double g = 0.0;
						int n = sscanf( raw.c_str(), "%63s %lf", ptype, &g );
						if( n >= 1 ) phase_type = ptype;
						if( n >= 2 ) phase_g    = g;
					}

					std::string density_painter = bag.GetString( "density_painter", "none" );
					unsigned int resolution     = bag.GetUInt(   "resolution",      64 );

					char color_to_scalar = 'l';
					if( bag.Has( "color_to_scalar" ) ) {
						std::string c2s = bag.GetString( "color_to_scalar" );
						if( c2s == "luminance" ) {
							color_to_scalar = 'l';
						} else if( c2s == "max" ) {
							color_to_scalar = 'm';
						} else if( c2s == "red" ) {
							color_to_scalar = 'r';
						} else {
							GlobalLog()->PrintEx( eLog_Error, "PainterHeterogeneousMedium:: Unknown color_to_scalar `%s`, using luminance", c2s.c_str() );
							color_to_scalar = 'l';
						}
					}

					double bbox_min[3] = {0,0,0};
					double bbox_max[3] = {0,0,0};
					bag.GetVec3( "bbox_min", bbox_min );
					bag.GetVec3( "bbox_max", bbox_max );

					if( density_painter == "none" ) {
						GlobalLog()->PrintEasyError( "PainterHeterogeneousMedium:: density_painter is required" );
						return false;
					}

					if( resolution == 0 ) {
						GlobalLog()->PrintEasyError( "PainterHeterogeneousMedium:: resolution must be > 0" );
						return false;
					}

					return pJob.AddPainterHeterogeneousMedium( name.c_str(),
						max_sigma_a, max_sigma_s, emission, phase_type.c_str(), phase_g,
						density_painter.c_str(), resolution, color_to_scalar,
						bbox_min, bbox_max );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "painter_heterogeneous_medium"; cd.category = ChunkCategory::Medium;
						cd.description = "Heterogeneous medium whose density field comes from a painter evaluation.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";            p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "absorption";      p.kind = ValueKind::DoubleVec3; p.description = "Max absorption coefficient (R G B)"; }
						{ auto& p = P(); p.name = "scattering";      p.kind = ValueKind::DoubleVec3; p.description = "Max scattering coefficient (R G B)"; }
						{ auto& p = P(); p.name = "emission";        p.kind = ValueKind::DoubleVec3; p.description = "Volumetric emission (R G B)"; }
						{ auto& p = P(); p.name = "phase";           p.kind = ValueKind::String;     p.description = "Phase function: either `isotropic` or `hg <g>` (Henyey-Greenstein with asymmetry g)"; p.tupleKinds = {ValueKind::Enum, ValueKind::Double}; p.enumValues = {"isotropic","hg"}; p.defaultValueHint = "isotropic"; }
						{ auto& p = P(); p.name = "density_painter"; p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "Density painter"; }
						{ auto& p = P(); p.name = "resolution";      p.kind = ValueKind::UInt;       p.description = "Voxel resolution"; p.defaultValueHint = "64"; }
						{ auto& p = P(); p.name = "color_to_scalar"; p.kind = ValueKind::Enum;       p.enumValues = {"luminance","max","red"}; p.description = "RGB→scalar rule"; p.defaultValueHint = "luminance"; }
						{ auto& p = P(); p.name = "bbox_min";        p.kind = ValueKind::DoubleVec3; p.description = "World-space bbox min"; }
						{ auto& p = P(); p.name = "bbox_max";        p.kind = ValueKind::DoubleVec3; p.description = "World-space bbox max"; }
						return cd;
					}();
					return d;
				}
			};


			//////////////////////////////////////////
			// Objects
			//////////////////////////////////////////

			struct StandardObjectAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name     = bag.GetString( "name",     "noname" );

					// 87 step 3a: a node carries at most ONE of `geometry` (a leaf
					// shape) and `source` (an instance of another node).  Count the
					// forms and refuse BEFORE any mutation, exactly as sweep_geometry
					// does for profile_point / profile_circle / profile_rect -- with
					// the one difference that ZERO forms is LEGAL here: that is the
					// CONTAINER node.
					const bool hasGeometryForm = bag.Has( "geometry" ) && bag.GetString( "geometry", "none" ) != "none";
					const bool hasSourceForm   = bag.Has( "source" )   && bag.GetString( "source",   "none" ) != "none";
					if( hasGeometryForm && hasSourceForm ) {
						const std::string diag = "standard_object `" + name + "`: `geometry` and `source` are mutually "
							"exclusive -- a node is EITHER a leaf shape (`geometry`) OR an instance of another node "
							"(`source`), never both.  Drop one.";
						GlobalLog()->PrintEx( eLog_Error, "%s", diag.c_str() );
						if( RISE::g_cstFinalizeDiagSink ) *RISE::g_cstFinalizeDiagSink = diag;
						return false;
					}
					// A LONE `source` must never reach here: Cst::DeriveToJob EXPANDS it
					// at the instancing chunk's own position in PASS-2 and hands this
					// Finalize a bag with `source` already consumed.  Arriving with one
					// still on the bag means the chunk was applied by a path that does
					// NOT expand instances -- which would silently build a CONTAINER
					// where the author asked for a copy.  Refuse loudly instead.
					// (DeriveToJobIncremental refuses such a chunk up front, so this is
					// a backstop against a future third apply path, not a live case.)
					if( hasSourceForm ) {
						const std::string diag = "standard_object `" + name + "`: `source` reached the parser UNEXPANDED "
							"-- this chunk was applied by a path that does not expand instances (Cst::DeriveToJob PASS-2 is "
							"the only one that does).  Re-derive the whole scene.";
						GlobalLog()->PrintEx( eLog_Error, "%s", diag.c_str() );
						if( RISE::g_cstFinalizeDiagSink ) *RISE::g_cstFinalizeDiagSink = diag;
						return false;
					}
					// 87 step 3c: `count_u` / `count_v` REPEAT AN INSTANCE, so they mean
					// nothing without a `source` to repeat.  Unlike the backstop above this
					// one is genuinely REACHABLE: a chunk with counts and no `source` is not
					// an instancing chunk, so PASS-2 hands it to this Finalize like any
					// other object.  Silently ignoring the counts would derive ONE object
					// from a chunk the author wrote expecting N.
					if( bag.Has( "count_u" ) || bag.Has( "count_v" ) ) {
						const std::string diag = "standard_object `" + name + "`: `count_u` / `count_v` repeat an "
							"INSTANCE, so they need a `source` naming the node to repeat.  On a chunk with a "
							"`geometry` (or with neither) there is nothing to repeat, and the counts would be "
							"silently ignored.  Add a `source`, or drop the counts.";
						GlobalLog()->PrintEx( eLog_Error, "%s", diag.c_str() );
						if( RISE::g_cstFinalizeDiagSink ) *RISE::g_cstFinalizeDiagSink = diag;
						return false;
					}

					// 87: `geometry` is OPTIONAL.  Absent (or the universal
					// no-reference sentinel `none`) means this object is a
					// CONTAINER node -- a pure transform other objects parent
					// to.  Job::AddObject turns that into a geometry-less,
					// world-INVISIBLE object.  NB this is a real semantic
					// change: before 87 an object chunk with no `geometry`
					// hard-failed with "Geometry not found `none`".
					std::string geometry = bag.GetString( "geometry", "none" );
					std::string parent   = bag.GetString( "parent",   "" );
					std::string material = bag.GetString( "material", "none" );
					std::string modifier = bag.GetString( "modifier", "none" );
					std::string shader   = bag.GetString( "shader",   "none" );
					std::string interior_medium = bag.GetString( "interior_medium", "none" );

					RadianceMapConfig radianceMapConfig;
					if( bag.Has( "radiance_map" ) )    radianceMapConfig.name  = bag.GetString( "radiance_map" ).c_str();
					if( bag.Has( "radiance_scale" ) )  radianceMapConfig.scale = bag.GetDouble( "radiance_scale" );
					if( bag.GetVec3( "radiance_orient", radianceMapConfig.orientation ) ) {
						radianceMapConfig.orientation[0] *= DEG_TO_RAD;
						radianceMapConfig.orientation[1] *= DEG_TO_RAD;
						radianceMapConfig.orientation[2] *= DEG_TO_RAD;
					}

					bool bCastsShadows    = bag.GetBool( "casts_shadows",    true );
					bool bReceivesShadows = bag.GetBool( "receives_shadows", true );

					// Transform precedence (highest first): matrix > quaternion > orientation (Euler).
					// `position` and `scale` apply alongside quaternion / orientation but are subsumed
					// by `matrix`, since a 4x4 already encodes translation + rotation + scale.
					const bool hasMatrix     = bag.Has( "matrix" );
					const bool hasQuaternion = bag.Has( "quaternion" );
					const bool hasEuler      = bag.Has( "orientation" );

					if( hasMatrix && (hasQuaternion || hasEuler) ) {
						GlobalLog()->PrintEx( eLog_Warning,
							"standard_object `%s`: `matrix` overrides `quaternion` / `orientation` "
							"(matrix already encodes the full transform)", name.c_str() );
					} else if( hasQuaternion && hasEuler ) {
						GlobalLog()->PrintEx( eLog_Warning,
							"standard_object `%s`: `quaternion` overrides `orientation` (Euler decomposition is lossy)",
							name.c_str() );
					}

					bool bRet = false;
					if( hasMatrix ) {
						double mat[16];
						bag.GetMat4( "matrix", mat );
						bRet = pJob.AddObjectMatrix(
							name.c_str(), geometry.c_str(),
							material=="none"?0:material.c_str(),
							modifier=="none"?0:modifier.c_str(),
							shader=="none"?0:shader.c_str(),
							radianceMapConfig, mat,
							bCastsShadows, bReceivesShadows );
					} else if( hasQuaternion ) {
						// Compose translation, quaternion rotation, and per-axis scale into a
						// single column-major 4x4, then call AddObjectMatrix.  Using
						// AddObject with Euler-from-quaternion would re-introduce the
						// gimbal-lock loss this parameter exists to avoid.  Math is shared
						// with OverrideObjectAsciiChunkParser (Phase 6.2) via
						// ComposeTRS_QuaternionGltf (§8.10).
						double pos[3]   = {0,0,0};
						double q[4]     = {0,0,0,1};	// xyzw, glTF convention
						double scale[3] = {1,1,1};
						bag.GetVec3( "position",   pos );
						bag.GetVec4( "quaternion", q );
						bag.GetVec3( "scale",      scale );

						double M[16];
						ComposeTRS_QuaternionGltf( pos, q, scale, M );

						bRet = pJob.AddObjectMatrix(
							name.c_str(), geometry.c_str(),
							material=="none"?0:material.c_str(),
							modifier=="none"?0:modifier.c_str(),
							shader=="none"?0:shader.c_str(),
							radianceMapConfig, M,
							bCastsShadows, bReceivesShadows );
					} else {
						double pos[3]    = {0,0,0};
						double orient[3] = {0,0,0};
						double scale[3]  = {1.0,1.0,1.0};
						bag.GetVec3( "position", pos );
						if( bag.GetVec3( "orientation", orient ) ) {
							orient[0] *= DEG_TO_RAD;
							orient[1] *= DEG_TO_RAD;
							orient[2] *= DEG_TO_RAD;
						}
						bag.GetVec3( "scale", scale );

						bRet = pJob.AddObject( name.c_str(), geometry.c_str(),
							material=="none"?0:material.c_str(),
							modifier=="none"?0:modifier.c_str(),
							shader=="none"?0:shader.c_str(),
							radianceMapConfig, pos, orient, scale, bCastsShadows, bReceivesShadows );
					}

					// A CONTAINER has no interior either -- it has no surface to be
					// inside of.  Job::AddObject drops the other four surface
					// bindings; `interior_medium` comes through a separate call, so
					// it is skipped here, keeping "a container takes no surface
					// bindings" literally true.
					const bool isContainer = ( geometry.empty() || geometry == "none" );
					if( bRet && !isContainer && !(interior_medium == "none") ) {
						bRet = pJob.SetObjectInteriorMedium( name.c_str(), interior_medium.c_str() );
					} else if( bRet && isContainer && !(interior_medium == "none") ) {
						GlobalLog()->PrintEx( eLog_Warning,
							"standard_object `%s`: names no `geometry`, so it is a CONTAINER node -- its "
							"`interior_medium` binding is IGNORED (a container has no surface to be inside of)",
							name.c_str() );
					}

					// doc 89 slice C: the LOCAL MIRROR.  ALWAYS called, mirror line or
					// not, for the reason `parent` is (below): this Finalize also runs on
					// an INCREMENTAL re-apply, where the edit may have DELETED the
					// `mirror` line, and skipping the call would leave the object
					// rendering reflected until a save + reload silently un-reflected it.
					//
					// LEGAL ON A CONTAINER, unlike `interior_medium` above, and the
					// asymmetry is not an oversight.  `interior_medium` on a container is
					// WARNED-AND-IGNORED there (the chunk still derives) because a
					// container has no SURFACE to be inside of -- it is a binding with no
					// referent.  A mirror is a TRANSFORM, and a container
					// is nothing BUT a transform: `position`, `orientation`, `scale` and
					// `matrix` are all honoured on one, and every object parented under it
					// composes through the result.  Refusing the reflection alone would
					// mean the one transform an assembly most wants (mirror the whole
					// left arm into a right arm, in one edit) is the one it cannot have,
					// while `scale -1 1 1` -- the same negative-determinant matrix, spelled
					// obscurely -- kept working.  So: children compose through it, exactly
					// once, and that IS the feature.
					if( bRet && !pJob.SetObjectMirror( name.c_str(), bag.GetString( "mirror", "none" ).c_str() ) ) {
						const std::string diag = "standard_object `" + name + "`: `mirror " +
							bag.GetString( "mirror", "none" ) + "` -- the axis must be `x`, `y` or `z` "
							"(lower case).  A mirror reflects the object across the plane through its OWN "
							"origin perpendicular to that axis.";
						GlobalLog()->PrintEx( eLog_Error, "%s", diag.c_str() );
						if( RISE::g_cstFinalizeDiagSink ) *RISE::g_cstFinalizeDiagSink = diag;
						bRet = false;
					}

					// 87 recursive scene graph: record the parent LINK.  Nothing
					// is composed here -- `world = parent.world * local` is baked
					// by the derive's tail walk -- which is what makes editing a
					// container an ordinary one-chunk param edit, and is what
					// will make hierarchical animation fall out for free once
					// the per-frame re-bake lands (87 step 2; not yet).
					//
					// A refused link FAILS the chunk rather than silently
					// dropping the object out of its tree.  IJob::SetObjectParent
					// logs the SPECIFIC reason (undeclared parent, self-parent,
					// cycle, CSG operand at either end); the diagnostic sink below
					// cannot reach it -- the bool return is all that comes back --
					// so it enumerates the whole refusal set and points at the log
					// line.  An earlier wording asserted three conditions as if
					// they were the only ones, which read as a lie whenever the
					// cause was the fourth.
					// ALWAYS call, including with no parent: this Finalize also runs
					// on an INCREMENTAL re-apply, where the edit may have REMOVED
					// the `parent` line.  Skipping the call in that case would
					// leave the old link in the authored graph -- the object would
					// keep rendering under a parent its own chunk no longer names,
					// until a save + reload silently moved it.  `parent none` is
					// the explicit form of the same request, matching `geometry
					// none`'s "no reference" sentinel.
					const bool wantsParent = !parent.empty() && parent != "none";
					if( bRet ) {
						if( !pJob.SetObjectParent( name.c_str(), wantsParent ? parent.c_str() : 0 ) && wantsParent ) {
							if( RISE::g_cstFinalizeDiagSink ) {
								*RISE::g_cstFinalizeDiagSink = "standard_object `" + name + "`: `parent " + parent +
									"` was refused.  A `parent` must be a DECLARED-EARLIER object; must not be this "
									"object; must not already be one of its descendants; and must not be a CSG "
									"operand (parent the csg_object instead).  The log line immediately above names "
									"WHICH of those it was.";
							}
							bRet = false;
						}
					}

					return bRet;
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "standard_object"; cd.category = ChunkCategory::Object;
						cd.description = "Scene-graph node.  With a `geometry` it is a LEAF shape (with material, "
							"modifier and shader); with no `geometry` it is a pure CONTAINER -- a transform that "
							"other objects are parented to, invisible to the renderer itself.  `parent` names "
							"another object, declared EARLIER in the file, whose transform this one composes "
							"into: the node's world transform is `parent.world * local`, so moving a parent moves "
							"its whole subtree.  Nesting is arbitrary; a cycle is refused.  "
							"Transform precedence: `matrix` > `quaternion` > `orientation` (Euler).  "
							"`matrix` (16 doubles, column-major) bypasses the position / orientation / scale "
							"composition entirely; `quaternion` (xyzw, glTF convention) replaces Euler "
							"rotation but still composes with `position` and `scale`.  All of them describe "
							"the node's LOCAL transform, relative to its parent.  "
							"`source` instead of `geometry` makes the node an INSTANCE of another node's "
							"subtree -- see that parameter.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";             p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "geometry";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Geometry}; p.description = "Geometry to instance; omit for a pure container node"; }
						{ auto& p = P(); p.name = "source";           p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Object}; p.description = "Object (declared EARLIER) to INSTANCE: this node takes a copy of that object's bindings -- geometry / material / modifier / shader / radiance map / interior medium / shadow flags -- while its OWN position, orientation and scale say where the copy goes.  If the source has CHILDREN its whole SUBTREE is copied too: each descendant becomes one further object named `<this name>.<that node's name>`, parented to the copy of its own parent, so the assembly arrives intact and moving this node moves all of it.  The source keeps rendering; `source` copies, it does not move or hide anything.  The whole subtree must be declared before this chunk.  Mutually exclusive with `geometry`"; }
						{ auto& p = P(); p.name = "count_u";          p.kind = ValueKind::UInt;      p.description = "REPEAT this instance `count_u` times along the first axis (87 step 3c).  Needs a `source`; refused without one.  Each repetition is a whole copy of the instance -- root plus subtree -- named `<this name>[i,j]` and `<this name>[i,j].<member>`.  Every OTHER parameter on this chunk may then be a PER-COMPONENT `expr(...)` over the instance variables `i`/`j` (the indices) and `u`/`v` (the same, normalized into [0,1]; 0 when the count is 1), e.g. `position expr(i*3) 0 0`.  The counts vary THIS chunk's own parameters only -- a member of the copied subtree is not per-instance variable.  PRESENCE selects the repeated form: `count_u 1` names its one entry `[0,0]`, it does not fall back to the plain name.  A fractional, negative, non-finite or > 1e6 count is refused (never rounded)"; p.defaultValueHint = "1"; }
						{ auto& p = P(); p.name = "count_v";          p.kind = ValueKind::UInt;      p.description = "Second axis of the `count_u` repetition (default 1).  Needs `count_u`"; p.defaultValueHint = "1"; }
						{ auto& p = P(); p.name = "parent";           p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Object}; p.description = "Object to parent this one to (must be declared earlier)"; }
						{ auto& p = P(); p.name = "material";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Material}; p.description = "Surface material"; }
						{ auto& p = P(); p.name = "modifier";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Modifier}; p.description = "Geometry modifier"; }
						{ auto& p = P(); p.name = "shader";           p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Shader}; p.description = "Shader override"; }
						{ auto& p = P(); p.name = "position";         p.kind = ValueKind::DoubleVec3;p.description = "Position, LOCAL to `parent` (world-space when unparented)"; p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "orientation";      p.kind = ValueKind::DoubleVec3;p.description = "Euler orientation (degrees)"; p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "quaternion";       p.kind = ValueKind::DoubleVec4;p.description = "Rotation quaternion (xyzw, glTF convention)"; p.defaultValueHint = "0 0 0 1"; }
						{ auto& p = P(); p.name = "matrix";           p.kind = ValueKind::DoubleMat4;p.description = "Full 4x4 transform, column-major, LOCAL to `parent` (overrides position/orientation/quaternion/scale)"; }
						{ auto& p = P(); p.name = "scale";            p.kind = ValueKind::DoubleVec3;p.description = "Per-axis scale"; p.defaultValueHint = "1 1 1"; }
						{ auto& p = P(); p.name = "mirror";           p.kind = ValueKind::Enum;      p.enumValues = {"x","y","z","none"}; p.description = "REFLECT this node across the plane through its OWN origin perpendicular to the named LOCAL axis -- author one wing, hand, fin or shoe and mirror the other instead of building both.  Applied INNERMOST, before this node's `position` / `orientation` / `scale`, so `mirror x  position 3 0 0` puts the reflected shape AT +3 (it does not move it to -3).  With `source` the whole cloned SUBTREE arrives reflected, which is the headline use: `standard_object { name right_wing  source left_wing  mirror x }` off an UN-mirrored `left_wing`; with `count_u` every repetition is mirrored.  `mirror` is INSTANCE-OWN, never inherited through `source` -- exactly like `position` / `orientation` / `scale`.  So if the SOURCE itself carries a `mirror`, a plain `source` copy DROPS it and comes out as the source's mirror image; repeat the same `mirror <axis>` on the copy to reproduce the source exactly (the derive warns when you have not).  Legal on a geometry-less CONTAINER too -- everything parented under it composes through the reflection, exactly once, so a mirrored arm's own children are not double-mirrored.  `none` (or omitting the line) means no mirror"; }
						{ auto& p = P(); p.name = "casts_shadows";    p.kind = ValueKind::Bool;      p.description = "Participates in shadow casting"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "receives_shadows"; p.kind = ValueKind::Bool;      p.description = "Receives shadows from other objects"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "radiance_map";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Per-object radiance map"; }
						{ auto& p = P(); p.name = "radiance_scale";   p.kind = ValueKind::Double;    p.description = "Radiance-map scale"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "radiance_orient";  p.kind = ValueKind::DoubleVec3;p.description = "Radiance-map orientation (degrees)"; p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "interior_medium";  p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Medium}; p.description = "Interior participating medium"; }
						return cd;
					}();
					return d;
				}
			};

			// Phase 6.2 (docs/ROUND_TRIP_SAVE_PLAN.md §8.9): applies
			// transform overrides to an already-declared object.  The
			// chunk MUST appear AFTER the target's standard_object;
			// missing target is a hard parse error.  Precedence
			// matrix > quaternion > orientation mirrors the
			// standard_object path.
			struct OverrideObjectAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					const std::string name = bag.GetString( "name", std::string() );
					if( name.empty() ) {
						GlobalLog()->PrintEasyError(
							"override_object: missing `name` field." );
						return false;
					}
					struct NumericArity { const char* key; int count; };
					const NumericArity numericArities[] = {
						{ "position", 3 }, { "orientation", 3 }, { "quaternion", 4 },
						{ "matrix", 16 }, { "scale", 3 }
					};
					for( const NumericArity& arity : numericArities ) {
						if( !HasExactNumericArity( bag, arity.key, arity.count ) ) {
							GlobalLog()->PrintEx( eLog_Error,
								"override_object `%s`: parameter `%s` requires exactly %d finite numbers.",
								name.c_str(), arity.key, arity.count );
							return false;
						}
					}
					IJobPriv* priv = dynamic_cast<IJobPriv*>( &pJob );
					if( !priv ) {
						GlobalLog()->PrintEasyError(
							"override_object: IJob does not expose IObjectManager (mock?)" );
						return false;
					}
					IObjectManager* objs = priv->GetObjects();
					if( !objs ) return false;
					IObjectPriv* obj = objs->GetItem( name.c_str() );
					if( !obj ) {
						GlobalLog()->PrintEx( eLog_Error,
							"override_object: target `%s` not found in scene.  "
							"Possible causes: (a) the override_object chunk appears "
							"BEFORE the chunk that creates the target — chunks are "
							"order-sensitive (§8.5); (b) the file was edited between "
							"save and reload and the standard_object/csg_object was "
							"deleted; (c) the target name has a typo or was renamed "
							"after the override was authored.",
							name.c_str() );
						return false;
					}

					// Precedence: matrix > quaternion > orientation
					// (mirrors StandardObjectAsciiChunkParser).
					const bool hasMatrix     = bag.Has( "matrix" );
					const bool hasQuaternion = bag.Has( "quaternion" );

					bool any = false;
					if( hasMatrix ) {
						double m[16];
						bag.GetMat4( "matrix", m );
						const Matrix4 transform = BuildMatrix4FromColumnMajor( m );
						if( Implementation::Transformable* concrete =
							dynamic_cast<Implementation::Transformable*>( obj ) ) {
							concrete->SetFinalTransformMatrix( transform );
						} else {
							obj->ClearAllTransforms();
							obj->PushTopTransStack( transform );
						}
						any = true;
					} else if( hasQuaternion ) {
						double pos[3]={0,0,0}, q[4]={0,0,0,1}, s[3]={1,1,1};
						bag.GetVec3( "position",   pos );
						bag.GetVec4( "quaternion", q );
						bag.GetVec3( "scale",      s );
						double M[16];
						ComposeTRS_QuaternionGltf( pos, q, s, M );
						const Matrix4 transform = BuildMatrix4FromColumnMajor( M );
						if( Implementation::Transformable* concrete =
							dynamic_cast<Implementation::Transformable*>( obj ) ) {
							concrete->SetFinalTransformMatrix( transform );
						} else {
							obj->ClearAllTransforms();
							obj->PushTopTransStack( transform );
						}
						any = true;
					} else {
						// Per-field path.  Each Set* is independent;
						// missing fields leave the corresponding
						// component untouched (allows partial overrides
						// like `override_object { name X  position 1 2 3 }`
						// to update X's translation without disturbing
						// its orientation/scale).
						//
						// Matrix-authored objects retain one authoritative
						// affine matrix in Transformable, so these absolute
						// setters replace the requested field without
						// composing hidden component state beneath it.
						double v[3];
						if( bag.GetVec3( "position", v ) ) {
							obj->SetPosition( Point3( v[0], v[1], v[2] ) );
							any = true;
						}
						if( bag.GetVec3( "orientation", v ) ) {
							// orientation is DEGREES per scene convention
							// (matches standard_object).
							obj->SetOrientation( Vector3(
								v[0]*DEG_TO_RAD, v[1]*DEG_TO_RAD, v[2]*DEG_TO_RAD ) );
							any = true;
						}
						double scl[3];
						if( bag.GetVec3( "scale", scl ) ) {
							// R2 fix (pinned 2.7): ALWAYS use
							// SetStretch — `scale` Vec3 routes to
							// SetStretch in standard_object too
							// (its parser in this file → AddObject
							// → IObjectPriv::SetStretch).
							obj->SetStretch( Vector3( scl[0], scl[1], scl[2] ) );
							any = true;
						}
					}

					if( !any ) {
						GlobalLog()->PrintEx( eLog_Warning,
							"override_object `%s`: no override parameters present "
							"(empty block — likely a bug in the writer).",
							name.c_str() );
						return true;	// not a fatal error; just a no-op.
					}

					obj->FinalizeTransformations();
					objs->InvalidateSpatialStructure();
					// Record the override so the CST incremental apply can refuse when any
					// is present (its String target-ref is invisible to the static graph,
					// so the closure of editing the target would miss it -- review P1.3).
					pJob.NoteObjectOverride();
					return true;
				}

				const ChunkDescriptor& Describe() const override
				{
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword     = "override_object";
						cd.category    = ChunkCategory::Object;
						cd.description = "Apply transform overrides to an already-declared "
							"object.  Legacy chunk emitted by the pre-CST round-trip save "
							"(deleted in Model-B P5 Slice 6d) for objects that had no direct "
							"source-line representation; nothing emits it today, but it "
							"remains parseable for older scene files that contain it.  Order "
							"in the scene file matters — this chunk must appear AFTER the "
							"chunk that created the target.";
						auto P = [&cd]() -> ParameterDescriptor& {
							cd.parameters.emplace_back();
							return cd.parameters.back();
						};
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;     p.required = true;
						  p.description = "Name of the existing object to override."; }
						{ auto& p = P(); p.name = "position";    p.kind = ValueKind::DoubleVec3; p.required = false;
						  p.description = "Position, in the TARGET's own local frame -- world-space unless the "
						                  "target's own chunk declares a `parent`.  This chunk has no `parent` "
						                  "of its own; it edits the target's transform.  Matches "
						                  "standard_object semantics."; }
						{ auto& p = P(); p.name = "orientation"; p.kind = ValueKind::DoubleVec3; p.required = false;
						  p.description = "Euler orientation in DEGREES; matches standard_object semantics."; }
						{ auto& p = P(); p.name = "quaternion";  p.kind = ValueKind::DoubleVec4; p.required = false;
						  p.description = "Rotation quaternion (xyzw, glTF); matches standard_object semantics."; }
						{ auto& p = P(); p.name = "matrix";      p.kind = ValueKind::DoubleMat4; p.required = false;
						  p.description = "Full 4x4 transform, column-major, in the TARGET's own local frame "
						                  "(world-space unless the target's chunk declares a `parent`); overrides "
						                  "position/orientation/quaternion/scale.  The pre-CST "
						                  "byte-splice save (deleted in Slice 6d) emitted this for "
						                  "objects whose transform was not decomposable into "
						                  "pos+orient+scale (e.g. after ScaleObjectFromAnchor); "
						                  "nothing emits override_object today, but older scene "
						                  "files that contain it remain parseable."; }
						{ auto& p = P(); p.name = "scale";       p.kind = ValueKind::DoubleVec3; p.required = false;
						  p.description = "Per-axis scale; matches standard_object semantics."; }
						return cd;
					}();
					return d;
				}
			};

			struct CSGObjectAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name     = bag.GetString( "name",     "noname" );
					std::string obja     = bag.GetString( "obja",     "none" );
					std::string objb     = bag.GetString( "objb",     "none" );
					std::string material = bag.GetString( "material", "none" );
					std::string modifier = bag.GetString( "modifier", "none" );
					std::string shader   = bag.GetString( "shader",   "none" );

					double pos[3]    = {0,0,0};
					double orient[3] = {0,0,0};
					bag.GetVec3( "position", pos );
					if( bag.GetVec3( "orientation", orient ) ) {
						orient[0] *= DEG_TO_RAD;
						orient[1] *= DEG_TO_RAD;
						orient[2] *= DEG_TO_RAD;
					}

					RadianceMapConfig radianceMapConfig;
					if( bag.Has( "radiance_map" ) )    radianceMapConfig.name  = bag.GetString( "radiance_map" ).c_str();
					if( bag.Has( "radiance_scale" ) )  radianceMapConfig.scale = bag.GetDouble( "radiance_scale" );
					if( bag.GetVec3( "radiance_orient", radianceMapConfig.orientation ) ) {
						radianceMapConfig.orientation[0] *= DEG_TO_RAD;
						radianceMapConfig.orientation[1] *= DEG_TO_RAD;
						radianceMapConfig.orientation[2] *= DEG_TO_RAD;
					}

					char op = 0;
					if( bag.Has( "operation" ) ) {
						std::string opStr = bag.GetString( "operation" );
						if( opStr == "union" ) {
							op = 0;
						} else if( opStr == "intersection" ) {
							op = 1;
						} else if( opStr == "subtraction" ) {
							op = 2;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "csg_object:: unknown operation `%s`", opStr.c_str() );
							return false;
						}
					}

					bool bCastsShadows    = bag.GetBool( "casts_shadows",    true );
					bool bReceivesShadows = bag.GetBool( "receives_shadows", true );

					// Acknowledgment idiom mirroring `allow_non_sampling_emitter` on this same
					// chunk (below) -- except this flag is NOT inert: it is threaded through to
					// Job::AddCSGObject, which reads it to suppress its operand-rebase advisory
					// (see that function's comment) for a csg_object that deliberately re-bases
					// already-transformed operands.
					bool bAllowTransformedOperands = bag.GetBool( "allow_transformed_operands", false );

					if( !pJob.AddCSGObject( name.c_str(), obja.c_str(), objb.c_str(), op, material=="none"?0:material.c_str(), modifier=="none"?0:modifier.c_str(), shader=="none"?0:shader.c_str(), radianceMapConfig, pos, orient, bCastsShadows, bReceivesShadows, bAllowTransformedOperands ) ) {
						return false;
					}

					// 87: a csg_object is an ordinary scene-graph node -- it is
					// world-visible and its final matrix really is its world
					// transform, so it can be parented (its OPERANDS cannot; see
					// ObjectManager::SetObjectParent).  Same unconditional call as
					// standard_object, so deleting the line detaches on an
					// incremental re-apply.
					const std::string csgParent = bag.GetString( "parent", "" );
					const bool csgWantsParent = !csgParent.empty() && csgParent != "none";
					if( !pJob.SetObjectParent( name.c_str(), csgWantsParent ? csgParent.c_str() : 0 ) && csgWantsParent ) {
						if( RISE::g_cstFinalizeDiagSink ) {
							*RISE::g_cstFinalizeDiagSink = "csg_object `" + name + "`: `parent " + csgParent +
								"` was refused.  A `parent` must be a DECLARED-EARLIER object; must not be this "
								"object; must not already be one of its descendants; and must not be a CSG operand "
								"(parent that csg_object instead).  The log line immediately above names WHICH of "
								"those it was.";
						}
						return false;
					}
					return true;
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "csg_object"; cd.category = ChunkCategory::Object;
						cd.description = "Constructive solid geometry combining two objects by a boolean operation.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "parent";  p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Object}; p.description = "Object to parent this composite to (must be declared earlier); its transform is LOCAL, relative to that parent"; }
						{ auto& p = P(); p.name = "obja";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Object}; p.description = "First operand object"; }
						{ auto& p = P(); p.name = "objb";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Object}; p.description = "Second operand object"; }
						{ auto& p = P(); p.name = "operation";   p.kind = ValueKind::Enum;      p.enumValues = {"union","intersection","subtraction"}; p.description = "CSG operation"; }
						{ auto& p = P(); p.name = "material";    p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Material}; p.description = "Override material"; }
						{ auto& p = P(); p.name = "modifier";    p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Modifier}; p.description = "Override modifier"; }
						{ auto& p = P(); p.name = "shader";      p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Shader}; p.description = "Override shader"; }
						{ auto& p = P(); p.name = "position";        p.kind = ValueKind::DoubleVec3;p.description = "Position, LOCAL to `parent` (world-space when unparented)"; p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "orientation";     p.kind = ValueKind::DoubleVec3;p.description = "Euler orientation (degrees)"; p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "casts_shadows";   p.kind = ValueKind::Bool;      p.description = "Casts shadows"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "receives_shadows";p.kind = ValueKind::Bool;      p.description = "Receives shadows"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "radiance_map";    p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Per-object radiance map"; }
						{ auto& p = P(); p.name = "radiance_scale";  p.kind = ValueKind::Double;    p.description = "Radiance-map scale"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "radiance_orient"; p.kind = ValueKind::DoubleVec3;p.description = "Radiance-map orientation (degrees)"; p.defaultValueHint = "0 0 0"; }
						// Post-arc enforcement E1 (docs/agentic-redesign/75-expressive-surface-arc.md
						// sec 7, the LUMINAIRE_NULL_GEOMETRY entry): a csg_object has no directly-
						// owned geometry, so an emissive material bound to it is never selected by
						// NEE light-sampling -- it only glows on direct view.  This flag has a
						// single purpose: let an author ACKNOWLEDGE that gap is intentional.  It is
						// parsed and validated like any other declared parameter but is otherwise
						// semantically inert here -- it does not change CSG construction, rendering,
						// or light-list membership; AgentSession reads it (via the CST, not this
						// Finalize) to silence the LUMINAIRE_NULL_GEOMETRY Warning and the agent-
						// edit creation gate for an object that carries it TRUE.
						{ auto& p = P(); p.name = "allow_non_sampling_emitter"; p.kind = ValueKind::Bool; p.description = "Acknowledges that this object's emissive material intentionally will not light-sample (a csg_object has no directly-owned geometry, so it is never selected for next-event estimation) -- it only glows on direct view."; p.defaultValueHint = "FALSE"; }
						// 87 review: an operand's `position`/`orientation` is interpreted in THIS
						// csg_object's LOCAL frame, so when the csg_object ALSO carries its own
						// `position`/`orientation`, that transform RE-BASES every already-positioned
						// operand -- a valid, supported construction (an already-authored
						// sub-assembly rebased as a unit; the "two half-spheres offset locally,
						// assembly positioned as a unit" lens idiom, e.g.
						// scenes/FeatureBased/Combined/crystal_lens.RISEscene), but one
						// Job::AddCSGObject otherwise flags with a parse-time advisory in case it
						// was accidental (see docs/SCENE_CONVENTIONS.md sec 5.5).  This flag has a
						// single purpose: let an author ACKNOWLEDGE the rebase is intentional.
						// UNLIKE `allow_non_sampling_emitter` above, this one is NOT semantically
						// inert here -- it is threaded straight through to Job::AddCSGObject, which
						// reads it to suppress that advisory.
						{ auto& p = P(); p.name = "allow_transformed_operands"; p.kind = ValueKind::Bool; p.description = "Acknowledges that this csg_object deliberately re-bases already-transformed operands -- its own transform composes with each operand's transform, which is interpreted in this csg_object's local frame; suppresses the operand-rebase warning."; p.defaultValueHint = "FALSE"; }
						return cd;
					}();
					return d;
				}
			};

			//////////////////////////////////////////
			// Photon Mapping
			//////////////////////////////////////////


			//////////////////////////////////////////
			// Lights
			//////////////////////////////////////////

			// COLOUR CONVENTION (2026-09-02) shared by all four zero-area lights.
			//
			// A light's `color` is a LINEAR Rec.709 triple by default -- the same
			// reading `uniformcolor_painter` and every emissive material's
			// `exitance` already had.  Before this date the four Job::Add*Light
			// entry points silently gamma-DECODED the triple as sRGB, so an
			// authored `color 1.0 0.2 0.2` lit the scene with (1.0, 0.033, 0.033)
			// while the SAME triple on a painter meant what it said.  Authors who
			// picked their colour in an sRGB colour picker now say so explicitly
			// with `colorspace sRGB`; `tools/migrate_scenes_light_colorspace.py`
			// adds exactly that line to pre-existing scenes so their look is
			// preserved bit-for-bit.
			//
			// The accepted value set is `uniformcolor_painter`'s (Job.cpp routes
			// both through the one ColorSpaceNameToRISEPel switch), minus the
			// internal "RISERGB" passthrough which is not an authoring colour
			// space.  Job still ACCEPTS "RISERGB" if some caller passes it; it is
			// simply not offered in the enum a scene author / highlighter sees.
			const std::vector<std::string> kLightColorSpaceValues =
				{ "sRGB", "Rec709RGB_Linear", "ROMMRGB_Linear", "ProPhotoRGB" };
			const char* const kLightDefaultColorSpace = "Rec709RGB_Linear";
			const char* const kLightColorDescription =
				"R G B emission colour -- LINEAR Rec.709 unless `colorspace` says otherwise";
			const char* const kLightColorSpaceDescription =
				"Interpretation of `color` (linear default; same value set as uniformcolor_painter's `colorspace`).  "
				"Use `sRGB` for a value read off a colour picker";

			// AmbientLight — descriptor-driven (reference pattern for migrations).
			// State holds the accumulator values; apply functions below populate
			// it; kAmbientLightDescriptor lists every valid parameter with its
			// metadata and apply binding; ParseChunk creates a state, dispatches
			// via the descriptor, then hands the state to pJob.AddAmbientLight.
			// Adding or removing a parameter is a single edit in the descriptor.
			// AmbientLight — descriptor-driven, Finalize-only
			struct AmbientLightAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					double power = bag.GetDouble( "power", 1.0 );
					double color[3] = {0,0,0};
					bag.GetVec3( "color", color );
					const std::string cspace = bag.GetString( "colorspace", kLightDefaultColorSpace );
					return pJob.AddAmbientLight( name.c_str(), power, color, cspace.c_str() );
				}

				const ChunkDescriptor& Describe() const override
				{
					static const ChunkDescriptor d = [](){
						ChunkDescriptor cd;
						cd.keyword     = "ambient_light";
						cd.category    = ChunkCategory::Light;
						cd.description = "Uniform ambient illumination (no spatial variation).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";  p.kind = ValueKind::String;     p.description = "Unique name for this light";  p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "power"; p.kind = ValueKind::Double;     p.description = "Power scale (multiplies color)"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "color"; p.kind = ValueKind::DoubleVec3; p.description = kLightColorDescription; p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "colorspace"; p.kind = ValueKind::Enum; p.enumValues = kLightColorSpaceValues; p.description = kLightColorSpaceDescription; p.defaultValueHint = kLightDefaultColorSpace; }
						return cd;
					}();
					return d;
				}
			};

			// OmniLight — descriptor-driven, Finalize-only
			struct OmniLightAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					double power = bag.GetDouble( "power", 1.0 );
					double position[3] = {0,0,0}; bag.GetVec3( "position", position );
					double color[3]    = {0,0,0}; bag.GetVec3( "color",    color );
					bool shootphotons  = bag.GetBool( "shootphotons", true );
					const std::string cspace = bag.GetString( "colorspace", kLightDefaultColorSpace );
					return pJob.AddPointOmniLight( name.c_str(), power, color, cspace.c_str(), position, shootphotons );
				}

				const ChunkDescriptor& Describe() const override
				{
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "omni_light"; cd.category = ChunkCategory::Light;
						cd.description = "Point light radiating uniformly in all directions.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";         p.kind = ValueKind::String;     p.description = "Unique name for this light";        p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "power";        p.kind = ValueKind::Double;     p.description = "Power scale (multiplies color)";   p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "position";     p.kind = ValueKind::DoubleVec3; p.description = "World-space position";             p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "color";        p.kind = ValueKind::DoubleVec3; p.description = kLightColorDescription;             p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "colorspace";   p.kind = ValueKind::Enum;       p.enumValues = kLightColorSpaceValues; p.description = kLightColorSpaceDescription; p.defaultValueHint = kLightDefaultColorSpace; }
						{ auto& p = P(); p.name = "shootphotons"; p.kind = ValueKind::Bool;       p.description = "Whether this light emits photons"; p.defaultValueHint = "TRUE"; }
						return cd;
					}();
					return d;
				}
			};

			// SpotLight — descriptor-driven, Finalize-only
			struct SpotLightAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					double power = bag.GetDouble( "power", 1.0 );
					// inner/outer specified in degrees; converted iff present.
					double inner = PI_OV_FOUR;
					double outer = PI_OV_TWO;
					if( bag.Has("inner") ) inner = bag.GetDouble("inner", 45.0) * DEG_TO_RAD;
					if( bag.Has("outer") ) outer = bag.GetDouble("outer", 90.0) * DEG_TO_RAD;
					double position[3] = {0,0,0};  bag.GetVec3( "position", position );
					double target[3]   = {0,0,0};  bag.GetVec3( "target",   target );
					double color[3]    = {0,0,0};  bag.GetVec3( "color",    color );
					bool shootphotons  = bag.GetBool( "shootphotons", true );
					const std::string cspace = bag.GetString( "colorspace", kLightDefaultColorSpace );
					return pJob.AddPointSpotLight( name.c_str(), power, color, cspace.c_str(), target, inner, outer, position, shootphotons );
				}

				const ChunkDescriptor& Describe() const override
				{
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "spot_light"; cd.category = ChunkCategory::Light;
						cd.description = "Cone spot light with inner and outer falloff angles.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";         p.kind = ValueKind::String;     p.description = "Unique name for this light";       p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "power";        p.kind = ValueKind::Double;     p.description = "Power scale (multiplies color)";  p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "inner";        p.kind = ValueKind::Double;     p.description = "Inner cone half-angle (degrees)"; p.defaultValueHint = "45"; }
						{ auto& p = P(); p.name = "outer";        p.kind = ValueKind::Double;     p.description = "Outer cone half-angle (degrees)"; p.defaultValueHint = "90"; }
						{ auto& p = P(); p.name = "position";     p.kind = ValueKind::DoubleVec3; p.description = "World-space position";            p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "target";       p.kind = ValueKind::DoubleVec3; p.description = "World-space target point";        p.defaultValueHint = "0 0 -1"; }
						{ auto& p = P(); p.name = "color";        p.kind = ValueKind::DoubleVec3; p.description = kLightColorDescription;            p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "colorspace";   p.kind = ValueKind::Enum;       p.enumValues = kLightColorSpaceValues; p.description = kLightColorSpaceDescription; p.defaultValueHint = kLightDefaultColorSpace; }
						{ auto& p = P(); p.name = "shootphotons"; p.kind = ValueKind::Bool;       p.description = "Whether this light emits photons"; p.defaultValueHint = "TRUE"; }
						return cd;
					}();
					return d;
				}
			};

			// HosekWilkieSkylight — Landing 3.D analytic sun-and-sky.
			// Creates an IRadianceMap (HW model wrapper) and registers
			// it as the scene's global radiance map.  Optionally also
			// creates a matched directional_light named `__hw_sun__`
			// with the same solar elevation/azimuth — toggleable so a
			// scene can use HW for the env hemisphere only.
			struct HosekWilkieSkylightAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					const double  solarElevation = bag.GetDouble( "solar_elevation",     45.0 );
					const double  solarAzimuth   = bag.GetDouble( "solar_azimuth",        0.0 );
					const double  turbidity      = bag.GetDouble( "turbidity",            3.0 );
					double        groundAlbedo[3] = { 0.3, 0.3, 0.3 };
					if( bag.Has( "ground_albedo" ) ) {
						bag.GetVec3( "ground_albedo", groundAlbedo );
					}
					const double  skyScale       = bag.GetDouble( "sky_intensity_scale", 1.0 );
					const double  sunPower       = bag.GetDouble( "sun_intensity_scale", 3.14 );
					const bool    createSun      = bag.GetBool(   "create_sun",          true );

					return pJob.AddHosekWilkieSkylight(
						solarElevation, solarAzimuth, turbidity,
						groundAlbedo, skyScale, sunPower, createSun );
				}

				const ChunkDescriptor& Describe() const override
				{
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "hosek_wilkie_skylight";
						cd.category = ChunkCategory::Light;
						cd.description = "Analytic spectral sun-and-sky (Hosek-Wilkie 2012; v1 uses Preetham 1999 internally).  Creates a global radiance map and an optional matched directional_light atomically — they share the same solar position so they can't drift apart.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "solar_elevation";    p.kind = ValueKind::Double;     p.description = "Sun angle above horizon, degrees (0 = horizon, 90 = zenith)";          p.defaultValueHint = "45.0"; }
						{ auto& p = P(); p.name = "solar_azimuth";      p.kind = ValueKind::Double;     p.description = "Sun compass bearing, degrees (0 = +Z, 90 = +X)";                       p.defaultValueHint = "0.0"; }
						{ auto& p = P(); p.name = "turbidity";          p.kind = ValueKind::Double;     p.description = "Atmospheric turbidity ∈ [1, 10] (1 = arctic clear, 3 = typical, 10 = polluted)"; p.defaultValueHint = "3.0"; }
						{ auto& p = P(); p.name = "ground_albedo";      p.kind = ValueKind::DoubleVec3; p.description = "Per-channel ground albedo (v1: stored but not coupled — Preetham fallback; emits one-time warning if non-default)"; p.defaultValueHint = "0.3 0.3 0.3"; }
						{ auto& p = P(); p.name = "sky_intensity_scale"; p.kind = ValueKind::Double;    p.description = "Multiplier on radiance-map output (does not affect the matched sun light)"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "sun_intensity_scale"; p.kind = ValueKind::Double;    p.description = "Power for the matched directional_light (RISE convention: π for unit-flux key)"; p.defaultValueHint = "3.14"; }
						{ auto& p = P(); p.name = "create_sun";         p.kind = ValueKind::Bool;       p.description = "If true (default), also creates a directional_light `__hw_sun__` matched to the solar position";  p.defaultValueHint = "true"; }
						return cd;
					}();
					return d;
				}
			};

			// DirectionalLight — descriptor-driven, Finalize-only
			struct DirectionalLightAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					double power = bag.GetDouble( "power", 1.0 );
					double dir[3]   = {0,0,0}; bag.GetVec3( "direction", dir );
					double color[3] = {0,0,0}; bag.GetVec3( "color",     color );
					const std::string cspace = bag.GetString( "colorspace", kLightDefaultColorSpace );
					return pJob.AddDirectionalLight( name.c_str(), power, color, cspace.c_str(), dir );
				}

				const ChunkDescriptor& Describe() const override
				{
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "directional_light"; cd.category = ChunkCategory::Light;
						cd.description = "Parallel rays from a fixed direction (e.g. sunlight).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";      p.kind = ValueKind::String;     p.description = "Unique name for this light";      p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "power";     p.kind = ValueKind::Double;     p.description = "Power scale (multiplies color)"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "direction"; p.kind = ValueKind::DoubleVec3; p.description = "Direction vector";                p.defaultValueHint = "0 -1 0"; }
						{ auto& p = P(); p.name = "color";     p.kind = ValueKind::DoubleVec3; p.description = kLightColorDescription;           p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "colorspace"; p.kind = ValueKind::Enum;      p.enumValues = kLightColorSpaceValues; p.description = kLightColorSpaceDescription; p.defaultValueHint = kLightDefaultColorSpace; }
						return cd;
					}();
					return d;
				}
			};

			//======================================================================
			// RectLight -- a first-class ONE-CHUNK PHYSICAL light (2026-08-12).
			//
			// WHY IT EXISTS.  docs/agentic-redesign/83-staged-construction-plan.md
			// sec 9: three separate mechanism families (palette order + sole worked
			// example; one completion per light; source-first enumeration) each
			// failed to get an agent to author an area light, because the category
			// of the task summons the category of the chunk -- asked for LIGHTING,
			// a model writes chunks from the lighting category, and until now every
			// chunk in that category was a zero-area idealization.  The fix is a
			// lighting-category chunk that IS an area light.
			//
			// WHAT IT IS.  Pure PARSE-TIME SUGAR over the canonical four-chunk area
			// light (docs/SCENE_CONVENTIONS.md sec 3.5): this Finalize makes exactly
			// the four IJob calls a hand-authored chain makes, in the same order.
			// The renderer core learns NO new concept -- there is no RectLight
			// entity, no new ILight, nothing downstream to teach.  The DOCUMENT
			// keeps the compact `rect_light` text (the CST stores the chunk as
			// authored, so save round-trips it verbatim) and every derive expands it
			// identically.  Because scene load and the agent insert path both run
			// through this same registry, both get the chunk with no second edit.
			//
			// EXITANCE ONLY.  There is deliberately no `power` parameter and no
			// alias for one (project owner, 2026-08-12).  `exitance` is brightness
			// PER UNIT AREA -- it is what `scale` means on the underlying
			// lambertian_luminaire_material -- so the same number on a panel twice
			// the size delivers twice the light.  A `power` knob would invite the
			// zero-area lights' `color * power` reading, which is a different
			// quantity.
			//
			// SIDEDNESS.  The panel emits toward `facing` and nowhere else.  Two
			// facts make that work, and BOTH are load-bearing:
			//   * LambertianEmitter::emittedRadiance returns black when
			//     Dot(out, N) <= 0 (LambertianEmitter.cpp) -- emission is
			//     one-sided about the surface normal.
			//   * ClippedPlaneGeometry derives that normal from the corner winding:
			//     N = normalize(Cross(dpdu, dpdv)) with dpdu = ptb - pta and
			//     dpdv = ptd - pta for a planar parallelogram (the (u,v) layout is
			//     pta->(0,0), ptb->(1,0), ptc->(1,1), ptd->(0,1); see
			//     ClippedPlaneGeometry.cpp's header block and IntersectRay).
			// So the corner order below is chosen to make Cross(ptb-pta, ptd-pta)
			// point along `facing`, and `doublesided` is FALSE: with it TRUE a
			// back-face hit flips the normal toward the ray, Dot(out, N) becomes
			// positive again, and the panel would emit from BOTH faces.
			//
			// DERIVED NAMES.  The chunk's `name` names the OBJECT; the three
			// entities it also creates are `<name>__pnt` (painter), `<name>__mat`
			// (luminaire material) and `<name>__geo` (quad).  A collision on any of
			// the four fails the derive with the manager's ordinary duplicate-name
			// error -- never silently.
			//======================================================================
			const char* const kRectLightPainterSuffix  = "__pnt";
			const char* const kRectLightMaterialSuffix = "__mat";
			const char* const kRectLightGeometrySuffix = "__geo";

			struct RectLightAsciiChunkParser : public IAsciiChunkParser
			{
				//! One place both the log line and the CST derive diagnostic are
				//! written from, so an author sees the same sentence wherever the
				//! failure surfaces.
				static bool Reject( const std::string& why )
				{
					if( RISE::g_cstFinalizeDiagSink ) *RISE::g_cstFinalizeDiagSink = why;
					GlobalLog()->PrintEx( eLog_Error, "rect_light:: %s", why.c_str() );
					return false;
				}

				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					// ---- REQUIRED PARAMETERS.  `required` in a ParameterDescriptor
					// is metadata for the schema/editor surfaces; the dispatcher does
					// not enforce it, so every required parameter is checked here (the
					// same shape thinlens_camera's validation block uses).
					const std::string name = bag.GetString( "name", std::string() );
					if( name.empty() ) {
						return Reject( "`name` is required -- it names the object this light becomes" );
					}

					double center[3] = {0,0,0};
					if( !bag.GetVec3( "center", center ) ) {
						return Reject( "`center` is required -- the world-space centre of the panel" );
					}

					double facing[3] = {0,0,0};
					if( !bag.GetVec3( "facing", facing ) ) {
						return Reject( "`facing` is required -- the direction the panel emits toward" );
					}

					if( !bag.Has( "size" ) ) {
						return Reject( "`size` is required -- two numbers, width and height" );
					}
					if( !HasExactNumericArity( bag, "size", 2 ) ) {
						return Reject( "`size` takes exactly two numbers (width height); got `" +
						               bag.GetString( "size" ) + "`" );
					}
					double width = 0.0, height = 0.0;
					sscanf( bag.GetString( "size" ).c_str(), "%lf %lf", &width, &height );
					if( width <= 0.0 || height <= 0.0 ) {
						char buf[128];
						std::snprintf( buf, sizeof(buf),
							"`size` must be two POSITIVE numbers (width height); got %g %g", width, height );
						return Reject( buf );
					}

					if( !bag.Has( "exitance" ) ) {
						return Reject( "`exitance` is required -- emitted radiance per unit area, "
						               "so a panel twice the size at the same exitance delivers twice the light" );
					}
					const double exitance = bag.GetDouble( "exitance", 0.0 );
					if( !( exitance > 0.0 ) ) {
						char buf[128];
						std::snprintf( buf, sizeof(buf),
							"`exitance` must be greater than zero; got %g (a light that emits nothing is not a light)",
							exitance );
						return Reject( buf );
					}

					double color[3] = { 1.0, 1.0, 1.0 };
					bag.GetVec3( "color", color );

					// ---- THE PANEL BASIS.  `facing` becomes the quad's geometric
					// normal exactly; U and V are its in-plane width / height axes.
					// Cross(U, V) == F by construction: V = Cross(F, U) with U unit
					// and perpendicular to F gives Cross(U, Cross(F, U)) =
					// F(U.U) - U(U.F) = F.
					const Vector3 rawFacing( facing[0], facing[1], facing[2] );
					const Scalar  facingLen = Vector3Ops::Magnitude( rawFacing );
					if( !( facingLen > 1e-12 ) ) {
						return Reject( "`facing` is degenerate (zero-length) -- it must be the "
						               "direction the panel emits toward, e.g. `facing 0 -1 0` for a "
						               "ceiling panel pointing at the floor" );
					}
					const Vector3 F = rawFacing * ( 1.0 / facingLen );

					// The helper axis picks the in-plane frame.  World up is the
					// natural choice -- it makes `height` the vertical axis for any
					// horizontally-facing panel -- but it degenerates when `facing`
					// IS (anti)parallel to up, so a floor / ceiling panel falls back
					// to world +Z and gets width along X, height along Z.
					const Vector3 kWorldUp( 0.0, 1.0, 0.0 );
					const Vector3 kWorldFwd( 0.0, 0.0, 1.0 );
					const Vector3 helper =
						( fabs( Vector3Ops::Dot( F, kWorldUp ) ) < 0.999999 ) ? kWorldUp : kWorldFwd;
					const Vector3 U = Vector3Ops::Normalize( Vector3Ops::Cross( helper, F ) );
					const Vector3 V = Vector3Ops::Cross( F, U );

					const Scalar hw = 0.5 * width;
					const Scalar hh = 0.5 * height;
					const Vector3 du = U * hw;
					const Vector3 dv = V * hh;

					// Winding pta -> ptb -> ptc -> ptd walks (-U,-V) (+U,-V) (+U,+V)
					// (-U,+V), so ptb - pta = width*U and ptd - pta = height*V, and
					// Cross(ptb - pta, ptd - pta) = width*height*F -- the geometric
					// normal IS `facing`.
					double pta[3], ptb[3], ptc[3], ptd[3];
					for( int k = 0; k < 3; ++k ) {
						const double c  = center[k];
						const double eu = ( k == 0 ? du.x : ( k == 1 ? du.y : du.z ) );
						const double ev = ( k == 0 ? dv.x : ( k == 1 ? dv.y : dv.z ) );
						pta[k] = c - eu - ev;
						ptb[k] = c + eu - ev;
						ptc[k] = c + eu + ev;
						ptd[k] = c - eu + ev;
					}

					// ---- THE FOUR CALLS.  Same order, same arguments as the
					// hand-authored chain in docs/SCENE_CONVENTIONS.md sec 3.5.
					const std::string pntName = name + kRectLightPainterSuffix;
					const std::string matName = name + kRectLightMaterialSuffix;
					const std::string geoName = name + kRectLightGeometrySuffix;

					// `Rec709RGB_Linear` matches uniformcolor_painter's own default:
					// a hand-typed light tint is a physical number, not a
					// display-referred one.
					if( !pJob.AddUniformColorPainter( pntName.c_str(), color, "Rec709RGB_Linear" ) ) {
						return Reject( "could not create the emitted-colour painter `" + pntName +
						               "` -- the usual cause is that a chunk of that name already exists" );
					}
					// Mirror uniformcolor_painter's cache write so a downstream chunk
					// that reads a painter's colour sees this one too.
					{
						PainterColor pc = { { color[0], color[1], color[2] } };
						s_painterColors[pntName] = pc;
					}

					if( !pJob.AddLambertianLuminaireMaterial( matName.c_str(), pntName.c_str(), "none", exitance ) ) {
						return Reject( "could not create the luminaire material `" + matName +
						               "` -- the usual cause is that a chunk of that name already exists" );
					}

					if( !pJob.AddClippedPlaneGeometry( geoName.c_str(), pta, ptb, ptc, ptd, false ) ) {
						return Reject( "could not create the panel geometry `" + geoName +
						               "` -- the usual cause is that a chunk of that name already exists" );
					}

					RadianceMapConfig radianceMapConfig;
					double pos[3]    = { 0.0, 0.0, 0.0 };
					double orient[3] = { 0.0, 0.0, 0.0 };
					double scl[3]    = { 1.0, 1.0, 1.0 };
					if( !pJob.AddObject( name.c_str(), geoName.c_str(), matName.c_str(), 0, 0,
					                     radianceMapConfig, pos, orient, scl, true, true ) ) {
						return Reject( "could not create the object `" + name +
						               "` -- the usual cause is that a chunk of that name already exists" );
					}

					// 87: the object this chunk synthesizes is an ordinary
					// scene-graph node, so a lamp can be carried by an assembly.
					// Unconditional, so deleting the line detaches.
					{
						const std::string lightParent = bag.GetString( "parent", "" );
						const bool wants = !lightParent.empty() && lightParent != "none";
						if( !pJob.SetObjectParent( name.c_str(), wants ? lightParent.c_str() : 0 ) && wants ) {
							return Reject( "`" + name + "`: `parent " + lightParent + "` was refused.  A `parent` "
							               "must be a DECLARED-EARLIER object; must not be this object; must not "
							               "already be one of its descendants; and must not be a CSG operand (parent "
							               "the csg_object instead).  The log line immediately above names WHICH of "
							               "those it was." );
						}
					}

					return true;
				}

				const ChunkDescriptor& Describe() const override
				{
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "rect_light"; cd.category = ChunkCategory::Light;
						cd.description =
							"A rectangular AREA light: a real emitting surface in the scene, and the "
							"physically based way to light one.  It is expanded at parse time into the "
							"four chunks an area light is otherwise written as -- a uniformcolor_painter "
							"holding `color`, a lambertian_luminaire_material whose exitance is that "
							"painter and whose scale is `exitance`, a clippedplane_geometry whose four "
							"corners come from center/size/facing, and a standard_object binding them.  "
							"Those three helper entities are named `<name>__pnt`, `<name>__mat` and "
							"`<name>__geo`; the standard_object takes `<name>` itself, so `<name>` is "
							"what render-time tools (solo, object map, isolate) report.  A collision on "
							"any of the four names fails the load with the ordinary duplicate-name "
							"error.  The panel emits toward `facing` ONLY -- the back face is not hit at "
							"all.  Being an ordinary object it is rendered like one: the camera sees its "
							"surface wherever it is, so it is both a light and a thing the picture shows.  "
							"It has real area, so it casts soft shadows and falls off with distance.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";     p.kind = ValueKind::String;     p.required = true;
						  p.description = "Unique name.  Names the OBJECT; the painter, material and geometry this chunk also creates are `<name>__pnt`, `<name>__mat` and `<name>__geo`"; }
						{ auto& p = P(); p.name = "parent"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Object}; p.description = "Object to parent the emitted object to (must be declared earlier); its transform is then LOCAL to that parent"; }
						{ auto& p = P(); p.name = "center";   p.kind = ValueKind::DoubleVec3; p.required = true;
						  p.description = "Centre of the panel, LOCAL to `parent` (world-space when unparented)"; }
						{ auto& p = P(); p.name = "size";     p.kind = ValueKind::Double;     p.required = true;
						  p.tupleKinds = {ValueKind::Double, ValueKind::Double};
						  p.description = "Two positive numbers: width and height, in scene units"; p.unitLabel = "scene units"; }
						{ auto& p = P(); p.name = "facing";   p.kind = ValueKind::DoubleVec3; p.required = true;
						  p.description = "The direction the panel EMITS toward (a ceiling panel lighting the floor is `0 -1 0`).  It becomes the quad's normal exactly; the back face does not emit.  Need not be unit length, but must not be zero"; }
						{ auto& p = P(); p.name = "exitance"; p.kind = ValueKind::Double;     p.required = true;
						  p.description = "Emitted radiance PER UNIT AREA, so the same number on a panel twice the size delivers twice the light.  Must be greater than zero.  Existing scenes span four orders of magnitude: tens for a soft interior fill panel, thousands for a small window reading as daylight"; }
						{ auto& p = P(); p.name = "color";    p.kind = ValueKind::DoubleVec3;
						  p.description = "R G B tint of the emitted light, linear Rec.709"; p.defaultValueHint = "1 1 1"; }
						return cd;
					}();
					return d;
				}
			};

			//======================================================================
			// ShapeLight -- the CLOSED-SOLID sibling of rect_light (2026-08-12).
			//
			// WHY IT EXISTS.  `rect_light` (above) closed the one-chunk gap for a
			// PANEL, and the first live run using it recorded the remaining half
			// (docs/agentic-redesign/83-staged-construction-plan.md sec 11): the
			// glowing creatures of an imagined scene still became omni_lights,
			// because a jellyfish is not a rectangle and the only one-chunk light
			// with real area was.  `shape_light` is the same mechanism for the
			// shapes a glowing THING is: a bulb, an orb, a lamp body, a glowing
			// volume.  Owner-approved 2026-08-12, together with the zero-area
			// budget that makes reaching for an idealization cost something.
			//
			// WHAT IT IS.  Pure PARSE-TIME SUGAR, exactly as rect_light is: this
			// Finalize makes the same four IJob calls the hand-authored area-light
			// chain makes (docs/SCENE_CONVENTIONS.md sec 3.5) -- painter,
			// lambertian luminaire material, geometry, object.  The renderer core
			// learns NO new concept, the CST keeps the compact authored text, and
			// scene load and the agent insert path both get it through this one
			// registry.  rect_light is NOT folded into it: a rectangle needs a
			// `facing`, a closed solid emits every way at once, and the two
			// parameter sets do not want to be one.
			//
			// SIDEDNESS IS FREE HERE, AND THIS IS THE INTERESTING DIFFERENCE.
			// rect_light needed a `facing` parameter, a corner winding chosen to
			// make Cross(ptb-pta, ptd-pta) point along it, and `doublesided FALSE`,
			// all so that LambertianEmitter's one-sidedness (emittedRadiance
			// returns black when Dot(out, N) <= 0) pointed the right way.  Every
			// shape below is a CLOSED SOLID whose surface normal already points
			// OUTWARD at every point -- SphereGeometry / EllipsoidGeometry return
			// the outward radial normal, BoxGeometry the outward face normal,
			// CylinderGeometry the outward radial normal on the wall and the
			// outward axial normal on each cap.  Composed with the same one-sided
			// emitter that gives exactly "emits outward everywhere, inward
			// nowhere", which is what a bulb does, so there is NO facing parameter
			// and nothing for an author to get backwards.
			//
			// SIZE IS PER-SHAPE, and its arity is what selects the shape's own
			// constructor.  sphere = one radius; ellipsoid = three radii (the
			// semi-axes, like ellipsoid_geometry's `radii`); box = width height
			// depth (the X, Y and Z extents of box_geometry, which is centred on
			// the origin); cylinder = radius height, built on the +Y axis so it
			// stands upright, because `orientation` can turn it anywhere and the
			// upright reading is the one an author can hold in their head.
			//
			// DERIVED NAMES.  The same three suffixes rect_light uses, and
			// deliberately the same: the two chunks produce the same kind of
			// entity set, so an author who has learned one set of derived names
			// has learned both.  A collision on any of the four fails the derive
			// with the manager's ordinary duplicate-name error.
			//======================================================================
			const char* const kShapeLightPainterSuffix  = kRectLightPainterSuffix;
			const char* const kShapeLightMaterialSuffix = kRectLightMaterialSuffix;
			const char* const kShapeLightGeometrySuffix = kRectLightGeometrySuffix;

			//! The four shapes, their `size` arity and what those numbers mean.
			//! ONE table: the validator, the geometry switch and the descriptor's
			//! own text are all written from it, so a fifth shape cannot be added
			//! to one and forgotten in another.
			struct ShapeLightShape_
			{
				const char* keyword;
				int         sizeArity;
				const char* sizeMeaning;   //!< for the descriptor and the diagnostics
			};
			const ShapeLightShape_ kShapeLightShapes[] = {
				{ "sphere",    1, "one radius" },
				{ "ellipsoid", 3, "three radii -- the semi-axes, X Y Z" },
				{ "box",       3, "width height depth -- the X, Y and Z extents" },
				{ "cylinder",  2, "radius height -- the cylinder stands on the +Y axis" }
			};
			const std::size_t kShapeLightShapeCount =
				sizeof( kShapeLightShapes ) / sizeof( kShapeLightShapes[0] );

			//! "`sphere` (one radius), `ellipsoid` (...), ..." -- the valid set,
			//! spelled the same way in every diagnostic that names it.
			inline std::string ShapeLightValidShapes()
			{
				std::string s;
				for( std::size_t i = 0; i < kShapeLightShapeCount; ++i ) {
					if( i ) s += ( i + 1 == kShapeLightShapeCount ) ? " and " : ", ";
					s += std::string( "`" ) + kShapeLightShapes[i].keyword + "` (" +
					     kShapeLightShapes[i].sizeMeaning + ")";
				}
				return s;
			}

			struct ShapeLightAsciiChunkParser : public IAsciiChunkParser
			{
				//! One place both the log line and the CST derive diagnostic are
				//! written from -- rect_light's Reject, same reason.
				static bool Reject( const std::string& why )
				{
					if( RISE::g_cstFinalizeDiagSink ) *RISE::g_cstFinalizeDiagSink = why;
					GlobalLog()->PrintEx( eLog_Error, "shape_light:: %s", why.c_str() );
					return false;
				}

				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					// ---- REQUIRED PARAMETERS.  `required` in a ParameterDescriptor
					// is metadata for the schema / editor surfaces; the dispatcher
					// does not enforce it, so every required parameter is checked
					// here (rect_light's block, and thinlens_camera's before it).
					const std::string name = bag.GetString( "name", std::string() );
					if( name.empty() ) {
						return Reject( "`name` is required -- it names the object this light becomes" );
					}

					// ---- THE SHAPE, which also fixes what `size` means.  The
					// dispatcher does NOT validate an Enum parameter's value (it
					// only type-checks numeric kinds), so an unknown shape has to
					// fail here, naming the whole valid set rather than just
					// saying no.
					const std::string shape = bag.GetString( "shape", std::string() );
					if( shape.empty() ) {
						return Reject( "`shape` is required -- one of " + ShapeLightValidShapes() );
					}
					const ShapeLightShape_* chosen = 0;
					for( std::size_t i = 0; i < kShapeLightShapeCount; ++i )
						if( shape == kShapeLightShapes[i].keyword ) { chosen = &kShapeLightShapes[i]; break; }
					if( !chosen ) {
						return Reject( "`shape " + shape + "` is not a shape this chunk builds -- it is one of " +
						               ShapeLightValidShapes() );
					}

					double center[3] = {0,0,0};
					if( !bag.GetVec3( "center", center ) ) {
						return Reject( "`center` is required -- the world-space centre of the emitting solid" );
					}

					if( !bag.Has( "size" ) ) {
						return Reject( std::string( "`size` is required -- for `shape " ) + chosen->keyword +
						               "` it is " + chosen->sizeMeaning );
					}
					if( !HasExactNumericArity( bag, "size", chosen->sizeArity ) ) {
						char buf[64];
						std::snprintf( buf, sizeof(buf), "%d", chosen->sizeArity );
						return Reject( std::string( "`size` takes exactly " ) + buf + " number" +
						               ( chosen->sizeArity == 1 ? "" : "s" ) + " for `shape " + chosen->keyword +
						               "` -- " + chosen->sizeMeaning + "; got `" + bag.GetString( "size" ) + "`" );
					}
					double sz[3] = { 0.0, 0.0, 0.0 };
					{
						std::istringstream iss( bag.GetString( "size" ) );
						for( int k = 0; k < chosen->sizeArity; ++k ) iss >> sz[k];
					}
					for( int k = 0; k < chosen->sizeArity; ++k ) {
						if( !( sz[k] > 0.0 ) ) {
							char buf[192];
							std::snprintf( buf, sizeof(buf),
								"`size` must be %d POSITIVE number%s (%s); got `%s`",
								chosen->sizeArity, chosen->sizeArity == 1 ? "" : "s",
								chosen->sizeMeaning, bag.GetString( "size" ).c_str() );
							return Reject( buf );
						}
					}

					if( !bag.Has( "exitance" ) ) {
						return Reject( "`exitance` is required -- emitted radiance per unit area, "
						               "so the same number on a bigger solid delivers more light" );
					}
					const double exitance = bag.GetDouble( "exitance", 0.0 );
					if( !( exitance > 0.0 ) ) {
						char buf[128];
						std::snprintf( buf, sizeof(buf),
							"`exitance` must be greater than zero; got %g (a light that emits nothing is not a light)",
							exitance );
						return Reject( buf );
					}

					double color[3] = { 1.0, 1.0, 1.0 };
					bag.GetVec3( "color", color );

					// `orientation` is Euler DEGREES on the standard_object this
					// expands into, exactly as an author would write it there --
					// same parameter, same units, same axis order.
					double orient[3] = { 0.0, 0.0, 0.0 };
					if( bag.GetVec3( "orientation", orient ) ) {
						orient[0] *= DEG_TO_RAD;
						orient[1] *= DEG_TO_RAD;
						orient[2] *= DEG_TO_RAD;
					}

					// ---- THE FOUR CALLS.  Same order and same arguments as the
					// hand-authored chain, and as rect_light's Finalize.
					const std::string pntName = name + kShapeLightPainterSuffix;
					const std::string matName = name + kShapeLightMaterialSuffix;
					const std::string geoName = name + kShapeLightGeometrySuffix;

					if( !pJob.AddUniformColorPainter( pntName.c_str(), color, "Rec709RGB_Linear" ) ) {
						return Reject( "could not create the emitted-colour painter `" + pntName +
						               "` -- the usual cause is that a chunk of that name already exists" );
					}
					{
						PainterColor pc = { { color[0], color[1], color[2] } };
						s_painterColors[pntName] = pc;
					}

					if( !pJob.AddLambertianLuminaireMaterial( matName.c_str(), pntName.c_str(), "none", exitance ) ) {
						return Reject( "could not create the luminaire material `" + matName +
						               "` -- the usual cause is that a chunk of that name already exists" );
					}

					// Every one of these constructors builds its solid CENTRED ON
					// THE ORIGIN (BoxGeometry spans +-w/2, CylinderGeometry spans
					// +-height/2 along its axis), so `center` is carried entirely
					// by the object's position below -- there is no half-extent
					// offset to apply here and none to get wrong.
					bool geoOk = false;
					if( shape == "sphere" ) {
						geoOk = pJob.AddSphereGeometry( geoName.c_str(), sz[0] );
					} else if( shape == "ellipsoid" ) {
						const double radii[3] = { sz[0], sz[1], sz[2] };
						geoOk = pJob.AddEllipsoidGeometry( geoName.c_str(), radii );
					} else if( shape == "box" ) {
						geoOk = pJob.AddBoxGeometry( geoName.c_str(), sz[0], sz[1], sz[2] );
					} else {
						// CAPPED, and that is load-bearing: an open tube has no end
						// caps, so it would emit from the wall only and a camera
						// looking down the axis would see straight through a light.
						geoOk = pJob.AddCylinderGeometry( geoName.c_str(), 'y', sz[0], sz[1], true );
					}
					if( !geoOk ) {
						return Reject( "could not create the `" + shape + "` geometry `" + geoName +
						               "` -- the usual cause is that a chunk of that name already exists" );
					}

					RadianceMapConfig radianceMapConfig;
					double scl[3] = { 1.0, 1.0, 1.0 };
					if( !pJob.AddObject( name.c_str(), geoName.c_str(), matName.c_str(), 0, 0,
					                     radianceMapConfig, center, orient, scl, true, true ) ) {
						return Reject( "could not create the object `" + name +
						               "` -- the usual cause is that a chunk of that name already exists" );
					}

					// 87: the object this chunk synthesizes is an ordinary
					// scene-graph node, so a lamp can be carried by an assembly.
					// Unconditional, so deleting the line detaches.
					{
						const std::string lightParent = bag.GetString( "parent", "" );
						const bool wants = !lightParent.empty() && lightParent != "none";
						if( !pJob.SetObjectParent( name.c_str(), wants ? lightParent.c_str() : 0 ) && wants ) {
							return Reject( "`" + name + "`: `parent " + lightParent + "` was refused.  A `parent` "
							               "must be a DECLARED-EARLIER object; must not be this object; must not "
							               "already be one of its descendants; and must not be a CSG operand (parent "
							               "the csg_object instead).  The log line immediately above names WHICH of "
							               "those it was." );
						}
					}

					return true;
				}

				const ChunkDescriptor& Describe() const override
				{
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "shape_light"; cd.category = ChunkCategory::Light;
						cd.description =
							"A SOLID AREA light: a real emitting sphere, ellipsoid, box or cylinder in the "
							"scene -- the form a bulb, an orb, a lamp body or a glowing volume takes, and "
							"the physically based way to light one.  It is the closed-solid sibling of "
							"`rect_light` (a rectangular panel) and is expanded at parse time into the "
							"same four chunks an area light is otherwise written as -- a "
							"uniformcolor_painter holding `color`, a lambertian_luminaire_material whose "
							"exitance is that painter and whose scale is `exitance`, the shape's own "
							"geometry chunk, and a standard_object placing it at `center`.  Those three "
							"helper entities are named `<name>__pnt`, `<name>__mat` and `<name>__geo`; "
							"the standard_object takes `<name>` itself, so `<name>` is what render-time "
							"tools (solo, object map, isolate) report.  A collision on any of the four "
							"names fails the load with the ordinary duplicate-name error.  IT EMITS "
							"OUTWARD IN EVERY DIRECTION AND THERE IS NO FACING PARAMETER: each of these "
							"shapes is a closed solid whose surface normal points outward everywhere, and "
							"lambertian emission is one-sided about that normal, so the whole surface "
							"emits away from the solid and nothing emits into it.  (That is the one place "
							"it differs from rect_light, which is an open quad and therefore needs a "
							"`facing`.)  Being an ordinary object it is rendered like one: the camera sees "
							"its surface wherever it is, so it is both a light and a thing the picture "
							"shows.  It has real area, so it casts soft shadows and falls off with "
							"distance.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;     p.required = true;
						  p.description = "Unique name.  Names the OBJECT; the painter, material and geometry this chunk also creates are `<name>__pnt`, `<name>__mat` and `<name>__geo`"; }
						{ auto& p = P(); p.name = "parent"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Object}; p.description = "Object to parent the emitted object to (must be declared earlier); its transform is then LOCAL to that parent"; }
						{ auto& p = P(); p.name = "shape";       p.kind = ValueKind::Enum;       p.required = true;
						  p.enumValues = {"sphere","ellipsoid","box","cylinder"};
						  p.description = "Which closed solid emits.  It also fixes how many numbers `size` takes: " + ShapeLightValidShapes(); }
						{ auto& p = P(); p.name = "center";      p.kind = ValueKind::DoubleVec3; p.required = true;
						  p.description = "Centre of the solid, LOCAL to `parent` (world-space when unparented).  Every shape is built centred on its own origin and placed here"; }
						{ auto& p = P(); p.name = "size";        p.kind = ValueKind::Double;     p.required = true;
						  p.tupleKinds = {ValueKind::Double, ValueKind::Double, ValueKind::Double};
						  p.description = "Positive numbers in scene units, as many as the chosen `shape` takes: " + ShapeLightValidShapes(); p.unitLabel = "scene units"; }
						{ auto& p = P(); p.name = "exitance";    p.kind = ValueKind::Double;     p.required = true;
						  p.description = "Emitted radiance PER UNIT AREA, so the same number on a bigger solid delivers more light.  Must be greater than zero.  Existing scenes span four orders of magnitude: tens for a soft glowing body, thousands for a small bright bulb"; }
						{ auto& p = P(); p.name = "orientation"; p.kind = ValueKind::DoubleVec3;
						  p.description = "Euler orientation in DEGREES, applied to the solid about `center` -- the same parameter standard_object takes.  A sphere is unchanged by it; it is what turns a cylinder off its default +Y axis, and what tilts a box or an ellipsoid"; p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "color";       p.kind = ValueKind::DoubleVec3;
						  p.description = "R G B tint of the emitted light, linear Rec.709"; p.defaultValueHint = "1 1 1"; }
						return cd;
					}();
					return d;
				}
			};

			//////////////////////////////////////////
			// ShaderOps
			//////////////////////////////////////////

			struct PathTracingShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string  name             = bag.GetString( "name",                "noname" );
					bool         smsEnabled       = bag.GetBool(   "sms_enabled",         false );
					unsigned int smsMaxIterations = bag.GetUInt(   "sms_max_iterations",  20 );
					double       smsThreshold     = bag.GetDouble( "sms_threshold",       1e-5 );
					unsigned int smsMaxChainDepth = bag.GetUInt(   "sms_max_chain_depth", 10 );
					bool         smsBiased        = bag.GetBool(   "sms_biased",          true );

					return pJob.AddPathTracingShaderOp( name.c_str(), smsEnabled, smsMaxIterations, smsThreshold, smsMaxChainDepth, smsBiased );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "pathtracing_shaderop"; cd.category = ChunkCategory::ShaderOp;
						cd.description = "Unidirectional path-tracing shader op with optional Specular Manifold Sampling.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";                p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "sms_enabled";         p.kind = ValueKind::Bool;   p.description = "Enable SMS"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "sms_max_iterations";  p.kind = ValueKind::UInt;   p.description = "SMS Newton iterations"; p.defaultValueHint = "20"; }
						{ auto& p = P(); p.name = "sms_threshold";       p.kind = ValueKind::Double; p.description = "SMS convergence threshold"; p.defaultValueHint = "1e-5"; }
						{ auto& p = P(); p.name = "sms_max_chain_depth"; p.kind = ValueKind::UInt;   p.description = "SMS chain depth"; p.defaultValueHint = "10"; }
						{ auto& p = P(); p.name = "sms_biased";          p.kind = ValueKind::Bool;   p.description = "Use biased SMS estimator"; p.defaultValueHint = "TRUE"; }
						// Legacy parameters — accepted for backwards compat, silently ignored.
						// `branch` was retired (legacy distribution-tracing knob).
						{ auto& p = P(); p.name = "branch";               p.kind = ValueKind::Bool; p.description = "Legacy — ignored"; }
						{ auto& p = P(); p.name = "force_check_emitters"; p.kind = ValueKind::Bool; p.description = "Legacy — ignored"; }
						{ auto& p = P(); p.name = "finalgather";          p.kind = ValueKind::Bool; p.description = "Legacy — ignored"; }
						{ auto& p = P(); p.name = "reflections";          p.kind = ValueKind::Bool; p.description = "Legacy — ignored"; }
						{ auto& p = P(); p.name = "refractions";          p.kind = ValueKind::Bool; p.description = "Legacy — ignored"; }
						{ auto& p = P(); p.name = "diffuse";              p.kind = ValueKind::Bool; p.description = "Legacy — ignored"; }
						{ auto& p = P(); p.name = "translucents";         p.kind = ValueKind::Bool; p.description = "Legacy — ignored"; }
						return cd;
					}();
					return d;
				}
			};

			struct SMSShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string  name          = bag.GetString( "name",            "noname" );
					unsigned int maxIterations = bag.GetUInt(   "max_iterations",  20 );
					double       threshold     = bag.GetDouble( "threshold",       1e-5 );
					unsigned int maxChainDepth = bag.GetUInt(   "max_chain_depth", 10 );
					bool         biased        = bag.GetBool(   "biased",          true );

					return pJob.AddSMSShaderOp( name.c_str(), maxIterations, threshold, maxChainDepth, biased );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "sms_shaderop"; cd.category = ChunkCategory::ShaderOp;
						cd.description = "Specular Manifold Sampling shader op.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";             p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "max_iterations";   p.kind = ValueKind::UInt;   p.description = "Newton iterations"; p.defaultValueHint = "20"; }
						{ auto& p = P(); p.name = "threshold";        p.kind = ValueKind::Double; p.description = "Convergence threshold"; p.defaultValueHint = "1e-5"; }
						{ auto& p = P(); p.name = "max_chain_depth";  p.kind = ValueKind::UInt;   p.description = "Max manifold-chain depth"; p.defaultValueHint = "10"; }
						{ auto& p = P(); p.name = "biased";           p.kind = ValueKind::Bool;   p.description = "Biased SMS estimator"; p.defaultValueHint = "TRUE"; }
						return cd;
					}();
					return d;
				}
			};

			struct DistributionTracingShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string  name               = bag.GetString( "name",                 "noname" );
					unsigned int samples            = bag.GetUInt(   "samples",              16 );
					bool         irradiancecaching  = bag.GetBool(   "irradiance_caching",   false );
					bool         forcecheckemitters = bag.GetBool(   "force_check_emitters", false );
					bool         reflections        = bag.GetBool(   "reflections",          true );
					bool         refractions        = bag.GetBool(   "refractions",          true );
					bool         diffuse            = bag.GetBool(   "diffuse",              true );
					bool         translucents       = bag.GetBool(   "translucents",         true );

					return pJob.AddDistributionTracingShaderOp( name.c_str(), samples, irradiancecaching, forcecheckemitters, reflections, refractions, diffuse, translucents );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "distributiontracing_shaderop"; cd.category = ChunkCategory::ShaderOp;
						cd.description = "Distribution ray tracing shader op.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";                 p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "samples";              p.kind = ValueKind::UInt;   p.description = "Samples per hit"; p.defaultValueHint = "16"; }
						{ auto& p = P(); p.name = "irradiance_caching";   p.kind = ValueKind::Bool;   p.description = "Enable irradiance caching"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "force_check_emitters"; p.kind = ValueKind::Bool;   p.description = "Force emitter visibility checks"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "reflections";          p.kind = ValueKind::Bool;   p.description = "Trace reflection rays"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "refractions";          p.kind = ValueKind::Bool;   p.description = "Trace refraction rays"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "diffuse";              p.kind = ValueKind::Bool;   p.description = "Trace diffuse-reflection rays"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "translucents";         p.kind = ValueKind::Bool;   p.description = "Trace translucent rays"; p.defaultValueHint = "TRUE"; }
						// Legacy parameter — accepted for backwards compat, silently ignored.
						// `branch` was retired (legacy distribution-tracing knob).
						{ auto& p = P(); p.name = "branch";               p.kind = ValueKind::Bool;   p.description = "Legacy — ignored"; }
						return cd;
					}();
					return d;
				}
			};

			struct FinalGatherShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string  name = bag.GetString( "name", "noname" );

					unsigned int thetasamples = 15;
					unsigned int phisamples   = (unsigned int)(Scalar(thetasamples)*PI);

					if( bag.Has("theta_samples") ) thetasamples = bag.GetUInt("theta_samples");
					if( bag.Has("phi_samples") )   phisamples   = bag.GetUInt("phi_samples");
					if( bag.Has("samples") ) {
						const unsigned int samples = bag.GetUInt("samples");
						const Scalar base = sqrt(Scalar(samples)/PI);
						thetasamples = static_cast<unsigned int>( base );
						phisamples   = static_cast<unsigned int>( PI*base );
					}

					bool         cachegradients              = bag.GetBool(   "cachegradients",             true );
					unsigned int min_effective_contributors  = bag.GetUInt(   "min_effective_contributors", 2 );
					double       high_variation_reuse_scale  = bag.GetDouble( "high_variation_reuse_scale", 0.25 );
					bool         cache                       = bag.GetBool(   "cache",                      true );

					return pJob.AddFinalGatherShaderOp( name.c_str(), thetasamples, phisamples, cachegradients, min_effective_contributors, high_variation_reuse_scale, cache );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "finalgather_shaderop"; cd.category = ChunkCategory::ShaderOp;
						cd.description = "Irradiance-cache final-gather shader op.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";                        p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "theta_samples";               p.kind = ValueKind::UInt;   p.description = "Theta (elevation) samples"; p.defaultValueHint = "15"; }
						{ auto& p = P(); p.name = "phi_samples";                 p.kind = ValueKind::UInt;   p.description = "Phi (azimuth) samples"; p.defaultValueHint = "47"; }
						{ auto& p = P(); p.name = "samples";                     p.kind = ValueKind::UInt;   p.description = "Total samples — derives theta and phi automatically"; }
						{ auto& p = P(); p.name = "cachegradients";              p.kind = ValueKind::Bool;   p.description = "Cache irradiance gradients"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "min_effective_contributors";  p.kind = ValueKind::UInt;   p.description = "Min contributors for reuse"; p.defaultValueHint = "2"; }
						{ auto& p = P(); p.name = "high_variation_reuse_scale";  p.kind = ValueKind::Double; p.description = "Reuse-scale for high-variation regions"; p.defaultValueHint = "0.25"; }
						{ auto& p = P(); p.name = "cache";                       p.kind = ValueKind::Bool;   p.description = "Use irradiance cache"; p.defaultValueHint = "TRUE"; }
						return cd;
					}();
					return d;
				}
			};


			struct AmbientOcclusionShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );

					unsigned int numtheta = 5;
					unsigned int numphi   = 15;

					if( bag.Has("numtheta") ) numtheta = bag.GetUInt("numtheta");
					if( bag.Has("numphi") )   numphi   = bag.GetUInt("numphi");
					if( bag.Has("samples") ) {
						const unsigned int samples = bag.GetUInt("samples");
						const Scalar base = sqrt(Scalar(samples)/PI);
						numtheta = static_cast<unsigned int>( base );
						numphi   = static_cast<unsigned int>( PI*base );
					}

					bool multiplybrdf     = bag.GetBool( "multiplybrdf",     true );
					bool irradiance_cache = bag.GetBool( "irradiance_cache", false );

					return pJob.AddAmbientOcclusionShaderOp( name.c_str(), numtheta, numphi, multiplybrdf, irradiance_cache );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "ambientocclusion_shaderop"; cd.category = ChunkCategory::ShaderOp;
						cd.description = "Screen-space / hemisphere ambient occlusion. A geometry-presence query -- an occluder's `casts_shadows FALSE` does NOT exempt it from occluding here.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";             p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "numtheta";         p.kind = ValueKind::UInt;   p.description = "Elevation samples"; p.defaultValueHint = "5"; }
						{ auto& p = P(); p.name = "numphi";           p.kind = ValueKind::UInt;   p.description = "Azimuth samples"; p.defaultValueHint = "15"; }
						{ auto& p = P(); p.name = "samples";          p.kind = ValueKind::UInt;   p.description = "Total samples — derives numtheta and numphi automatically"; }
						{ auto& p = P(); p.name = "multiplybrdf";     p.kind = ValueKind::Bool;   p.description = "Multiply by surface BRDF"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "irradiance_cache"; p.kind = ValueKind::Bool;   p.description = "Use irradiance cache"; p.defaultValueHint = "FALSE"; }
						return cd;
					}();
					return d;
				}
			};

			struct DirectLightingShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );
					std::string bsdf = bag.GetString( "bsdf", "none" );

					return pJob.AddDirectLightingShaderOp( name.c_str(), bsdf=="none"?0:bsdf.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "directlighting_shaderop"; cd.category = ChunkCategory::ShaderOp;
						cd.description = "Direct lighting only (no indirect).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name"; p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "bsdf"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Material}; p.description = "Override BSDF"; }
						// Legacy parameters — accepted for backwards compat, silently ignored.
						// The unified LightSampler now handles both analytic and mesh lights through
						// one path, and `cache` was never correct with the stochastic sampler.
						{ auto& p = P(); p.name = "nonmeshlights"; p.kind = ValueKind::Bool; p.description = "Legacy — ignored"; }
						{ auto& p = P(); p.name = "meshlights";    p.kind = ValueKind::Bool; p.description = "Legacy — ignored"; }
						{ auto& p = P(); p.name = "cache";         p.kind = ValueKind::Bool; p.description = "Legacy — ignored"; }
						return cd;
					}();
					return d;
				}
			};

			struct SimpleSubSurfaceScatteringShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string  name             = bag.GetString( "name",             "noname" );
					unsigned int numpoints        = bag.GetUInt(   "numpoints",        1000 );
					double       error            = bag.GetDouble( "error",            0.001 );
					unsigned int maxPointsPerNode = bag.GetUInt(   "maxpointspernode", 40 );
					unsigned char maxDepth        = static_cast<unsigned char>( bag.GetUInt( "maxdepth", 8 ) );
					double       irrad_scale      = bag.GetDouble( "irrad_scale",      1.0 );
					double       geometric_scale  = bag.GetDouble( "geometric_scale",  1.0 );
					bool         multiplyBSDF     = bag.GetBool(   "multiplybsdf",     false );
					bool         regenerate       = bag.GetBool(   "regenerate",       true );
					std::string  shader           = bag.GetString( "shader",           "none" );
					bool         cache            = bag.GetBool(   "cache",            true );
					bool         low_discrepancy  = bag.GetBool(   "low_discrepancy",  true );
					double       extinction[3]   = {0.02, 0.03, 0.09};
					bag.GetVec3( "extinction", extinction );

					return pJob.AddSimpleSubSurfaceScatteringShaderOp( name.c_str(), numpoints, error, maxPointsPerNode, maxDepth, irrad_scale, geometric_scale, multiplyBSDF, regenerate, shader.c_str(), cache, low_discrepancy, extinction );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "simple_sss_shaderop"; cd.category = ChunkCategory::ShaderOp;
						cd.description = "Simple point-cloud subsurface scattering.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";             p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "numpoints";        p.kind = ValueKind::UInt;   p.description = "Sample points per object"; p.defaultValueHint = "1000"; }
						{ auto& p = P(); p.name = "error";            p.kind = ValueKind::Double; p.description = "Octree error threshold"; p.defaultValueHint = "0.001"; }
						{ auto& p = P(); p.name = "maxpointspernode"; p.kind = ValueKind::UInt;   p.description = "Octree leaf capacity"; p.defaultValueHint = "40"; }
						{ auto& p = P(); p.name = "maxdepth";         p.kind = ValueKind::UInt;   p.description = "Octree depth"; p.defaultValueHint = "8"; }
						{ auto& p = P(); p.name = "irrad_scale";      p.kind = ValueKind::Double; p.description = "Irradiance scale"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "geometric_scale";  p.kind = ValueKind::Double; p.description = "Geometric scale"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "multiplybsdf";     p.kind = ValueKind::Bool;   p.description = "Multiply result by BSDF"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "regenerate";       p.kind = ValueKind::Bool;   p.description = "Re-build cache each frame"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "shader";           p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Shader}; p.description = "Sample-irradiance shader"; }
						{ auto& p = P(); p.name = "cache";            p.kind = ValueKind::Bool;   p.description = "Cache sample irradiances"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "low_discrepancy";  p.kind = ValueKind::Bool;   p.description = "Use low-discrepancy sampling"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "extinction";       p.kind = ValueKind::DoubleVec3; p.description = "Extinction coefficient"; p.defaultValueHint = "0.02 0.03 0.09"; }
						return cd;
					}();
					return d;
				}
			};

			struct DiffusionApproximationSubSurfaceScatteringShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string  name             = bag.GetString( "name",             "noname" );
					unsigned int numpoints        = bag.GetUInt(   "numpoints",        1000 );
					double       error            = bag.GetDouble( "error",            0.001 );
					unsigned int maxPointsPerNode = bag.GetUInt(   "maxpointspernode", 40 );
					unsigned char maxDepth        = static_cast<unsigned char>( bag.GetUInt( "maxdepth", 8 ) );
					double       irrad_scale      = bag.GetDouble( "irrad_scale",      1.0 );
					double       geometric_scale  = bag.GetDouble( "geometric_scale",  1.0 );
					bool         multiplyBSDF     = bag.GetBool(   "multiplybsdf",     false );
					bool         regenerate       = bag.GetBool(   "regenerate",       true );
					std::string  shader           = bag.GetString( "shader",           "none" );
					bool         cache            = bag.GetBool(   "cache",            true );
					bool         low_discrepancy  = bag.GetBool(   "low_discrepancy",  true );
					double       scattering[3]   = {2.19, 2.62, 3.0};
					double       absorption[3]   = {0.0021, 0.0041, 0.0071};
					bag.GetVec3( "scattering", scattering );
					bag.GetVec3( "absorption", absorption );
					double       ior              = bag.GetDouble( "ior",              1.3 );
					double       g                = bag.GetDouble( "g",                0.8 );

					return pJob.AddDiffusionApproximationSubSurfaceScatteringShaderOp( name.c_str(), numpoints, error, maxPointsPerNode, maxDepth, irrad_scale, geometric_scale, multiplyBSDF, regenerate, shader.c_str(), cache, low_discrepancy, scattering, absorption, ior, g );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "diffusion_approximation_sss_shaderop"; cd.category = ChunkCategory::ShaderOp;
						cd.description = "Diffusion-approximation point-cloud SSS (Jensen et al.).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";             p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "numpoints";        p.kind = ValueKind::UInt;   p.description = "Sample points per object"; p.defaultValueHint = "1000"; }
						{ auto& p = P(); p.name = "error";            p.kind = ValueKind::Double; p.description = "Octree error threshold"; p.defaultValueHint = "0.001"; }
						{ auto& p = P(); p.name = "maxpointspernode"; p.kind = ValueKind::UInt;   p.description = "Octree leaf capacity"; p.defaultValueHint = "40"; }
						{ auto& p = P(); p.name = "maxdepth";         p.kind = ValueKind::UInt;   p.description = "Octree depth"; p.defaultValueHint = "8"; }
						{ auto& p = P(); p.name = "irrad_scale";      p.kind = ValueKind::Double; p.description = "Irradiance scale"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "geometric_scale";  p.kind = ValueKind::Double; p.description = "Geometric scale"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "multiplybsdf";     p.kind = ValueKind::Bool;   p.description = "Multiply by BSDF"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "regenerate";       p.kind = ValueKind::Bool;   p.description = "Re-build cache each frame"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "shader";           p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Shader}; p.description = "Sample-irradiance shader"; }
						{ auto& p = P(); p.name = "cache";            p.kind = ValueKind::Bool;   p.description = "Cache irradiances"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "low_discrepancy";  p.kind = ValueKind::Bool;   p.description = "Low-discrepancy sampling"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "scattering";       p.kind = ValueKind::DoubleVec3; p.description = "Scattering coefficient"; p.defaultValueHint = "2.19 2.62 3.0"; }
						{ auto& p = P(); p.name = "absorption";       p.kind = ValueKind::DoubleVec3; p.description = "Absorption coefficient"; p.defaultValueHint = "0.0021 0.0041 0.0071"; }
						{ auto& p = P(); p.name = "ior";              p.kind = ValueKind::Double; p.description = "Index of refraction"; p.defaultValueHint = "1.3"; }
						{ auto& p = P(); p.name = "g";                p.kind = ValueKind::Double; p.description = "Henyey-Greenstein g"; p.defaultValueHint = "0.8"; }
						return cd;
					}();
					return d;
				}
			};

			struct DonnerJensenSkinSSSShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string  name             = bag.GetString( "name",             "noname" );
					unsigned int numpoints        = bag.GetUInt(   "numpoints",        10000 );
					double       error            = bag.GetDouble( "error",            0.001 );
					unsigned int maxPointsPerNode = bag.GetUInt(   "maxpointspernode", 40 );
					unsigned char maxDepth        = static_cast<unsigned char>( bag.GetUInt( "maxdepth", 8 ) );
					double       irrad_scale      = bag.GetDouble( "irrad_scale",      1.0 );
					std::string  shader           = bag.GetString( "shader",           "none" );
					bool         cache            = bag.GetBool(   "cache",            true );

					double melanin_fraction     = bag.GetDouble( "melanin_fraction",     0.02 );
					double melanin_blend        = bag.GetDouble( "melanin_blend",        0.5 );
					double hemoglobin_epidermis = bag.GetDouble( "hemoglobin_epidermis", 0.002 );
					double carotene_fraction    = bag.GetDouble( "carotene_fraction",    0.001 );
					double hemoglobin_dermis    = bag.GetDouble( "hemoglobin_dermis",    0.005 );
					double epidermis_thickness  = bag.GetDouble( "epidermis_thickness",  0.025 );
					double ior_epidermis        = bag.GetDouble( "ior_epidermis",        1.4 );
					double ior_dermis           = bag.GetDouble( "ior_dermis",           1.38 );
					double blood_oxygenation    = bag.GetDouble( "blood_oxygenation",    0.7 );

					std::string melanin_fraction_offset     = bag.GetString( "melanin_fraction_offset",     "" );
					std::string hemoglobin_epidermis_offset = bag.GetString( "hemoglobin_epidermis_offset", "" );
					std::string hemoglobin_dermis_offset    = bag.GetString( "hemoglobin_dermis_offset",    "" );

					return pJob.AddDonnerJensenSkinSSSShaderOp( name.c_str(),
						numpoints, error, maxPointsPerNode, maxDepth, irrad_scale,
						shader.c_str(), cache,
						melanin_fraction, melanin_blend, hemoglobin_epidermis,
						carotene_fraction, hemoglobin_dermis, epidermis_thickness,
						ior_epidermis, ior_dermis, blood_oxygenation,
						melanin_fraction_offset.c_str(),
						hemoglobin_epidermis_offset.c_str(),
						hemoglobin_dermis_offset.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "donner_jensen_skin_sss_shaderop"; cd.category = ChunkCategory::ShaderOp;
						cd.description = "Donner-Jensen spectral skin SSS shader op.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						static const char* doubles[] = {
							"melanin_fraction","melanin_blend","hemoglobin_epidermis","carotene_fraction",
							"hemoglobin_dermis","epidermis_thickness","ior_epidermis","ior_dermis","blood_oxygenation"
						};
						{ auto& p = P(); p.name = "name";             p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "numpoints";        p.kind = ValueKind::UInt;   p.description = "Sample points per object"; p.defaultValueHint = "10000"; }
						{ auto& p = P(); p.name = "error";            p.kind = ValueKind::Double; p.description = "Octree error"; p.defaultValueHint = "0.001"; }
						{ auto& p = P(); p.name = "maxpointspernode"; p.kind = ValueKind::UInt;   p.description = "Octree leaf capacity"; p.defaultValueHint = "40"; }
						{ auto& p = P(); p.name = "maxdepth";         p.kind = ValueKind::UInt;   p.description = "Octree depth"; p.defaultValueHint = "8"; }
						{ auto& p = P(); p.name = "irrad_scale";      p.kind = ValueKind::Double; p.description = "Irradiance scale"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "shader";           p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Shader}; p.description = "Irradiance-gather shader"; }
						{ auto& p = P(); p.name = "cache";            p.kind = ValueKind::Bool;   p.description = "Cache sample irradiances"; p.defaultValueHint = "TRUE"; }
						for (const char* n : doubles) {
							auto& p = P(); p.name = n; p.kind = ValueKind::Double; p.description = "Donner-Jensen skin parameter";
						}
						{ auto& p = P(); p.name = "melanin_fraction_offset";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Spatial melanin offset"; }
						{ auto& p = P(); p.name = "hemoglobin_epidermis_offset";p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Spatial hb-epi offset"; }
						{ auto& p = P(); p.name = "hemoglobin_dermis_offset";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Spatial hb-derm offset"; }
						return cd;
					}();
					return d;
				}
			};

			struct AreaLightShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string  name     = bag.GetString( "name",   "noname" );
					double       width    = bag.GetDouble( "width",  1.0 );
					double       height   = bag.GetDouble( "height", 1.0 );
					double       location[3] = {0, 10, 0};
					double       dir[3]      = {0, -1, 0};
					bag.GetVec3( "location", location );
					bag.GetVec3( "dir",      dir );

					// `make_dir` derives `dir` from a target point relative to location
					if( bag.Has("make_dir") ) {
						double target[3] = {0,0,0};
						bag.GetVec3( "make_dir", target );
						dir[0] = target[0] - location[0];
						dir[1] = target[1] - location[1];
						dir[2] = target[2] - location[2];
					}

					unsigned int samples = bag.GetUInt(   "samples",  9 );
					std::string  emm     = bag.GetString( "emission", "color_white" );
					Scalar       power   = bag.GetDouble( "power",    1.0 );
					std::string  N       = bag.GetString( "N",        "1.0" );
					Scalar       hotspot = bag.Has("hotspot") ? bag.GetDouble("hotspot") * DEG_TO_RAD : PI;
					bool         cache   = bag.GetBool(   "cache",    false );

					return pJob.AddAreaLightShaderOp( name.c_str(), width, height, location, dir, samples, emm.c_str(), power, N.c_str(), hotspot, cache );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "arealight_shaderop"; cd.category = ChunkCategory::ShaderOp;
						cd.description = "Area-light shader op (direct sampling of an emitting rectangle).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";     p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "width";    p.kind = ValueKind::Double;     p.description = "Rectangle width"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "height";   p.kind = ValueKind::Double;     p.description = "Rectangle height"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "location"; p.kind = ValueKind::DoubleVec3; p.description = "World-space center"; p.defaultValueHint = "0 10 0"; }
						{ auto& p = P(); p.name = "dir";      p.kind = ValueKind::DoubleVec3; p.description = "Rectangle normal"; p.defaultValueHint = "0 -1 0"; }
						{ auto& p = P(); p.name = "make_dir"; p.kind = ValueKind::DoubleVec3; p.description = "Derive dir from target = make_dir - location"; }
						{ auto& p = P(); p.name = "samples";  p.kind = ValueKind::UInt;       p.description = "Samples per shade"; p.defaultValueHint = "9"; }
						{ auto& p = P(); p.name = "emission"; p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "Emission colour"; p.defaultValueHint = "color_white"; }
						{ auto& p = P(); p.name = "power";    p.kind = ValueKind::Double;     p.description = "Radiant power"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "N";        p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "Directionality (Phong) exponent (physical SCALAR, single value: a scalar_painter name, or an inline scalar -- a COLOUR painter does not bind here)"; p.defaultValueHint = "1.0"; p.semantics.pipe = ParameterPipe::Scalar; }
						{ auto& p = P(); p.name = "hotspot";  p.kind = ValueKind::Double;     p.description = "Hotspot half-angle (degrees)"; p.defaultValueHint = "180"; }
						{ auto& p = P(); p.name = "cache";    p.kind = ValueKind::Bool;       p.description = "Cache direct-light estimate"; p.defaultValueHint = "FALSE"; }
						return cd;
					}();
					return d;
				}
			};

			struct TransparencyShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name      = bag.GetString( "name",         "noname" );
					std::string trans     = bag.GetString( "transparency", "color_white" );
					bool        one_sided = bag.GetBool(   "one_sided",    false );

					return pJob.AddTransparencyShaderOp( name.c_str(), trans.c_str(), one_sided );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "transparency_shaderop"; cd.category = ChunkCategory::ShaderOp;
						cd.description = "Alpha-transparency shader op.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";         p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "transparency"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Alpha painter"; p.defaultValueHint = "color_white"; }
						{ auto& p = P(); p.name = "one_sided";    p.kind = ValueKind::Bool;      p.description = "Transparent from one side only"; p.defaultValueHint = "FALSE"; }
						return cd;
					}();
					return d;
				}
			};

			struct AlphaTestShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name   = bag.GetString( "name",   "noname" );
					std::string alpha  = bag.GetString( "alpha",  "color_white" );
					double      cutoff = bag.GetDouble( "cutoff", 0.5 );
					return pJob.AddAlphaTestShaderOp( name.c_str(), alpha.c_str(), cutoff );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "alpha_test_shaderop"; cd.category = ChunkCategory::ShaderOp;
						cd.description = "Cutout-alpha (glTF alphaMode=MASK) shader op.  At hit time samples "
							"`alpha` from the painter; if alpha < cutoff the ray is forwarded past the "
							"surface as if it never hit.  Caveat: only honoured by integrators that go "
							"through IShader::Shade() (path tracer + legacy direct shaders); BDPT, VCM, "
							"MLT, and photon tracers treat the surface as fully opaque.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";   p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "alpha";  p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Alpha painter"; p.defaultValueHint = "color_white"; }
						{ auto& p = P(); p.name = "cutoff"; p.kind = ValueKind::Double;    p.description = "Alpha threshold (alpha < cutoff continues the ray past the surface)"; p.defaultValueHint = "0.5"; }
						return cd;
					}();
					return d;
				}
			};

			//////////////////////////////////////////
			// Shaders
			//////////////////////////////////////////

			struct StandardShaderAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );

					const std::vector<std::string>& shaderops = bag.GetRepeatable( "shaderop" );
					const unsigned int num = static_cast<unsigned int>(shaderops.size());

					char* shmem = new char[num*256];
					memset( shmem, 0, num*256 );
					char** shops = new char*[num];

					for( unsigned int i=0; i<num; i++ ) {
						shops[i] = &shmem[i*256];
						strncpy( shops[i], shaderops[i].c_str(), 255 );
					}

					bool bRet = pJob.AddStandardShader( name.c_str(), num, (const char**)shops );

					delete [] shops;
					delete [] shmem;

					return bRet;
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "standard_shader"; cd.category = ChunkCategory::Shader;
						cd.description = "Linear chain of shader ops evaluated per hit.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";     p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "shaderop"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::ShaderOp}; p.repeatable = true; p.description = "Shader op to chain (repeatable)"; }
						return cd;
					}();
					return d;
				}
			};

			struct AdvancedShaderAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "noname" );

					std::vector<String> shaderops;
					std::vector<unsigned int> mins, maxs;
					std::vector<char> operations;

					// Repeatable composite tokens: "<shaderop-name> <min-depth> <max-depth> <op>"
					const std::vector<std::string>& sops = bag.GetRepeatable( "shaderop" );
					for( size_t k = 0; k < sops.size(); ++k ) {
						char buf[256] = {0};
						unsigned int min=1, max=10000;
						char operation = '+';
						sscanf( sops[k].c_str(), "%s %u %u %c", buf, &min, &max, &operation );
						shaderops.push_back( String(buf) );
						mins.push_back( min );
						maxs.push_back( max );
						operations.push_back( operation );
					}

					const unsigned int num = static_cast<unsigned int>(shaderops.size());
					char* shmem = new char[num*256];
					memset( shmem, 0, num*256 );
					char** shops = new char*[num];

					for( unsigned int i=0; i<num; i++ ) {
						shops[i] = &shmem[i*256];
						strncpy( shops[i], shaderops[i].c_str(), 255 );
					}

					bool bRet = pJob.AddAdvancedShader( name.c_str(), num, (const char**)shops, (unsigned int*)(&(*(mins.begin()))), (unsigned int*)(&(*(maxs.begin()))), (char*)(&(*(operations.begin()))) );

					delete [] shops;
					delete [] shmem;

					return bRet;
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "advanced_shader"; cd.category = ChunkCategory::Shader;
						cd.description = "Depth-scoped shader chain with per-op recursion ranges and composition operators.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";     p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "shaderop"; p.kind = ValueKind::String; p.repeatable = true; p.tupleKinds = {ValueKind::Reference, ValueKind::UInt, ValueKind::UInt, ValueKind::Enum}; p.referenceCategories = {ChunkCategory::ShaderOp}; p.description = "Shader-op triple: <shaderop-name> <min-depth> <max-depth> <op>"; }
						return cd;
					}();
					return d;
				}
			};

			struct DirectVolumeRenderingShaderAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string  name              = bag.GetString( "name",            "noname" );
					std::string  szVolumeFilePattern = bag.GetString( "file_pattern",  "" );
					std::string  iso_shader        = bag.GetString( "iso_shader",      "none" );
					unsigned int width             = bag.GetUInt(   "width",           0 );
					unsigned int height            = bag.GetUInt(   "height",          0 );
					unsigned int startz            = bag.GetUInt(   "startz",          0 );
					unsigned int endz              = bag.GetUInt(   "endz",            0 );

					std::string accessorS  = bag.GetString( "accessor",  "n" );
					std::string gradientS  = bag.GetString( "gradient",  "i" );
					std::string compositeS = bag.GetString( "composite", "c" );
					std::string samplerS   = bag.GetString( "sampler",   "u" );
					char accessor  = accessorS.empty()  ? 'n' : (char)tolower( accessorS[0] );
					char gradient  = gradientS.empty()  ? 'i' : (char)tolower( gradientS[0] );
					char composite = compositeS.empty() ? 'c' : (char)tolower( compositeS[0] );
					char sampler   = samplerS.empty()   ? 'u' : (char)tolower( samplerS[0] );

					double       dThresholdStart   = bag.GetDouble( "threshold_start", 0.4 );
					double       dThresholdEnd     = bag.GetDouble( "threshold_end",   1.0 );
					unsigned int samples           = bag.GetUInt(   "samples",         50 );
					std::string  transfer_red      = bag.GetString( "transfer_red",    "none" );
					std::string  transfer_green    = bag.GetString( "transfer_green",  "none" );
					std::string  transfer_blue     = bag.GetString( "transfer_blue",   "none" );
					std::string  transfer_alpha    = bag.GetString( "transfer_alpha",  "none" );

					return pJob.AddDirectVolumeRenderingShader( name.c_str(), szVolumeFilePattern.c_str(), width, height, startz, endz,
						accessor, gradient, composite, dThresholdStart, dThresholdEnd, sampler, samples, transfer_red.c_str(), transfer_green.c_str(), transfer_blue.c_str(), transfer_alpha.c_str(), iso_shader=="none"?0:iso_shader.c_str()
						);
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "directvolumerendering_shader"; cd.category = ChunkCategory::Shader;
						cd.description = "Direct volume rendering shader.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";            p.kind = ValueKind::String;   p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "file_pattern";    p.kind = ValueKind::String;   p.description = "Volume file pattern"; }
						{ auto& p = P(); p.name = "width";           p.kind = ValueKind::UInt;     p.description = "Volume width"; }
						{ auto& p = P(); p.name = "height";          p.kind = ValueKind::UInt;     p.description = "Volume height"; }
						{ auto& p = P(); p.name = "startz";          p.kind = ValueKind::UInt;     p.description = "Start slice index"; }
						{ auto& p = P(); p.name = "endz";            p.kind = ValueKind::UInt;     p.description = "End slice index"; }
						{ auto& p = P(); p.name = "accessor";        p.kind = ValueKind::Enum;     p.enumValues = {"n","t"}; p.description = "Voxel accessor"; p.defaultValueHint = "n"; }
						{ auto& p = P(); p.name = "gradient";        p.kind = ValueKind::Enum;     p.enumValues = {"i","c","s"}; p.description = "Gradient estimator"; p.defaultValueHint = "i"; }
						{ auto& p = P(); p.name = "composite";       p.kind = ValueKind::Enum;     p.enumValues = {"c","m","i"}; p.description = "Compositing op"; p.defaultValueHint = "c"; }
						{ auto& p = P(); p.name = "threshold_start"; p.kind = ValueKind::Double;   p.description = "Low opacity cutoff"; p.defaultValueHint = "0.4"; }
						{ auto& p = P(); p.name = "threshold_end";   p.kind = ValueKind::Double;   p.description = "High opacity cutoff"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "sampler";         p.kind = ValueKind::Enum;     p.enumValues = {"u","s"}; p.description = "Ray sampler"; p.defaultValueHint = "u"; }
						{ auto& p = P(); p.name = "samples";         p.kind = ValueKind::UInt;     p.description = "Samples along ray"; p.defaultValueHint = "50"; }
						{ auto& p = P(); p.name = "transfer_red";    p.kind = ValueKind::Reference;p.referenceCategories = {ChunkCategory::Painter,ChunkCategory::Function}; p.description = "R transfer function"; }
						{ auto& p = P(); p.name = "transfer_green";  p.kind = ValueKind::Reference;p.referenceCategories = {ChunkCategory::Painter,ChunkCategory::Function}; p.description = "G transfer function"; }
						{ auto& p = P(); p.name = "transfer_blue";   p.kind = ValueKind::Reference;p.referenceCategories = {ChunkCategory::Painter,ChunkCategory::Function}; p.description = "B transfer function"; }
						{ auto& p = P(); p.name = "transfer_alpha";  p.kind = ValueKind::Reference;p.referenceCategories = {ChunkCategory::Painter,ChunkCategory::Function}; p.description = "A transfer function"; }
						{ auto& p = P(); p.name = "iso_shader";      p.kind = ValueKind::Reference;p.referenceCategories = {ChunkCategory::Shader}; p.description = "Iso-surface shader"; }
						return cd;
					}();
					return d;
				}
			};

			struct SpectralDirectVolumeRenderingShaderAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string  name              = bag.GetString( "name",            "noname" );
					std::string  szVolumeFilePattern = bag.GetString( "file_pattern",  "" );
					std::string  iso_shader        = bag.GetString( "iso_shader",      "none" );
					unsigned int width             = bag.GetUInt(   "width",           0 );
					unsigned int height            = bag.GetUInt(   "height",          0 );
					unsigned int startz            = bag.GetUInt(   "startz",          0 );
					unsigned int endz              = bag.GetUInt(   "endz",            0 );

					std::string accessorS  = bag.GetString( "accessor",  "n" );
					std::string gradientS  = bag.GetString( "gradient",  "i" );
					std::string compositeS = bag.GetString( "composite", "c" );
					std::string samplerS   = bag.GetString( "sampler",   "u" );
					char accessor  = accessorS.empty()  ? 'n' : (char)tolower( accessorS[0] );
					char gradient  = gradientS.empty()  ? 'i' : (char)tolower( gradientS[0] );
					char composite = compositeS.empty() ? 'c' : (char)tolower( compositeS[0] );
					char sampler   = samplerS.empty()   ? 'u' : (char)tolower( samplerS[0] );

					double       dThresholdStart    = bag.GetDouble( "threshold_start",   0.4 );
					double       dThresholdEnd      = bag.GetDouble( "threshold_end",     1.0 );
					unsigned int samples            = bag.GetUInt(   "samples",           50 );
					std::string  transfer_spectral  = bag.GetString( "transfer_spectral", "none" );
					std::string  transfer_alpha     = bag.GetString( "transfer_alpha",    "none" );

					return pJob.AddSpectralDirectVolumeRenderingShader( name.c_str(), szVolumeFilePattern.c_str(), width, height, startz, endz,
						accessor, gradient, composite, dThresholdStart, dThresholdEnd, sampler, samples, transfer_alpha.c_str(), transfer_spectral.c_str(), iso_shader=="none"?0:iso_shader.c_str()
						);
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "spectraldirectvolumerendering_shader"; cd.category = ChunkCategory::Shader;
						cd.description = "Spectral direct volume rendering shader.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";              p.kind = ValueKind::String;   p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "file_pattern";      p.kind = ValueKind::String;   p.description = "Volume file pattern"; }
						{ auto& p = P(); p.name = "width";             p.kind = ValueKind::UInt;     p.description = "Volume width"; }
						{ auto& p = P(); p.name = "height";            p.kind = ValueKind::UInt;     p.description = "Volume height"; }
						{ auto& p = P(); p.name = "startz";            p.kind = ValueKind::UInt;     p.description = "Start slice"; }
						{ auto& p = P(); p.name = "endz";              p.kind = ValueKind::UInt;     p.description = "End slice"; }
						{ auto& p = P(); p.name = "accessor";          p.kind = ValueKind::Enum;     p.enumValues = {"n","t"}; p.description = "Voxel accessor"; p.defaultValueHint = "n"; }
						{ auto& p = P(); p.name = "gradient";          p.kind = ValueKind::Enum;     p.enumValues = {"i","c","s"}; p.description = "Gradient estimator"; p.defaultValueHint = "i"; }
						{ auto& p = P(); p.name = "composite";         p.kind = ValueKind::Enum;     p.enumValues = {"c","m","i"}; p.description = "Compositing op"; p.defaultValueHint = "c"; }
						{ auto& p = P(); p.name = "threshold_start";   p.kind = ValueKind::Double;   p.description = "Low opacity cutoff"; p.defaultValueHint = "0.4"; }
						{ auto& p = P(); p.name = "threshold_end";     p.kind = ValueKind::Double;   p.description = "High opacity cutoff"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "sampler";           p.kind = ValueKind::Enum;     p.enumValues = {"u","s"}; p.description = "Ray sampler"; p.defaultValueHint = "u"; }
						{ auto& p = P(); p.name = "samples";           p.kind = ValueKind::UInt;     p.description = "Samples along ray"; p.defaultValueHint = "50"; }
						{ auto& p = P(); p.name = "transfer_alpha";    p.kind = ValueKind::Reference;p.referenceCategories = {ChunkCategory::Painter,ChunkCategory::Function}; p.description = "Alpha transfer function"; }
						{ auto& p = P(); p.name = "transfer_spectral"; p.kind = ValueKind::Reference;p.referenceCategories = {ChunkCategory::Function}; p.description = "Spectral transfer function"; }
						{ auto& p = P(); p.name = "iso_shader";        p.kind = ValueKind::Reference;p.referenceCategories = {ChunkCategory::Shader}; p.description = "Iso-surface shader"; }
						return cd;
					}();
					return d;
				}
			};

			//////////////////////////////////////////
			// Rasterizers
			//////////////////////////////////////////

			// (Helper templates AddStabilityConfigParams / AddPathGuidingParams
			//  / AddAdaptiveSamplingParams / AddPixelFilterParams /
			//  AddRadianceMapParams / AddProgressiveParams /
			//  AddSpectralConfigParams / AddSMSConfigParams /
			//  AddPhotonMapGenerate*/Gather* / AddCameraCommonParams /
			//  AddNoisePainterCommonParams / AddBaseRasterizerParams /
			//  AddOptimalMISParams are defined above the Painters section
			//  so every chunk parser can reference them.)

			struct PixelPelRasterizerAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					PixelPelDefaults dflt;
					std::string defaultshader   = bag.GetString( "defaultshader",  dflt.defaultShader );
					unsigned int maxRecur       = bag.GetUInt(   "max_recursion",  dflt.maxRecursion );
					unsigned int numSamples     = bag.GetUInt(   "samples",        dflt.numPixelSamples );
					unsigned int numLumSamples  = bag.GetUInt(   "lum_samples",    dflt.numLumSamples );
					std::string luminarySampler = bag.GetString( "luminary_sampler", dflt.luminarySampler );
					double luminarySamplerParam = bag.GetDouble( "luminary_sampler_param", dflt.luminarySamplerParam );
					bool showLuminaires         = bag.GetBool(   "show_luminaires", dflt.showLuminaires );
					bool oidnDenoise            = bag.GetBool(   "oidn_denoise",    dflt.oidnDenoise );
					OidnQuality oidnQuality     = ParseOidnQuality(   bag.GetString( "oidn_quality",   to_hint(dflt.oidnQuality) ) );
					OidnDevice  oidnDevice      = ParseOidnDevice(    bag.GetString( "oidn_device",    to_hint(dflt.oidnDevice) ) );
					OidnPrefilter oidnPrefilter = ParseOidnPrefilter( bag.GetString( "oidn_prefilter", to_hint(dflt.oidnPrefilter) ) );

					RadianceMapConfig radianceMapConfig;
					if( bag.Has("radiance_map") )        radianceMapConfig.name         = String(bag.GetString("radiance_map").c_str());
					if( bag.Has("radiance_scale") )      radianceMapConfig.scale        = bag.GetDouble("radiance_scale");
					if( bag.Has("radiance_background") ) radianceMapConfig.isBackground = bag.GetBool("radiance_background");
					if( bag.Has("radiance_orient") ) {
						bag.GetVec3( "radiance_orient", radianceMapConfig.orientation );
						radianceMapConfig.orientation[0] *= DEG_TO_RAD;
						radianceMapConfig.orientation[1] *= DEG_TO_RAD;
						radianceMapConfig.orientation[2] *= DEG_TO_RAD;
					}

					PixelFilterConfig pixelFilterConfig;
					if( bag.Has("blue_noise_sampler") )  pixelFilterConfig.blueNoiseSampler = bag.GetBool("blue_noise_sampler");
					if( bag.Has("pixel_sampler") )       pixelFilterConfig.pixelSampler     = String(bag.GetString("pixel_sampler").c_str());
					if( bag.Has("pixel_sampler_param") ) pixelFilterConfig.pixelSamplerParam= bag.GetDouble("pixel_sampler_param");
					if( bag.Has("pixel_filter") )        pixelFilterConfig.filter           = String(bag.GetString("pixel_filter").c_str());
					if( bag.Has("pixel_filter_width") )  pixelFilterConfig.width            = bag.GetDouble("pixel_filter_width");
					if( bag.Has("pixel_filter_height") ) pixelFilterConfig.height           = bag.GetDouble("pixel_filter_height");
					if( bag.Has("pixel_filter_paramA") ) pixelFilterConfig.paramA           = bag.GetDouble("pixel_filter_paramA");
					if( bag.Has("pixel_filter_paramB") ) pixelFilterConfig.paramB           = bag.GetDouble("pixel_filter_paramB");

					PathGuidingConfig guidingConfig;
					if( bag.Has("pathguiding") )                                         guidingConfig.enabled                = bag.GetBool("pathguiding");
					if( bag.Has("pathguiding_iterations") )                              guidingConfig.trainingIterations     = bag.GetUInt("pathguiding_iterations");
					if( bag.Has("pathguiding_spp") )                                     guidingConfig.trainingSPP            = bag.GetUInt("pathguiding_spp");
					if( bag.Has("pathguiding_combine_training") )                        guidingConfig.combineTrainingIterations = bag.GetBool("pathguiding_combine_training");
					if( bag.Has("pathguiding_online") )                                  guidingConfig.online                 = bag.GetBool("pathguiding_online");
					if( bag.Has("pathguiding_warmup_iterations") )                       guidingConfig.warmupIterations       = bag.GetUInt("pathguiding_warmup_iterations");
					if( bag.Has("pathguiding_alpha") )                                   guidingConfig.alpha                  = bag.GetDouble("pathguiding_alpha");
				if( bag.Has("pathguiding_learned_alpha") )                           guidingConfig.learnedAlpha           = bag.GetBool("pathguiding_learned_alpha");
					if( bag.Has("pathguiding_max_depth") )                               guidingConfig.maxGuidingDepth        = bag.GetUInt("pathguiding_max_depth");
					if( bag.Has("pathguiding_light_max_depth") )                         guidingConfig.maxLightGuidingDepth   = bag.GetUInt("pathguiding_light_max_depth");
					if( bag.Has("pathguiding_sampling_type") ) {
						const std::string st = bag.GetString("pathguiding_sampling_type");
						guidingConfig.samplingType = ( st == "ris" || st == "RIS" ) ? eGuidingRIS : eGuidingOneSampleMIS;
					}
					if( bag.Has("pathguiding_ris_candidates") )                          guidingConfig.risCandidates          = std::max( 2u, bag.GetUInt("pathguiding_ris_candidates") );
					if( bag.Has("pathguiding_complete_paths") )                          guidingConfig.completePathGuiding    = bag.GetBool("pathguiding_complete_paths");
					if( bag.Has("pathguiding_complete_path_strategy_selection") )        guidingConfig.completePathStrategySelection = bag.GetBool("pathguiding_complete_path_strategy_selection");
					if( bag.Has("pathguiding_complete_path_strategy_samples") )          guidingConfig.completePathStrategySamples   = bag.GetUInt("pathguiding_complete_path_strategy_samples");

					AdaptiveSamplingConfig adaptiveConfig;
					if( bag.Has("adaptive_max_samples") ) adaptiveConfig.maxSamples = bag.GetUInt("adaptive_max_samples");
					if( bag.Has("adaptive_threshold") )   adaptiveConfig.threshold  = bag.GetDouble("adaptive_threshold");
					if( bag.Has("show_adaptive_map") )    adaptiveConfig.showMap    = bag.GetBool("show_adaptive_map");

					StabilityConfig stabilityConfig;
					if( bag.Has("direct_clamp") )                    stabilityConfig.directClamp                  = bag.GetDouble("direct_clamp");
					if( bag.Has("indirect_clamp") )                  stabilityConfig.indirectClamp                = bag.GetDouble("indirect_clamp");
					if( bag.Has("filter_glossy") )                   stabilityConfig.filterGlossy                 = bag.GetDouble("filter_glossy");
					if( bag.Has("rr_min_depth") )                    stabilityConfig.rrMinDepth                   = bag.GetUInt("rr_min_depth");
					if( bag.Has("rr_threshold") )                    stabilityConfig.rrThreshold                  = bag.GetDouble("rr_threshold");
					if( bag.Has("max_diffuse_bounce") )              stabilityConfig.maxDiffuseBounce             = bag.GetUInt("max_diffuse_bounce");
					if( bag.Has("max_glossy_bounce") )               stabilityConfig.maxGlossyBounce              = bag.GetUInt("max_glossy_bounce");
					if( bag.Has("max_transmission_bounce") )         stabilityConfig.maxTransmissionBounce        = bag.GetUInt("max_transmission_bounce");
					if( bag.Has("max_translucent_bounce") )          stabilityConfig.maxTranslucentBounce         = bag.GetUInt("max_translucent_bounce");
					if( bag.Has("max_volume_bounce") )               stabilityConfig.maxVolumeBounce              = bag.GetUInt("max_volume_bounce");
					if( bag.Has("light_bvh") )                       stabilityConfig.useLightBVH                  = bag.GetBool("light_bvh");
					if( bag.Has("optimal_mis") )                     stabilityConfig.optimalMIS                   = bag.GetBool("optimal_mis");
					if( bag.Has("optimal_mis_training_iterations") ) stabilityConfig.optimalMISTrainingIterations = bag.GetUInt("optimal_mis_training_iterations");
					if( bag.Has("optimal_mis_tile_size") )           stabilityConfig.optimalMISTileSize           = bag.GetUInt("optimal_mis_tile_size");

					ProgressiveConfig progressiveConfig;
					if( bag.Has("progressive_rendering") )      progressiveConfig.enabled = bag.GetBool("progressive_rendering");
					if( bag.Has("progressive_samples_per_pass") ) {
						const unsigned int spp = bag.GetUInt("progressive_samples_per_pass");
						progressiveConfig.samplesPerPass = spp > 0 ? spp : 1;
					}

					return pJob.SetPixelBasedPelRasterizer( numSamples, numLumSamples,
						maxRecur, defaultshader.c_str(), radianceMapConfig,
						luminarySampler=="none"?0:luminarySampler.c_str(), luminarySamplerParam,
						pixelFilterConfig,
						showLuminaires, oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter, guidingConfig, adaptiveConfig, stabilityConfig, progressiveConfig );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						PixelPelDefaults dflt;
						StabilityConfig stabilityDflt;
						ChunkDescriptor cd;
						cd.keyword = "pixelpel_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "RGB pel-based unidirectional path-tracing integrator.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "defaultshader";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Shader}; p.description = "Default shader chain for hit points"; p.defaultValueHint = to_hint(dflt.defaultShader); }
						{ auto& p = P(); p.name = "max_recursion";         p.kind = ValueKind::UInt;      p.description = "Maximum ray recursion depth";   p.defaultValueHint = to_hint(dflt.maxRecursion); }
						{ auto& p = P(); p.name = "samples";               p.kind = ValueKind::UInt;      p.description = "Samples per pixel";              p.defaultValueHint = to_hint(dflt.numPixelSamples); }
						{ auto& p = P(); p.name = "lum_samples";           p.kind = ValueKind::UInt;      p.description = "Luminaire samples per hit";      p.defaultValueHint = to_hint(dflt.numLumSamples); }
						{ auto& p = P(); p.name = "luminary_sampler";      p.kind = ValueKind::String;    p.description = "Luminary sampling strategy";     p.defaultValueHint = to_hint(dflt.luminarySampler); }
						{ auto& p = P(); p.name = "luminary_sampler_param";p.kind = ValueKind::Double;    p.description = "Luminary sampler parameter";     p.defaultValueHint = to_hint(dflt.luminarySamplerParam); }
						{ auto& p = P(); p.name = "show_luminaires";       p.kind = ValueKind::Bool;      p.description = "Show direct-visible luminaires"; p.defaultValueHint = to_hint(dflt.showLuminaires); }
						{ auto& p = P(); p.name = "oidn_denoise";          p.kind = ValueKind::Bool;      p.description = "Enable OIDN denoiser";           p.defaultValueHint = to_hint(dflt.oidnDenoise); }
						{ auto& p = P(); p.name = "oidn_quality";          p.kind = ValueKind::Enum;      p.enumValues = {"auto","high","balanced","fast"}; p.description = "OIDN quality preset (auto picks from render-time / megapixels)"; p.defaultValueHint = to_hint(dflt.oidnQuality); }
						{ auto& p = P(); p.name = "oidn_device";           p.kind = ValueKind::Enum;      p.enumValues = {"auto","cpu","gpu"};              p.description = "OIDN device backend (auto = prefer GPU, fall back to CPU)";       p.defaultValueHint = to_hint(dflt.oidnDevice); }
					{ auto& p = P(); p.name = "oidn_prefilter";        p.kind = ValueKind::Enum;      p.enumValues = {"fast","accurate"};               p.description = "OIDN aux source mode (fast = retrace/first-hit, accurate = inline first-non-delta + prefilter)"; p.defaultValueHint = to_hint(dflt.oidnPrefilter); }
						{ auto& p = P(); p.name = "choose_one_light";      p.kind = ValueKind::Bool;      p.description = "Legacy — ignored (unified LightSampler always selects one light per NEE)"; p.defaultValueHint = ""; }
						AddPixelFilterParams( P );
						AddRadianceMapParams( P );
						AddPathGuidingParams( P );
						AddAdaptiveSamplingParams( P );
						AddStabilityConfigParams( P );
						{ auto& p = P(); p.name = "filter_glossy";                    p.kind = ValueKind::Double; p.description = "Glossy roughness floor (0 disables)";                     p.defaultValueHint = to_hint(stabilityDflt.filterGlossy); }
						AddOptimalMISParams( P );
						AddProgressiveParams( P );
						return cd;
					}();
					return d;
				}
			};

			struct PixelIntegratingSpectralRasterizerAsciiChunkParser : public IAsciiChunkParser
			{
				// Helper: read pairs of (key, vector) from a single-column file.
				static bool LoadSingleColumnFile( const std::string& filename, std::vector<double>& out ) {
					FILE* f = fopen( GlobalMediaPathLocator().Find(String(filename.c_str())).c_str(), "r" );
					if( !f ) {
						GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to open file `%s`", filename.c_str() );
						return false;
					}
					char lcbuf[MAX_CHARS_PER_LINE];
					while( fgets( lcbuf, sizeof( lcbuf ), f ) ) {
						const char* q = lcbuf;
						while( *q == ' ' || *q == '\t' || *q == '\r' || *q == '\n' ) ++q;
						if( *q == '\0' || *q == '#' ) continue;
						double v = 0.0;
						if( sscanf( lcbuf, "%lf", &v ) == 1 ) out.push_back( v );
					}
					fclose( f );
					return true;
				}

				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					PixelIntegratingSpectralDefaults dflt;
					std::string defaultshader   = bag.GetString( "defaultshader",  dflt.defaultShader );
					unsigned int maxRecur       = bag.GetUInt(   "max_recursion",  dflt.maxRecursion );
					unsigned int numSamples     = bag.GetUInt(   "samples",        dflt.numPixelSamples );
					unsigned int numLumSamples  = bag.GetUInt(   "lum_samples",    dflt.numLumSamples );
					std::string luminarySampler = bag.GetString( "luminary_sampler", dflt.luminarySampler );
					double luminarySamplerParam = bag.GetDouble( "luminary_sampler_param", dflt.luminarySamplerParam );
					bool showLuminaires         = bag.GetBool(   "show_luminaires", dflt.showLuminaires );
					bool oidnDenoise            = bag.GetBool(   "oidn_denoise",    dflt.oidnDenoise );
					OidnQuality oidnQuality     = ParseOidnQuality(   bag.GetString( "oidn_quality",   to_hint(dflt.oidnQuality) ) );
					OidnDevice  oidnDevice      = ParseOidnDevice(    bag.GetString( "oidn_device",    to_hint(dflt.oidnDevice) ) );
					OidnPrefilter oidnPrefilter = ParseOidnPrefilter( bag.GetString( "oidn_prefilter", to_hint(dflt.oidnPrefilter) ) );
					bool integrateRGB           = bag.GetBool(   "integrate_rgb",   dflt.integrateRGB );

					SpectralConfig spectralConfig;
					if( bag.Has("spectral_samples") ) spectralConfig.spectralSamples = bag.GetUInt("spectral_samples");
					if( bag.Has("num_wavelengths") )  spectralConfig.numWavelengths  = bag.GetUInt("num_wavelengths");
					if( bag.Has("nmbegin") )          spectralConfig.nmBegin         = bag.GetDouble("nmbegin");
					if( bag.Has("nmend") )            spectralConfig.nmEnd           = bag.GetDouble("nmend");
					if( bag.Has("hwss") )             spectralConfig.useHWSS         = bag.GetBool("hwss");

					RadianceMapConfig radianceMapConfig;
					if( bag.Has("radiance_map") )        radianceMapConfig.name         = String(bag.GetString("radiance_map").c_str());
					if( bag.Has("radiance_scale") )      radianceMapConfig.scale        = bag.GetDouble("radiance_scale");
					if( bag.Has("radiance_background") ) radianceMapConfig.isBackground = bag.GetBool("radiance_background");
					if( bag.Has("radiance_orient") ) {
						bag.GetVec3( "radiance_orient", radianceMapConfig.orientation );
						radianceMapConfig.orientation[0] *= DEG_TO_RAD;
						radianceMapConfig.orientation[1] *= DEG_TO_RAD;
						radianceMapConfig.orientation[2] *= DEG_TO_RAD;
					}

					PixelFilterConfig pixelFilterConfig;
					if( bag.Has("blue_noise_sampler") )  pixelFilterConfig.blueNoiseSampler = bag.GetBool("blue_noise_sampler");
					if( bag.Has("pixel_sampler") )       pixelFilterConfig.pixelSampler     = String(bag.GetString("pixel_sampler").c_str());
					if( bag.Has("pixel_sampler_param") ) pixelFilterConfig.pixelSamplerParam= bag.GetDouble("pixel_sampler_param");
					if( bag.Has("pixel_filter") )        pixelFilterConfig.filter           = String(bag.GetString("pixel_filter").c_str());
					if( bag.Has("pixel_filter_width") )  pixelFilterConfig.width            = bag.GetDouble("pixel_filter_width");
					if( bag.Has("pixel_filter_height") ) pixelFilterConfig.height           = bag.GetDouble("pixel_filter_height");
					if( bag.Has("pixel_filter_paramA") ) pixelFilterConfig.paramA           = bag.GetDouble("pixel_filter_paramA");
					if( bag.Has("pixel_filter_paramB") ) pixelFilterConfig.paramB           = bag.GetDouble("pixel_filter_paramB");

					std::vector<double> spd_wavelengths;
					std::vector<double> spd_r;
					std::vector<double> spd_g;
					std::vector<double> spd_b;

					// rgb_spd loads wavelength + R + G + B together from a 4-column file.
					if( bag.Has("rgb_spd") ) {
						const std::string filename = bag.GetString("rgb_spd");
						FILE* f = fopen( GlobalMediaPathLocator().Find(String(filename.c_str())).c_str(), "r" );
						if( f ) {
							char rgbbuf[MAX_CHARS_PER_LINE];
							while( fgets( rgbbuf, sizeof( rgbbuf ), f ) ) {
								const char* q = rgbbuf;
								while( *q == ' ' || *q == '\t' || *q == '\r' || *q == '\n' ) ++q;
								if( *q == '\0' || *q == '#' ) continue;
								double nm = 0.0, r = 0.0, g = 0.0, b = 0.0;
								if( sscanf( rgbbuf, "%lf %lf %lf %lf", &nm, &r, &g, &b ) == 4 ) {
									spd_wavelengths.push_back( nm );
									spd_r.push_back( r );
									spd_g.push_back( g );
									spd_b.push_back( b );
								}
							}
							fclose( f );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to open file `%s`", filename.c_str() );
							return false;
						}
					}
					if( bag.Has("rgb_spd_wavelengths") ) {
						if( !LoadSingleColumnFile( bag.GetString("rgb_spd_wavelengths"), spd_wavelengths ) ) return false;
					}
					if( bag.Has("rgb_spd_r") ) {
						if( !LoadSingleColumnFile( bag.GetString("rgb_spd_r"), spd_r ) ) return false;
					}
					if( bag.Has("rgb_spd_g") ) {
						if( !LoadSingleColumnFile( bag.GetString("rgb_spd_g"), spd_g ) ) return false;
					}
					if( bag.Has("rgb_spd_b") ) {
						if( !LoadSingleColumnFile( bag.GetString("rgb_spd_b"), spd_b ) ) return false;
					}

					StabilityConfig stabilityConfig;
					if( bag.Has("direct_clamp") )            stabilityConfig.directClamp           = bag.GetDouble("direct_clamp");
					if( bag.Has("indirect_clamp") )          stabilityConfig.indirectClamp         = bag.GetDouble("indirect_clamp");
					if( bag.Has("filter_glossy") )           stabilityConfig.filterGlossy          = bag.GetDouble("filter_glossy");
					if( bag.Has("rr_min_depth") )            stabilityConfig.rrMinDepth            = bag.GetUInt("rr_min_depth");
					if( bag.Has("rr_threshold") )            stabilityConfig.rrThreshold           = bag.GetDouble("rr_threshold");
					if( bag.Has("max_diffuse_bounce") )      stabilityConfig.maxDiffuseBounce      = bag.GetUInt("max_diffuse_bounce");
					if( bag.Has("max_glossy_bounce") )       stabilityConfig.maxGlossyBounce       = bag.GetUInt("max_glossy_bounce");
					if( bag.Has("max_transmission_bounce") ) stabilityConfig.maxTransmissionBounce = bag.GetUInt("max_transmission_bounce");
					if( bag.Has("max_translucent_bounce") )  stabilityConfig.maxTranslucentBounce  = bag.GetUInt("max_translucent_bounce");
					if( bag.Has("max_volume_bounce") )       stabilityConfig.maxVolumeBounce       = bag.GetUInt("max_volume_bounce");
					if( bag.Has("light_bvh") )               stabilityConfig.useLightBVH           = bag.GetBool("light_bvh");

					return pJob.SetPixelBasedSpectralIntegratingRasterizer( numSamples, numLumSamples, spectralConfig, maxRecur, defaultshader.c_str(), radianceMapConfig,
						luminarySampler=="none"?0:luminarySampler.c_str(), luminarySamplerParam,
						pixelFilterConfig,
						showLuminaires,
						integrateRGB, static_cast<unsigned int>(spd_wavelengths.size()), integrateRGB?&spd_wavelengths[0]:0, integrateRGB?&spd_r[0]:0, integrateRGB?&spd_g[0]:0, integrateRGB?&spd_b[0]:0,
						oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter, stabilityConfig
						);
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						PixelIntegratingSpectralDefaults dflt;
						StabilityConfig stabilityDflt;
						ChunkDescriptor cd;
						cd.keyword = "pixelintegratingspectral_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "Spectral pel-based path-tracing integrator.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddBaseRasterizerParams( P, dflt );
						{ auto& p = P(); p.name = "max_recursion";   p.kind = ValueKind::UInt; p.description = "Maximum ray recursion depth"; p.defaultValueHint = to_hint(dflt.maxRecursion); }
						{ auto& p = P(); p.name = "lum_samples";     p.kind = ValueKind::UInt; p.description = "Luminaire samples per hit";   p.defaultValueHint = to_hint(dflt.numLumSamples); }
						{ auto& p = P(); p.name = "luminary_sampler";p.kind = ValueKind::String; p.description = "Luminary sampling strategy"; p.defaultValueHint = to_hint(dflt.luminarySampler); }
						{ auto& p = P(); p.name = "luminary_sampler_param"; p.kind = ValueKind::Double; p.description = "Luminary sampler parameter"; p.defaultValueHint = to_hint(dflt.luminarySamplerParam); }
						{ auto& p = P(); p.name = "choose_one_light";p.kind = ValueKind::Bool;   p.description = "Legacy — ignored (unified LightSampler always selects one light per NEE)"; p.defaultValueHint = ""; }
						AddPixelFilterParams( P );
						AddRadianceMapParams( P );
						AddSpectralConfigParams( P );
						AddStabilityConfigParams( P );
						{ auto& p = P(); p.name = "filter_glossy"; p.kind = ValueKind::Double; p.description = "Glossy roughness floor (0 disables)"; p.defaultValueHint = to_hint(stabilityDflt.filterGlossy); }
						return cd;
					}();
					return d;
				}
			};

			struct BDPTPelRasterizerAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					BDPTPelDefaults dflt;
					std::string defaultshader   = bag.GetString( "defaultshader",  dflt.defaultShader );
					unsigned int numSamples     = bag.GetUInt(   "samples",        dflt.numPixelSamples );
					unsigned int maxEyeDepth    = bag.GetUInt(   "max_eye_depth",  dflt.maxEyeDepth );
					unsigned int maxLightDepth  = bag.GetUInt(   "max_light_depth",dflt.maxLightDepth );
					bool showLuminaires         = bag.GetBool(   "show_luminaires", dflt.showLuminaires );
					bool oidnDenoise            = bag.GetBool(   "oidn_denoise",    dflt.oidnDenoise );
					OidnQuality oidnQuality     = ParseOidnQuality(   bag.GetString( "oidn_quality",   to_hint(dflt.oidnQuality) ) );
					OidnDevice  oidnDevice      = ParseOidnDevice(    bag.GetString( "oidn_device",    to_hint(dflt.oidnDevice) ) );
					OidnPrefilter oidnPrefilter = ParseOidnPrefilter( bag.GetString( "oidn_prefilter", to_hint(dflt.oidnPrefilter) ) );

					RadianceMapConfig radianceMapConfig;
					if( bag.Has("radiance_map") )        radianceMapConfig.name         = String(bag.GetString("radiance_map").c_str());
					if( bag.Has("radiance_scale") )      radianceMapConfig.scale        = bag.GetDouble("radiance_scale");
					if( bag.Has("radiance_background") ) radianceMapConfig.isBackground = bag.GetBool("radiance_background");
					if( bag.Has("radiance_orient") ) {
						bag.GetVec3( "radiance_orient", radianceMapConfig.orientation );
						radianceMapConfig.orientation[0] *= DEG_TO_RAD;
						radianceMapConfig.orientation[1] *= DEG_TO_RAD;
						radianceMapConfig.orientation[2] *= DEG_TO_RAD;
					}

					PixelFilterConfig pixelFilterConfig;
					if( bag.Has("blue_noise_sampler") )  pixelFilterConfig.blueNoiseSampler = bag.GetBool("blue_noise_sampler");
					if( bag.Has("pixel_sampler") )       pixelFilterConfig.pixelSampler     = String(bag.GetString("pixel_sampler").c_str());
					if( bag.Has("pixel_sampler_param") ) pixelFilterConfig.pixelSamplerParam= bag.GetDouble("pixel_sampler_param");
					if( bag.Has("pixel_filter") )        pixelFilterConfig.filter           = String(bag.GetString("pixel_filter").c_str());
					if( bag.Has("pixel_filter_width") )  pixelFilterConfig.width            = bag.GetDouble("pixel_filter_width");
					if( bag.Has("pixel_filter_height") ) pixelFilterConfig.height           = bag.GetDouble("pixel_filter_height");
					if( bag.Has("pixel_filter_paramA") ) pixelFilterConfig.paramA           = bag.GetDouble("pixel_filter_paramA");
					if( bag.Has("pixel_filter_paramB") ) pixelFilterConfig.paramB           = bag.GetDouble("pixel_filter_paramB");

					PathGuidingConfig guidingConfig;
					if( bag.Has("pathguiding") )                                    guidingConfig.enabled              = bag.GetBool("pathguiding");
					if( bag.Has("pathguiding_iterations") )                         guidingConfig.trainingIterations   = bag.GetUInt("pathguiding_iterations");
					if( bag.Has("pathguiding_spp") )                                guidingConfig.trainingSPP          = bag.GetUInt("pathguiding_spp");
					if( bag.Has("pathguiding_combine_training") )                   guidingConfig.combineTrainingIterations = bag.GetBool("pathguiding_combine_training");
					if( bag.Has("pathguiding_online") )                             guidingConfig.online               = bag.GetBool("pathguiding_online");
					if( bag.Has("pathguiding_warmup_iterations") )                  guidingConfig.warmupIterations     = bag.GetUInt("pathguiding_warmup_iterations");
					if( bag.Has("pathguiding_alpha") )                              guidingConfig.alpha                = bag.GetDouble("pathguiding_alpha");
					if( bag.Has("pathguiding_learned_alpha") )                      guidingConfig.learnedAlpha         = bag.GetBool("pathguiding_learned_alpha");
					if( bag.Has("pathguiding_max_depth") )                          guidingConfig.maxGuidingDepth      = bag.GetUInt("pathguiding_max_depth");
					if( bag.Has("pathguiding_light_max_depth") )                    guidingConfig.maxLightGuidingDepth = bag.GetUInt("pathguiding_light_max_depth");
					if( bag.Has("pathguiding_sampling_type") ) {
						const std::string st = bag.GetString("pathguiding_sampling_type");
						guidingConfig.samplingType = ( st == "ris" || st == "RIS" ) ? eGuidingRIS : eGuidingOneSampleMIS;
					}
					if( bag.Has("pathguiding_ris_candidates") )                     guidingConfig.risCandidates                 = std::max( 2u, bag.GetUInt("pathguiding_ris_candidates") );
					if( bag.Has("pathguiding_complete_paths") )                     guidingConfig.completePathGuiding           = bag.GetBool("pathguiding_complete_paths");
					if( bag.Has("pathguiding_complete_path_strategy_selection") )   guidingConfig.completePathStrategySelection = bag.GetBool("pathguiding_complete_path_strategy_selection");
					if( bag.Has("pathguiding_complete_path_strategy_samples") )     guidingConfig.completePathStrategySamples   = bag.GetUInt("pathguiding_complete_path_strategy_samples");

					AdaptiveSamplingConfig adaptiveConfig;
					if( bag.Has("adaptive_max_samples") ) adaptiveConfig.maxSamples = bag.GetUInt("adaptive_max_samples");
					if( bag.Has("adaptive_threshold") )   adaptiveConfig.threshold  = bag.GetDouble("adaptive_threshold");
					if( bag.Has("show_adaptive_map") )    adaptiveConfig.showMap    = bag.GetBool("show_adaptive_map");

					StabilityConfig stabilityConfig;
					if( bag.Has("direct_clamp") )                    stabilityConfig.directClamp                  = bag.GetDouble("direct_clamp");
					if( bag.Has("indirect_clamp") )                  stabilityConfig.indirectClamp                = bag.GetDouble("indirect_clamp");
					if( bag.Has("rr_min_depth") )                    stabilityConfig.rrMinDepth                   = bag.GetUInt("rr_min_depth");
					if( bag.Has("rr_threshold") )                    stabilityConfig.rrThreshold                  = bag.GetDouble("rr_threshold");
					if( bag.Has("max_diffuse_bounce") )              stabilityConfig.maxDiffuseBounce             = bag.GetUInt("max_diffuse_bounce");
					if( bag.Has("max_glossy_bounce") )               stabilityConfig.maxGlossyBounce              = bag.GetUInt("max_glossy_bounce");
					if( bag.Has("max_transmission_bounce") )         stabilityConfig.maxTransmissionBounce        = bag.GetUInt("max_transmission_bounce");
					if( bag.Has("max_translucent_bounce") )          stabilityConfig.maxTranslucentBounce         = bag.GetUInt("max_translucent_bounce");
					if( bag.Has("max_volume_bounce") )               stabilityConfig.maxVolumeBounce              = bag.GetUInt("max_volume_bounce");
					if( bag.Has("light_bvh") )                       stabilityConfig.useLightBVH                  = bag.GetBool("light_bvh");
					// Optimal MIS is intentionally NOT parsed here:
					// `BDPTIntegrator` does not consume `rc.pOptimalMIS`
					// (the Kondapaneni 2019 single-step formulation has
					// not been extended to BDPT's per-strategy-pair MIS).
					// The descriptor below also omits `AddOptimalMISParams`,
					// so the parser hard-fails on `optimal_mis*` lines
					// rather than silently dropping them.  See
					// docs/SPECTRAL_PARITY_AUDIT.md §2.10.

					ProgressiveConfig progressiveConfig;
					if( bag.Has("progressive_rendering") )      progressiveConfig.enabled = bag.GetBool("progressive_rendering");
					if( bag.Has("progressive_samples_per_pass") ) {
						const unsigned int spp = bag.GetUInt("progressive_samples_per_pass");
						progressiveConfig.samplesPerPass = spp > 0 ? spp : 1;
					}

					return pJob.SetBDPTPelRasterizer( numSamples,
						maxEyeDepth, maxLightDepth,
						defaultshader.c_str(), radianceMapConfig,
						pixelFilterConfig,
						showLuminaires,
						oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter, guidingConfig, adaptiveConfig, stabilityConfig, progressiveConfig );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						BDPTPelDefaults dflt;
						ChunkDescriptor cd;
						cd.keyword = "bdpt_pel_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "RGB bidirectional path-tracing integrator.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddBaseRasterizerParams( P, dflt );
						{ auto& p = P(); p.name = "max_eye_depth";   p.kind = ValueKind::UInt; p.description = "Max eye subpath depth";   p.defaultValueHint = to_hint(dflt.maxEyeDepth); }
						{ auto& p = P(); p.name = "max_light_depth"; p.kind = ValueKind::UInt; p.description = "Max light subpath depth"; p.defaultValueHint = to_hint(dflt.maxLightDepth); }
						{ auto& p = P(); p.name = "choose_one_light";p.kind = ValueKind::Bool; p.description = "Legacy — ignored (unified LightSampler always selects one light per NEE)"; p.defaultValueHint = ""; }
						AddPixelFilterParams( P );
						AddRadianceMapParams( P );
						AddPathGuidingParams( P );
						AddAdaptiveSamplingParams( P );
						AddStabilityConfigParams( P );
						// Intentionally NO AddOptimalMISParams: BDPTIntegrator
						// does not consume rc.pOptimalMIS — see Finalize note
						// and SPECTRAL_PARITY_AUDIT.md §2.10.
						AddProgressiveParams( P );
						return cd;
					}();
					return d;
				}
			};

			struct BDPTSpectralRasterizerAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					BDPTSpectralDefaults dflt;
					std::string defaultshader   = bag.GetString( "defaultshader",  dflt.defaultShader );
					unsigned int numSamples     = bag.GetUInt(   "samples",        dflt.numPixelSamples );
					unsigned int maxEyeDepth    = bag.GetUInt(   "max_eye_depth",  dflt.maxEyeDepth );
					unsigned int maxLightDepth  = bag.GetUInt(   "max_light_depth",dflt.maxLightDepth );
					bool showLuminaires         = bag.GetBool(   "show_luminaires", dflt.showLuminaires );
					bool oidnDenoise            = bag.GetBool(   "oidn_denoise",    dflt.oidnDenoise );
					OidnQuality oidnQuality     = ParseOidnQuality(   bag.GetString( "oidn_quality",   to_hint(dflt.oidnQuality) ) );
					OidnDevice  oidnDevice      = ParseOidnDevice(    bag.GetString( "oidn_device",    to_hint(dflt.oidnDevice) ) );
					OidnPrefilter oidnPrefilter = ParseOidnPrefilter( bag.GetString( "oidn_prefilter", to_hint(dflt.oidnPrefilter) ) );

					RadianceMapConfig radianceMapConfig;
					if( bag.Has("radiance_map") )        radianceMapConfig.name         = String(bag.GetString("radiance_map").c_str());
					if( bag.Has("radiance_scale") )      radianceMapConfig.scale        = bag.GetDouble("radiance_scale");
					if( bag.Has("radiance_background") ) radianceMapConfig.isBackground = bag.GetBool("radiance_background");
					if( bag.Has("radiance_orient") ) {
						bag.GetVec3( "radiance_orient", radianceMapConfig.orientation );
						radianceMapConfig.orientation[0] *= DEG_TO_RAD;
						radianceMapConfig.orientation[1] *= DEG_TO_RAD;
						radianceMapConfig.orientation[2] *= DEG_TO_RAD;
					}

					PixelFilterConfig pixelFilterConfig;
					if( bag.Has("blue_noise_sampler") )  pixelFilterConfig.blueNoiseSampler = bag.GetBool("blue_noise_sampler");
					if( bag.Has("pixel_sampler") )       pixelFilterConfig.pixelSampler     = String(bag.GetString("pixel_sampler").c_str());
					if( bag.Has("pixel_sampler_param") ) pixelFilterConfig.pixelSamplerParam= bag.GetDouble("pixel_sampler_param");
					if( bag.Has("pixel_filter") )        pixelFilterConfig.filter           = String(bag.GetString("pixel_filter").c_str());
					if( bag.Has("pixel_filter_width") )  pixelFilterConfig.width            = bag.GetDouble("pixel_filter_width");
					if( bag.Has("pixel_filter_height") ) pixelFilterConfig.height           = bag.GetDouble("pixel_filter_height");
					if( bag.Has("pixel_filter_paramA") ) pixelFilterConfig.paramA           = bag.GetDouble("pixel_filter_paramA");
					if( bag.Has("pixel_filter_paramB") ) pixelFilterConfig.paramB           = bag.GetDouble("pixel_filter_paramB");

					SpectralConfig spectralConfig;
					if( bag.Has("spectral_samples") ) spectralConfig.spectralSamples = bag.GetUInt("spectral_samples");
					if( bag.Has("num_wavelengths") )  spectralConfig.numWavelengths  = bag.GetUInt("num_wavelengths");
					if( bag.Has("nmbegin") )          spectralConfig.nmBegin         = bag.GetDouble("nmbegin");
					if( bag.Has("nmend") )            spectralConfig.nmEnd           = bag.GetDouble("nmend");
					if( bag.Has("hwss") )             spectralConfig.useHWSS         = bag.GetBool("hwss");

					PathGuidingConfig guidingConfig;
					if( bag.Has("pathguiding") )                                    guidingConfig.enabled                       = bag.GetBool("pathguiding");
					if( bag.Has("pathguiding_iterations") )                         guidingConfig.trainingIterations            = bag.GetUInt("pathguiding_iterations");
					if( bag.Has("pathguiding_spp") )                                guidingConfig.trainingSPP                   = bag.GetUInt("pathguiding_spp");
					if( bag.Has("pathguiding_combine_training") )                   guidingConfig.combineTrainingIterations     = bag.GetBool("pathguiding_combine_training");
					if( bag.Has("pathguiding_online") )                             guidingConfig.online                        = bag.GetBool("pathguiding_online");
					if( bag.Has("pathguiding_warmup_iterations") )                  guidingConfig.warmupIterations              = bag.GetUInt("pathguiding_warmup_iterations");
					if( bag.Has("pathguiding_alpha") )                              guidingConfig.alpha                         = bag.GetDouble("pathguiding_alpha");
					if( bag.Has("pathguiding_learned_alpha") )                      guidingConfig.learnedAlpha                  = bag.GetBool("pathguiding_learned_alpha");
					if( bag.Has("pathguiding_max_depth") )                          guidingConfig.maxGuidingDepth               = bag.GetUInt("pathguiding_max_depth");
					if( bag.Has("pathguiding_light_max_depth") )                    guidingConfig.maxLightGuidingDepth          = bag.GetUInt("pathguiding_light_max_depth");
					if( bag.Has("pathguiding_sampling_type") ) {
						const std::string st = bag.GetString("pathguiding_sampling_type");
						guidingConfig.samplingType = ( st == "ris" || st == "RIS" ) ? eGuidingRIS : eGuidingOneSampleMIS;
					}
					if( bag.Has("pathguiding_ris_candidates") )                     guidingConfig.risCandidates                 = std::max( 2u, bag.GetUInt("pathguiding_ris_candidates") );
					if( bag.Has("pathguiding_complete_paths") )                     guidingConfig.completePathGuiding           = bag.GetBool("pathguiding_complete_paths");
					if( bag.Has("pathguiding_complete_path_strategy_selection") )   guidingConfig.completePathStrategySelection = bag.GetBool("pathguiding_complete_path_strategy_selection");
					if( bag.Has("pathguiding_complete_path_strategy_samples") )     guidingConfig.completePathStrategySamples   = bag.GetUInt("pathguiding_complete_path_strategy_samples");

					// Adaptive sampling wired here 2026-05-24 (audit §2.8 / §6.1
					// last remaining quick win).  Welford convergence is driven
					// by the XYZ.Y luminance signal — see BDPTSpectralRasterizer
					// ::IntegratePixel for the mirrored Pel-side pattern.
					AdaptiveSamplingConfig adaptiveConfig;
					if( bag.Has("adaptive_max_samples") ) adaptiveConfig.maxSamples = bag.GetUInt("adaptive_max_samples");
					if( bag.Has("adaptive_threshold") )   adaptiveConfig.threshold  = bag.GetDouble("adaptive_threshold");
					if( bag.Has("show_adaptive_map") )    adaptiveConfig.showMap    = bag.GetBool("show_adaptive_map");

					StabilityConfig stabilityConfig;
					if( bag.Has("direct_clamp") )                    stabilityConfig.directClamp                  = bag.GetDouble("direct_clamp");
					if( bag.Has("indirect_clamp") )                  stabilityConfig.indirectClamp                = bag.GetDouble("indirect_clamp");
					if( bag.Has("rr_min_depth") )                    stabilityConfig.rrMinDepth                   = bag.GetUInt("rr_min_depth");
					if( bag.Has("rr_threshold") )                    stabilityConfig.rrThreshold                  = bag.GetDouble("rr_threshold");
					if( bag.Has("max_diffuse_bounce") )              stabilityConfig.maxDiffuseBounce             = bag.GetUInt("max_diffuse_bounce");
					if( bag.Has("max_glossy_bounce") )               stabilityConfig.maxGlossyBounce              = bag.GetUInt("max_glossy_bounce");
					if( bag.Has("max_transmission_bounce") )         stabilityConfig.maxTransmissionBounce        = bag.GetUInt("max_transmission_bounce");
					if( bag.Has("max_translucent_bounce") )          stabilityConfig.maxTranslucentBounce         = bag.GetUInt("max_translucent_bounce");
					if( bag.Has("max_volume_bounce") )               stabilityConfig.maxVolumeBounce              = bag.GetUInt("max_volume_bounce");
					if( bag.Has("light_bvh") )                       stabilityConfig.useLightBVH                  = bag.GetBool("light_bvh");
					// Optimal MIS not parsed: BDPTIntegrator does not
					// consume rc.pOptimalMIS, and the spectral parent
					// rasterizer does not allocate the accumulator.
					// Descriptor below also omits AddOptimalMISParams.
					// See docs/SPECTRAL_PARITY_AUDIT.md §2.10.

					ProgressiveConfig progressiveConfig;
					if( bag.Has("progressive_rendering") )      progressiveConfig.enabled = bag.GetBool("progressive_rendering");
					if( bag.Has("progressive_samples_per_pass") ) {
						const unsigned int spp = bag.GetUInt("progressive_samples_per_pass");
						progressiveConfig.samplesPerPass = spp > 0 ? spp : 1;
					}

					return pJob.SetBDPTSpectralRasterizer( numSamples,
						maxEyeDepth, maxLightDepth,
						defaultshader.c_str(), radianceMapConfig,
						pixelFilterConfig,
						showLuminaires,
						spectralConfig,
						oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter, guidingConfig, adaptiveConfig, stabilityConfig, progressiveConfig );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						BDPTSpectralDefaults dflt;
						ChunkDescriptor cd;
						cd.keyword = "bdpt_spectral_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "Spectral bidirectional path-tracing integrator.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddBaseRasterizerParams( P, dflt );
						{ auto& p = P(); p.name = "max_eye_depth";   p.kind = ValueKind::UInt; p.description = "Max eye subpath depth";   p.defaultValueHint = to_hint(dflt.maxEyeDepth); }
						{ auto& p = P(); p.name = "max_light_depth"; p.kind = ValueKind::UInt; p.description = "Max light subpath depth"; p.defaultValueHint = to_hint(dflt.maxLightDepth); }
						{ auto& p = P(); p.name = "choose_one_light";p.kind = ValueKind::Bool; p.description = "Legacy — ignored (unified LightSampler always selects one light per NEE)"; p.defaultValueHint = ""; }
						AddPixelFilterParams( P );
						AddRadianceMapParams( P );
						// BDPT spectral consumes only the core spectral
						// fields; RGB-to-SPD conversion is done in the
						// painters pipeline.
						AddSpectralCoreParams( P );
						// Full path-guiding param set.  The shared
						// BDPTRasterizerBase honors every field including
						// light-subpath guiding and complete-path strategy.
						// Wired here 2026-05-07 (audit §2.7) — previously
						// the descriptor / Finalize hand-rolled a subset
						// that silently dropped 3 fields.
						AddPathGuidingParams( P );
						// Adaptive sampling wired 2026-05-24 (audit §2.8 /
						// §6.1 last remaining quick win).
						AddAdaptiveSamplingParams( P );
						AddStabilityConfigParams( P );
						// Intentionally NO AddOptimalMISParams: see Finalize
						// note and SPECTRAL_PARITY_AUDIT.md §2.10.
						AddProgressiveParams( P );
						return cd;
					}();
					return d;
				}
			};

			struct VCMPelRasterizerAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					VCMPelDefaults dflt;
					std::string defaultshader   = bag.GetString( "defaultshader",  dflt.defaultShader );
					unsigned int numSamples     = bag.GetUInt(   "samples",        dflt.numPixelSamples );
					unsigned int maxEyeDepth    = bag.GetUInt(   "max_eye_depth",  dflt.maxEyeDepth );
					unsigned int maxLightDepth  = bag.GetUInt(   "max_light_depth",dflt.maxLightDepth );
					bool showLuminaires         = bag.GetBool(   "show_luminaires", dflt.showLuminaires );
					bool oidnDenoise            = bag.GetBool(   "oidn_denoise",    dflt.oidnDenoise );
					OidnQuality oidnQuality     = ParseOidnQuality(   bag.GetString( "oidn_quality",   to_hint(dflt.oidnQuality) ) );
					OidnDevice  oidnDevice      = ParseOidnDevice(    bag.GetString( "oidn_device",    to_hint(dflt.oidnDevice) ) );
					OidnPrefilter oidnPrefilter = ParseOidnPrefilter( bag.GetString( "oidn_prefilter", to_hint(dflt.oidnPrefilter) ) );
					double mergeRadius          = bag.GetDouble( "merge_radius",    dflt.mergeRadius );
					bool enableVC               = bag.GetBool(   "vc_enabled",      dflt.enableVC );
					bool enableVM               = bag.GetBool(   "vm_enabled",      dflt.enableVM );

					RadianceMapConfig radianceMapConfig;
					if( bag.Has("radiance_map") )        radianceMapConfig.name         = String(bag.GetString("radiance_map").c_str());
					if( bag.Has("radiance_scale") )      radianceMapConfig.scale        = bag.GetDouble("radiance_scale");
					if( bag.Has("radiance_background") ) radianceMapConfig.isBackground = bag.GetBool("radiance_background");
					if( bag.Has("radiance_orient") ) {
						bag.GetVec3( "radiance_orient", radianceMapConfig.orientation );
						radianceMapConfig.orientation[0] *= DEG_TO_RAD;
						radianceMapConfig.orientation[1] *= DEG_TO_RAD;
						radianceMapConfig.orientation[2] *= DEG_TO_RAD;
					}

					PixelFilterConfig pixelFilterConfig;
					if( bag.Has("blue_noise_sampler") )  pixelFilterConfig.blueNoiseSampler = bag.GetBool("blue_noise_sampler");
					if( bag.Has("pixel_sampler") )       pixelFilterConfig.pixelSampler     = String(bag.GetString("pixel_sampler").c_str());
					if( bag.Has("pixel_sampler_param") ) pixelFilterConfig.pixelSamplerParam= bag.GetDouble("pixel_sampler_param");
					if( bag.Has("pixel_filter") )        pixelFilterConfig.filter           = String(bag.GetString("pixel_filter").c_str());
					if( bag.Has("pixel_filter_width") )  pixelFilterConfig.width            = bag.GetDouble("pixel_filter_width");
					if( bag.Has("pixel_filter_height") ) pixelFilterConfig.height           = bag.GetDouble("pixel_filter_height");
					if( bag.Has("pixel_filter_paramA") ) pixelFilterConfig.paramA           = bag.GetDouble("pixel_filter_paramA");
					if( bag.Has("pixel_filter_paramB") ) pixelFilterConfig.paramB           = bag.GetDouble("pixel_filter_paramB");

					PathGuidingConfig guidingConfig;	// VCMPel does not consume pathguiding params

					AdaptiveSamplingConfig adaptiveConfig;
					if( bag.Has("adaptive_max_samples") ) adaptiveConfig.maxSamples = bag.GetUInt("adaptive_max_samples");
					if( bag.Has("adaptive_threshold") )   adaptiveConfig.threshold  = bag.GetDouble("adaptive_threshold");
					if( bag.Has("show_adaptive_map") )    adaptiveConfig.showMap    = bag.GetBool("show_adaptive_map");

					StabilityConfig stabilityConfig;
					if( bag.Has("direct_clamp") )            stabilityConfig.directClamp           = bag.GetDouble("direct_clamp");
					if( bag.Has("indirect_clamp") )          stabilityConfig.indirectClamp         = bag.GetDouble("indirect_clamp");
					if( bag.Has("rr_min_depth") )            stabilityConfig.rrMinDepth            = bag.GetUInt("rr_min_depth");
					if( bag.Has("rr_threshold") )            stabilityConfig.rrThreshold           = bag.GetDouble("rr_threshold");
					if( bag.Has("max_diffuse_bounce") )      stabilityConfig.maxDiffuseBounce      = bag.GetUInt("max_diffuse_bounce");
					if( bag.Has("max_glossy_bounce") )       stabilityConfig.maxGlossyBounce       = bag.GetUInt("max_glossy_bounce");
					if( bag.Has("max_transmission_bounce") ) stabilityConfig.maxTransmissionBounce = bag.GetUInt("max_transmission_bounce");
					if( bag.Has("max_translucent_bounce") )  stabilityConfig.maxTranslucentBounce  = bag.GetUInt("max_translucent_bounce");
					if( bag.Has("max_volume_bounce") )       stabilityConfig.maxVolumeBounce       = bag.GetUInt("max_volume_bounce");
					if( bag.Has("light_bvh") )               stabilityConfig.useLightBVH           = bag.GetBool("light_bvh");

					ProgressiveConfig progressiveConfig;
					if( bag.Has("progressive_rendering") )      progressiveConfig.enabled = bag.GetBool("progressive_rendering");
					if( bag.Has("progressive_samples_per_pass") ) {
						const unsigned int spp = bag.GetUInt("progressive_samples_per_pass");
						progressiveConfig.samplesPerPass = spp > 0 ? spp : 1;
					}

					return pJob.SetVCMPelRasterizer( numSamples,
						maxEyeDepth, maxLightDepth,
						defaultshader.c_str(), radianceMapConfig,
						pixelFilterConfig,
						showLuminaires,
						mergeRadius, enableVC, enableVM, oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter,
						guidingConfig, adaptiveConfig, stabilityConfig, progressiveConfig );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						VCMPelDefaults dflt;
						ChunkDescriptor cd;
						cd.keyword = "vcm_pel_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "RGB vertex-connection-and-merging integrator.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddBaseRasterizerParams( P, dflt );
						{ auto& p = P(); p.name = "max_eye_depth";   p.kind = ValueKind::UInt;   p.description = "Max eye subpath depth";         p.defaultValueHint = to_hint(dflt.maxEyeDepth); }
						{ auto& p = P(); p.name = "max_light_depth"; p.kind = ValueKind::UInt;   p.description = "Max light subpath depth";       p.defaultValueHint = to_hint(dflt.maxLightDepth); }
						{ auto& p = P(); p.name = "merge_radius";    p.kind = ValueKind::Double; p.description = "Photon merge radius (0=auto)"; p.defaultValueHint = to_hint(dflt.mergeRadius); }
						{ auto& p = P(); p.name = "vc_enabled";      p.kind = ValueKind::Bool;   p.description = "Enable vertex connection";      p.defaultValueHint = to_hint(dflt.enableVC); }
						{ auto& p = P(); p.name = "vm_enabled";      p.kind = ValueKind::Bool;   p.description = "Enable vertex merging";         p.defaultValueHint = to_hint(dflt.enableVM); }
						{ auto& p = P(); p.name = "choose_one_light";p.kind = ValueKind::Bool;   p.description = "Legacy — ignored (unified LightSampler always selects one light per NEE)"; p.defaultValueHint = ""; }
						AddPixelFilterParams( P );
						AddRadianceMapParams( P );
						AddAdaptiveSamplingParams( P );
						AddStabilityConfigParams( P );
						AddProgressiveParams( P );
						return cd;
					}();
					return d;
				}
			};

			struct VCMSpectralRasterizerAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					VCMSpectralDefaults dflt;
					std::string defaultshader   = bag.GetString( "defaultshader",  dflt.defaultShader );
					unsigned int numSamples     = bag.GetUInt(   "samples",        dflt.numPixelSamples );
					unsigned int maxEyeDepth    = bag.GetUInt(   "max_eye_depth",  dflt.maxEyeDepth );
					unsigned int maxLightDepth  = bag.GetUInt(   "max_light_depth",dflt.maxLightDepth );
					bool showLuminaires         = bag.GetBool(   "show_luminaires", dflt.showLuminaires );
					bool oidnDenoise            = bag.GetBool(   "oidn_denoise",    dflt.oidnDenoise );
					OidnQuality oidnQuality     = ParseOidnQuality(   bag.GetString( "oidn_quality",   to_hint(dflt.oidnQuality) ) );
					OidnDevice  oidnDevice      = ParseOidnDevice(    bag.GetString( "oidn_device",    to_hint(dflt.oidnDevice) ) );
					OidnPrefilter oidnPrefilter = ParseOidnPrefilter( bag.GetString( "oidn_prefilter", to_hint(dflt.oidnPrefilter) ) );
					double mergeRadius          = bag.GetDouble( "merge_radius",    dflt.mergeRadius );
					bool enableVC               = bag.GetBool(   "vc_enabled",      dflt.enableVC );
					bool enableVM               = bag.GetBool(   "vm_enabled",      dflt.enableVM );

					RadianceMapConfig radianceMapConfig;
					if( bag.Has("radiance_map") )        radianceMapConfig.name         = String(bag.GetString("radiance_map").c_str());
					if( bag.Has("radiance_scale") )      radianceMapConfig.scale        = bag.GetDouble("radiance_scale");
					if( bag.Has("radiance_background") ) radianceMapConfig.isBackground = bag.GetBool("radiance_background");
					if( bag.Has("radiance_orient") ) {
						bag.GetVec3( "radiance_orient", radianceMapConfig.orientation );
						radianceMapConfig.orientation[0] *= DEG_TO_RAD;
						radianceMapConfig.orientation[1] *= DEG_TO_RAD;
						radianceMapConfig.orientation[2] *= DEG_TO_RAD;
					}

					PixelFilterConfig pixelFilterConfig;
					if( bag.Has("blue_noise_sampler") )  pixelFilterConfig.blueNoiseSampler = bag.GetBool("blue_noise_sampler");
					if( bag.Has("pixel_sampler") )       pixelFilterConfig.pixelSampler     = String(bag.GetString("pixel_sampler").c_str());
					if( bag.Has("pixel_sampler_param") ) pixelFilterConfig.pixelSamplerParam= bag.GetDouble("pixel_sampler_param");
					if( bag.Has("pixel_filter") )        pixelFilterConfig.filter           = String(bag.GetString("pixel_filter").c_str());
					if( bag.Has("pixel_filter_width") )  pixelFilterConfig.width            = bag.GetDouble("pixel_filter_width");
					if( bag.Has("pixel_filter_height") ) pixelFilterConfig.height           = bag.GetDouble("pixel_filter_height");
					if( bag.Has("pixel_filter_paramA") ) pixelFilterConfig.paramA           = bag.GetDouble("pixel_filter_paramA");
					if( bag.Has("pixel_filter_paramB") ) pixelFilterConfig.paramB           = bag.GetDouble("pixel_filter_paramB");

					SpectralConfig spectralConfig;
					if( bag.Has("spectral_samples") ) spectralConfig.spectralSamples = bag.GetUInt("spectral_samples");
					if( bag.Has("num_wavelengths") )  spectralConfig.numWavelengths  = bag.GetUInt("num_wavelengths");
					if( bag.Has("nmbegin") )          spectralConfig.nmBegin         = bag.GetDouble("nmbegin");
					if( bag.Has("nmend") )            spectralConfig.nmEnd           = bag.GetDouble("nmend");
					if( bag.Has("hwss") )             spectralConfig.useHWSS         = bag.GetBool("hwss");

					PathGuidingConfig guidingConfig;	// VCMSpectral does not consume pathguiding params

					AdaptiveSamplingConfig adaptiveConfig;
					if( bag.Has("adaptive_max_samples") ) adaptiveConfig.maxSamples = bag.GetUInt("adaptive_max_samples");
					if( bag.Has("adaptive_threshold") )   adaptiveConfig.threshold  = bag.GetDouble("adaptive_threshold");
					if( bag.Has("show_adaptive_map") )    adaptiveConfig.showMap    = bag.GetBool("show_adaptive_map");

					StabilityConfig stabilityConfig;
					if( bag.Has("direct_clamp") )            stabilityConfig.directClamp           = bag.GetDouble("direct_clamp");
					if( bag.Has("indirect_clamp") )          stabilityConfig.indirectClamp         = bag.GetDouble("indirect_clamp");
					if( bag.Has("rr_min_depth") )            stabilityConfig.rrMinDepth            = bag.GetUInt("rr_min_depth");
					if( bag.Has("rr_threshold") )            stabilityConfig.rrThreshold           = bag.GetDouble("rr_threshold");
					if( bag.Has("max_diffuse_bounce") )      stabilityConfig.maxDiffuseBounce      = bag.GetUInt("max_diffuse_bounce");
					if( bag.Has("max_glossy_bounce") )       stabilityConfig.maxGlossyBounce       = bag.GetUInt("max_glossy_bounce");
					if( bag.Has("max_transmission_bounce") ) stabilityConfig.maxTransmissionBounce = bag.GetUInt("max_transmission_bounce");
					if( bag.Has("max_translucent_bounce") )  stabilityConfig.maxTranslucentBounce  = bag.GetUInt("max_translucent_bounce");
					if( bag.Has("max_volume_bounce") )       stabilityConfig.maxVolumeBounce       = bag.GetUInt("max_volume_bounce");
					if( bag.Has("light_bvh") )               stabilityConfig.useLightBVH           = bag.GetBool("light_bvh");

					ProgressiveConfig progressiveConfig;
					if( bag.Has("progressive_rendering") )      progressiveConfig.enabled = bag.GetBool("progressive_rendering");
					if( bag.Has("progressive_samples_per_pass") ) {
						const unsigned int spp = bag.GetUInt("progressive_samples_per_pass");
						progressiveConfig.samplesPerPass = spp > 0 ? spp : 1;
					}

					return pJob.SetVCMSpectralRasterizer( numSamples,
						maxEyeDepth, maxLightDepth,
						defaultshader.c_str(), radianceMapConfig,
						pixelFilterConfig,
						showLuminaires,
						spectralConfig,
						mergeRadius, enableVC, enableVM, oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter,
						guidingConfig, adaptiveConfig, stabilityConfig, progressiveConfig );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						VCMSpectralDefaults dflt;
						ChunkDescriptor cd;
						cd.keyword = "vcm_spectral_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "Spectral vertex-connection-and-merging integrator.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddBaseRasterizerParams( P, dflt );
						{ auto& p = P(); p.name = "max_eye_depth";   p.kind = ValueKind::UInt;   p.description = "Max eye subpath depth";         p.defaultValueHint = to_hint(dflt.maxEyeDepth); }
						{ auto& p = P(); p.name = "max_light_depth"; p.kind = ValueKind::UInt;   p.description = "Max light subpath depth";       p.defaultValueHint = to_hint(dflt.maxLightDepth); }
						{ auto& p = P(); p.name = "merge_radius";    p.kind = ValueKind::Double; p.description = "Photon merge radius (0=auto)"; p.defaultValueHint = to_hint(dflt.mergeRadius); }
						{ auto& p = P(); p.name = "vc_enabled";      p.kind = ValueKind::Bool;   p.description = "Enable vertex connection";      p.defaultValueHint = to_hint(dflt.enableVC); }
						{ auto& p = P(); p.name = "vm_enabled";      p.kind = ValueKind::Bool;   p.description = "Enable vertex merging";         p.defaultValueHint = to_hint(dflt.enableVM); }
						{ auto& p = P(); p.name = "choose_one_light";p.kind = ValueKind::Bool;   p.description = "Legacy — ignored (unified LightSampler always selects one light per NEE)"; p.defaultValueHint = ""; }
						AddPixelFilterParams( P );
						AddRadianceMapParams( P );
						// VCM spectral consumes only the core spectral
						// fields; RGB-to-SPD conversion is done in the
						// painters pipeline.
						AddSpectralCoreParams( P );
						AddAdaptiveSamplingParams( P );
						AddStabilityConfigParams( P );
						AddProgressiveParams( P );
						return cd;
					}();
					return d;
				}
			};

			struct AutoRasterizerAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					AutoRasterizerDefaults dflt;
					std::string defaultshader   = bag.GetString( "defaultshader",  dflt.defaultShader );
					unsigned int numSamples     = bag.GetUInt(   "samples",        dflt.numPixelSamples );
					bool showLuminaires         = bag.GetBool(   "show_luminaires", dflt.showLuminaires );
					bool probeEnabled           = bag.GetBool(   "probe",          dflt.probeEnabled );
					bool oidnDenoise            = bag.GetBool(   "oidn_denoise",    dflt.oidnDenoise );
					OidnQuality oidnQuality     = ParseOidnQuality(   bag.GetString( "oidn_quality",   to_hint(dflt.oidnQuality) ) );
					OidnDevice  oidnDevice      = ParseOidnDevice(    bag.GetString( "oidn_device",    to_hint(dflt.oidnDevice) ) );
					OidnPrefilter oidnPrefilter = ParseOidnPrefilter( bag.GetString( "oidn_prefilter", to_hint(dflt.oidnPrefilter) ) );

					// Integrator pin (Tier 0).  Unknown / omitted -> auto, which
					// the dispatcher resolves to PT in Phase 1.  Accept the quoted
					// form too (`integrator "vcm"`) for consistency with sms_seeding.
					AutoIntegratorChoice integrator = dflt.integrator;
					if( bag.Has("integrator") ) {
						std::string iv = StripSurroundingQuotes( bag.GetString("integrator") );
						std::transform( iv.begin(), iv.end(), iv.begin(),
							[]( unsigned char c ){ return std::tolower( c ); } );
						if( iv == "pt" || iv == "pathtracing" || iv == "path_tracing" ) integrator = AutoIntegratorChoice::PT;
						else if( iv == "bdpt" )                                         integrator = AutoIntegratorChoice::BDPT;
						else if( iv == "vcm" )                                          integrator = AutoIntegratorChoice::VCM;
						else if( iv == "auto" )                                         integrator = AutoIntegratorChoice::Auto;
						else {
							GlobalLog()->PrintEx( eLog_Warning,
								"Parser: unknown auto_rasterizer integrator \"%s\"; defaulting to auto", iv.c_str() );
							integrator = AutoIntegratorChoice::Auto;
						}
					}

					RadianceMapConfig radianceMapConfig;
					if( bag.Has("radiance_map") )        radianceMapConfig.name         = String(bag.GetString("radiance_map").c_str());
					if( bag.Has("radiance_scale") )      radianceMapConfig.scale        = bag.GetDouble("radiance_scale");
					if( bag.Has("radiance_background") ) radianceMapConfig.isBackground = bag.GetBool("radiance_background");
					if( bag.Has("radiance_orient") ) {
						bag.GetVec3( "radiance_orient", radianceMapConfig.orientation );
						radianceMapConfig.orientation[0] *= DEG_TO_RAD;
						radianceMapConfig.orientation[1] *= DEG_TO_RAD;
						radianceMapConfig.orientation[2] *= DEG_TO_RAD;
					}

					PixelFilterConfig pixelFilterConfig;
					if( bag.Has("blue_noise_sampler") )  pixelFilterConfig.blueNoiseSampler = bag.GetBool("blue_noise_sampler");
					if( bag.Has("pixel_sampler") )       pixelFilterConfig.pixelSampler     = String(bag.GetString("pixel_sampler").c_str());
					if( bag.Has("pixel_sampler_param") ) pixelFilterConfig.pixelSamplerParam= bag.GetDouble("pixel_sampler_param");
					if( bag.Has("pixel_filter") )        pixelFilterConfig.filter           = String(bag.GetString("pixel_filter").c_str());
					if( bag.Has("pixel_filter_width") )  pixelFilterConfig.width            = bag.GetDouble("pixel_filter_width");
					if( bag.Has("pixel_filter_height") ) pixelFilterConfig.height           = bag.GetDouble("pixel_filter_height");
					if( bag.Has("pixel_filter_paramA") ) pixelFilterConfig.paramA           = bag.GetDouble("pixel_filter_paramA");
					if( bag.Has("pixel_filter_paramB") ) pixelFilterConfig.paramB           = bag.GetDouble("pixel_filter_paramB");

					PathGuidingConfig guidingConfig;
					if( bag.Has("pathguiding") )                                    guidingConfig.enabled              = bag.GetBool("pathguiding");
					if( bag.Has("pathguiding_iterations") )                         guidingConfig.trainingIterations   = bag.GetUInt("pathguiding_iterations");
					if( bag.Has("pathguiding_spp") )                                guidingConfig.trainingSPP          = bag.GetUInt("pathguiding_spp");
					if( bag.Has("pathguiding_combine_training") )                   guidingConfig.combineTrainingIterations = bag.GetBool("pathguiding_combine_training");
					if( bag.Has("pathguiding_online") )                             guidingConfig.online               = bag.GetBool("pathguiding_online");
					if( bag.Has("pathguiding_warmup_iterations") )                  guidingConfig.warmupIterations     = bag.GetUInt("pathguiding_warmup_iterations");
					if( bag.Has("pathguiding_alpha") )                              guidingConfig.alpha                = bag.GetDouble("pathguiding_alpha");
					if( bag.Has("pathguiding_learned_alpha") )                      guidingConfig.learnedAlpha         = bag.GetBool("pathguiding_learned_alpha");
					if( bag.Has("pathguiding_max_depth") )                          guidingConfig.maxGuidingDepth      = bag.GetUInt("pathguiding_max_depth");
					if( bag.Has("pathguiding_light_max_depth") )                    guidingConfig.maxLightGuidingDepth = bag.GetUInt("pathguiding_light_max_depth");
					if( bag.Has("pathguiding_sampling_type") ) {
						const std::string st = bag.GetString("pathguiding_sampling_type");
						guidingConfig.samplingType = ( st == "ris" || st == "RIS" ) ? eGuidingRIS : eGuidingOneSampleMIS;
					}
					if( bag.Has("pathguiding_ris_candidates") )                     guidingConfig.risCandidates                 = std::max( 2u, bag.GetUInt("pathguiding_ris_candidates") );
					if( bag.Has("pathguiding_complete_paths") )                     guidingConfig.completePathGuiding           = bag.GetBool("pathguiding_complete_paths");
					if( bag.Has("pathguiding_complete_path_strategy_selection") )   guidingConfig.completePathStrategySelection = bag.GetBool("pathguiding_complete_path_strategy_selection");
					if( bag.Has("pathguiding_complete_path_strategy_samples") )     guidingConfig.completePathStrategySamples   = bag.GetUInt("pathguiding_complete_path_strategy_samples");

					AdaptiveSamplingConfig adaptiveConfig;
					if( bag.Has("adaptive_max_samples") ) adaptiveConfig.maxSamples = bag.GetUInt("adaptive_max_samples");
					if( bag.Has("adaptive_threshold") )   adaptiveConfig.threshold  = bag.GetDouble("adaptive_threshold");
					if( bag.Has("show_adaptive_map") )    adaptiveConfig.showMap    = bag.GetBool("show_adaptive_map");

					StabilityConfig stabilityConfig;
					if( bag.Has("direct_clamp") )                    stabilityConfig.directClamp                  = bag.GetDouble("direct_clamp");
					if( bag.Has("indirect_clamp") )                  stabilityConfig.indirectClamp                = bag.GetDouble("indirect_clamp");
					if( bag.Has("rr_min_depth") )                    stabilityConfig.rrMinDepth                   = bag.GetUInt("rr_min_depth");
					if( bag.Has("rr_threshold") )                    stabilityConfig.rrThreshold                  = bag.GetDouble("rr_threshold");
					if( bag.Has("max_diffuse_bounce") )              stabilityConfig.maxDiffuseBounce             = bag.GetUInt("max_diffuse_bounce");
					if( bag.Has("max_glossy_bounce") )               stabilityConfig.maxGlossyBounce              = bag.GetUInt("max_glossy_bounce");
					if( bag.Has("max_transmission_bounce") )         stabilityConfig.maxTransmissionBounce        = bag.GetUInt("max_transmission_bounce");
					if( bag.Has("max_translucent_bounce") )          stabilityConfig.maxTranslucentBounce         = bag.GetUInt("max_translucent_bounce");
					if( bag.Has("max_volume_bounce") )               stabilityConfig.maxVolumeBounce              = bag.GetUInt("max_volume_bounce");
					if( bag.Has("light_bvh") )                       stabilityConfig.useLightBVH                  = bag.GetBool("light_bvh");
					if( bag.Has("optimal_mis") )                     stabilityConfig.optimalMIS                   = bag.GetBool("optimal_mis");
					if( bag.Has("optimal_mis_training_iterations") ) stabilityConfig.optimalMISTrainingIterations = bag.GetUInt("optimal_mis_training_iterations");
					if( bag.Has("optimal_mis_tile_size") )           stabilityConfig.optimalMISTileSize           = bag.GetUInt("optimal_mis_tile_size");

					ProgressiveConfig progressiveConfig;
					if( bag.Has("progressive_rendering") )      progressiveConfig.enabled = bag.GetBool("progressive_rendering");
					if( bag.Has("progressive_samples_per_pass") ) {
						const unsigned int spp = bag.GetUInt("progressive_samples_per_pass");
						progressiveConfig.samplesPerPass = spp > 0 ? spp : 1;
					}

					return pJob.SetAutoRasterizer( integrator, numSamples,
						defaultshader.c_str(), radianceMapConfig,
						pixelFilterConfig,
						showLuminaires,
						oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter, guidingConfig, adaptiveConfig, stabilityConfig, progressiveConfig,
						probeEnabled );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						AutoRasterizerDefaults dflt;
						ChunkDescriptor cd;
						cd.keyword = "auto_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "Auto-routing integrator dispatcher: delegates to PT/BDPT/VCM (Phase 1: author pin via `integrator`, else PT).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddBaseRasterizerParams( P, dflt );
						{ auto& p = P(); p.name = "integrator"; p.kind = ValueKind::Enum; p.enumValues = {"auto","pt","bdpt","vcm"}; p.description = "Integrator selection: auto (dispatcher decides) or an explicit pin (pt/bdpt/vcm)"; p.defaultValueHint = to_hint(dflt.integrator); }
						{ auto& p = P(); p.name = "probe"; p.kind = ValueKind::Bool; p.description = "Enable the Tier-2 render-time probe (Phase 4): a cheap reduced-res/low-spp pre-render of candidate integrators that picks per-scene (caustic median-lum -> VCM; strong-indirect σ²·T -> BDPT; else PT).  Only fires once production `samples` clears the activation-spp gate (GlobalOptions auto_probe_activation_spp); below it the dispatcher uses the Tier-1 static best-guess.  Default off — the probe is a final-render tool."; p.defaultValueHint = dflt.probeEnabled ? "TRUE" : "FALSE"; }
						AddPixelFilterParams( P );
						AddRadianceMapParams( P );
						AddPathGuidingParams( P );
						AddAdaptiveSamplingParams( P );
						AddStabilityConfigParams( P );
						AddOptimalMISParams( P );
						AddProgressiveParams( P );
						return cd;
					}();
					return d;
				}
			};

				struct AutoSpectralRasterizerAsciiChunkParser : public IAsciiChunkParser
				{
					bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
					{
						AutoRasterizerDefaults dflt;
						std::string defaultshader   = bag.GetString( "defaultshader",  dflt.defaultShader );
						unsigned int numSamples     = bag.GetUInt(   "samples",        dflt.numPixelSamples );
						bool showLuminaires         = bag.GetBool(   "show_luminaires", dflt.showLuminaires );
						bool probeEnabled           = bag.GetBool(   "probe",          dflt.probeEnabled );
						bool oidnDenoise            = bag.GetBool(   "oidn_denoise",    dflt.oidnDenoise );
						OidnQuality oidnQuality     = ParseOidnQuality(   bag.GetString( "oidn_quality",   to_hint(dflt.oidnQuality) ) );
						OidnDevice  oidnDevice      = ParseOidnDevice(    bag.GetString( "oidn_device",    to_hint(dflt.oidnDevice) ) );
						OidnPrefilter oidnPrefilter = ParseOidnPrefilter( bag.GetString( "oidn_prefilter", to_hint(dflt.oidnPrefilter) ) );

						// Integrator pin (Tier 0) — identical vocabulary to auto_rasterizer.
						AutoIntegratorChoice integrator = dflt.integrator;
						if( bag.Has("integrator") ) {
							std::string iv = StripSurroundingQuotes( bag.GetString("integrator") );
							std::transform( iv.begin(), iv.end(), iv.begin(),
								[]( unsigned char c ){ return std::tolower( c ); } );
							if( iv == "pt" || iv == "pathtracing" || iv == "path_tracing" ) integrator = AutoIntegratorChoice::PT;
							else if( iv == "bdpt" )                                         integrator = AutoIntegratorChoice::BDPT;
							else if( iv == "vcm" )                                          integrator = AutoIntegratorChoice::VCM;
							else if( iv == "auto" )                                         integrator = AutoIntegratorChoice::Auto;
							else {
								GlobalLog()->PrintEx( eLog_Warning,
									"Parser: unknown auto_spectral_rasterizer integrator \"%s\"; defaulting to auto", iv.c_str() );
								integrator = AutoIntegratorChoice::Auto;
							}
						}

						RadianceMapConfig radianceMapConfig;
						if( bag.Has("radiance_map") )        radianceMapConfig.name         = String(bag.GetString("radiance_map").c_str());
						if( bag.Has("radiance_scale") )      radianceMapConfig.scale        = bag.GetDouble("radiance_scale");
						if( bag.Has("radiance_background") ) radianceMapConfig.isBackground = bag.GetBool("radiance_background");
						if( bag.Has("radiance_orient") ) {
							bag.GetVec3( "radiance_orient", radianceMapConfig.orientation );
							radianceMapConfig.orientation[0] *= DEG_TO_RAD;
							radianceMapConfig.orientation[1] *= DEG_TO_RAD;
							radianceMapConfig.orientation[2] *= DEG_TO_RAD;
						}

						PixelFilterConfig pixelFilterConfig;
						if( bag.Has("blue_noise_sampler") )  pixelFilterConfig.blueNoiseSampler = bag.GetBool("blue_noise_sampler");
						if( bag.Has("pixel_sampler") )       pixelFilterConfig.pixelSampler     = String(bag.GetString("pixel_sampler").c_str());
						if( bag.Has("pixel_sampler_param") ) pixelFilterConfig.pixelSamplerParam= bag.GetDouble("pixel_sampler_param");
						if( bag.Has("pixel_filter") )        pixelFilterConfig.filter           = String(bag.GetString("pixel_filter").c_str());
						if( bag.Has("pixel_filter_width") )  pixelFilterConfig.width            = bag.GetDouble("pixel_filter_width");
						if( bag.Has("pixel_filter_height") ) pixelFilterConfig.height           = bag.GetDouble("pixel_filter_height");
						if( bag.Has("pixel_filter_paramA") ) pixelFilterConfig.paramA           = bag.GetDouble("pixel_filter_paramA");
						if( bag.Has("pixel_filter_paramB") ) pixelFilterConfig.paramB           = bag.GetDouble("pixel_filter_paramB");

						// Spectral-core params replace path-guiding on this chunk (the
						// spectral domain has no guiding); accept both `hwss` and the legacy
						// `use_hwss` spelling, matching pathtracing_spectral_rasterizer.
						SpectralConfig spectralConfig;
						if( bag.Has("spectral_samples") ) spectralConfig.spectralSamples = bag.GetUInt("spectral_samples");
						if( bag.Has("num_wavelengths") )  spectralConfig.numWavelengths  = bag.GetUInt("num_wavelengths");
						if( bag.Has("nmbegin") )          spectralConfig.nmBegin         = bag.GetDouble("nmbegin");
						if( bag.Has("nmend") )            spectralConfig.nmEnd           = bag.GetDouble("nmend");
						if( bag.Has("hwss") )             spectralConfig.useHWSS         = bag.GetBool("hwss");
						if( bag.Has("use_hwss") )         spectralConfig.useHWSS         = bag.GetBool("use_hwss");

						AdaptiveSamplingConfig adaptiveConfig;
						if( bag.Has("adaptive_max_samples") ) adaptiveConfig.maxSamples = bag.GetUInt("adaptive_max_samples");
						if( bag.Has("adaptive_threshold") )   adaptiveConfig.threshold  = bag.GetDouble("adaptive_threshold");
						if( bag.Has("show_adaptive_map") )    adaptiveConfig.showMap    = bag.GetBool("show_adaptive_map");

						// Stability config minus optimal-MIS: the spectral integrators do not
						// allocate the optimal-MIS accumulator (SPECTRAL_PARITY_AUDIT §1), so
						// the descriptor below omits AddOptimalMISParams too.
						StabilityConfig stabilityConfig;
						if( bag.Has("direct_clamp") )                    stabilityConfig.directClamp                  = bag.GetDouble("direct_clamp");
						if( bag.Has("indirect_clamp") )                  stabilityConfig.indirectClamp                = bag.GetDouble("indirect_clamp");
						if( bag.Has("rr_min_depth") )                    stabilityConfig.rrMinDepth                   = bag.GetUInt("rr_min_depth");
						if( bag.Has("rr_threshold") )                    stabilityConfig.rrThreshold                  = bag.GetDouble("rr_threshold");
						if( bag.Has("max_diffuse_bounce") )              stabilityConfig.maxDiffuseBounce             = bag.GetUInt("max_diffuse_bounce");
						if( bag.Has("max_glossy_bounce") )               stabilityConfig.maxGlossyBounce              = bag.GetUInt("max_glossy_bounce");
						if( bag.Has("max_transmission_bounce") )         stabilityConfig.maxTransmissionBounce        = bag.GetUInt("max_transmission_bounce");
						if( bag.Has("max_translucent_bounce") )          stabilityConfig.maxTranslucentBounce         = bag.GetUInt("max_translucent_bounce");
						if( bag.Has("max_volume_bounce") )               stabilityConfig.maxVolumeBounce              = bag.GetUInt("max_volume_bounce");
						if( bag.Has("light_bvh") )                       stabilityConfig.useLightBVH                  = bag.GetBool("light_bvh");

						ProgressiveConfig progressiveConfig;
						if( bag.Has("progressive_rendering") )      progressiveConfig.enabled = bag.GetBool("progressive_rendering");
						if( bag.Has("progressive_samples_per_pass") ) {
							const unsigned int spp = bag.GetUInt("progressive_samples_per_pass");
							progressiveConfig.samplesPerPass = spp > 0 ? spp : 1;
						}

						return pJob.SetAutoSpectralRasterizer( integrator, numSamples,
							defaultshader.c_str(), radianceMapConfig,
							pixelFilterConfig,
							showLuminaires,
							spectralConfig,
							oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter, adaptiveConfig, stabilityConfig, progressiveConfig,
							probeEnabled );
					}

					const ChunkDescriptor& Describe() const override {
						static const ChunkDescriptor d = []{
							AutoRasterizerDefaults dflt;
							ChunkDescriptor cd;
							cd.keyword = "auto_spectral_rasterizer"; cd.category = ChunkCategory::Rasterizer;
							cd.description = "Spectral auto-routing integrator dispatcher: delegates to PT/BDPT/VCM spectral (Phase 1b: author pin via `integrator`, else PT; `probe` enables the Tier-2 render-time probe).";
							auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
							AddBaseRasterizerParams( P, dflt );
							{ auto& p = P(); p.name = "integrator"; p.kind = ValueKind::Enum; p.enumValues = {"auto","pt","bdpt","vcm"}; p.description = "Integrator selection: auto (dispatcher decides) or an explicit pin (pt/bdpt/vcm)"; p.defaultValueHint = to_hint(dflt.integrator); }
							{ auto& p = P(); p.name = "probe"; p.kind = ValueKind::Bool; p.description = "Enable the Tier-2 render-time probe (Phase 4): a cheap reduced-res/low-spp pre-render of candidate spectral integrators that picks per-scene (caustic median+reach -> VCM; strong-indirect sigma2T -> BDPT; else PT).  Only fires once production `samples` clears the activation-spp gate; below it the dispatcher uses the Tier-1 static best-guess.  Default off."; p.defaultValueHint = dflt.probeEnabled ? "TRUE" : "FALSE"; }
							AddPixelFilterParams( P );
							AddRadianceMapParams( P );
							AddSpectralCoreParams( P );
							AddAdaptiveSamplingParams( P );
							AddStabilityConfigParams( P );
							AddProgressiveParams( P );
							return cd;
						}();
						return d;
					}
				};

			struct PathTracingPelRasterizerAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					PathTracingPelDefaults dflt;
					std::string defaultshader   = bag.GetString( "defaultshader",  dflt.defaultShader );
					unsigned int numSamples     = bag.GetUInt(   "samples",        dflt.numPixelSamples );
					bool showLuminaires         = bag.GetBool(   "show_luminaires", dflt.showLuminaires );
					bool oidnDenoise            = bag.GetBool(   "oidn_denoise",    dflt.oidnDenoise );
					OidnQuality oidnQuality     = ParseOidnQuality(   bag.GetString( "oidn_quality",   to_hint(dflt.oidnQuality) ) );
					OidnDevice  oidnDevice      = ParseOidnDevice(    bag.GetString( "oidn_device",    to_hint(dflt.oidnDevice) ) );
					OidnPrefilter oidnPrefilter = ParseOidnPrefilter( bag.GetString( "oidn_prefilter", to_hint(dflt.oidnPrefilter) ) );

					RadianceMapConfig radianceMapConfig;
					if( bag.Has("radiance_map") )        radianceMapConfig.name         = String(bag.GetString("radiance_map").c_str());
					if( bag.Has("radiance_scale") )      radianceMapConfig.scale        = bag.GetDouble("radiance_scale");
					if( bag.Has("radiance_background") ) radianceMapConfig.isBackground = bag.GetBool("radiance_background");
					if( bag.Has("radiance_orient") ) {
						bag.GetVec3( "radiance_orient", radianceMapConfig.orientation );
						radianceMapConfig.orientation[0] *= DEG_TO_RAD;
						radianceMapConfig.orientation[1] *= DEG_TO_RAD;
						radianceMapConfig.orientation[2] *= DEG_TO_RAD;
					}

					PixelFilterConfig pixelFilterConfig;
					if( bag.Has("blue_noise_sampler") )  pixelFilterConfig.blueNoiseSampler = bag.GetBool("blue_noise_sampler");
					if( bag.Has("pixel_sampler") )       pixelFilterConfig.pixelSampler     = String(bag.GetString("pixel_sampler").c_str());
					if( bag.Has("pixel_sampler_param") ) pixelFilterConfig.pixelSamplerParam= bag.GetDouble("pixel_sampler_param");
					if( bag.Has("pixel_filter") )        pixelFilterConfig.filter           = String(bag.GetString("pixel_filter").c_str());
					if( bag.Has("pixel_filter_width") )  pixelFilterConfig.width            = bag.GetDouble("pixel_filter_width");
					if( bag.Has("pixel_filter_height") ) pixelFilterConfig.height           = bag.GetDouble("pixel_filter_height");
					if( bag.Has("pixel_filter_paramA") ) pixelFilterConfig.paramA           = bag.GetDouble("pixel_filter_paramA");
					if( bag.Has("pixel_filter_paramB") ) pixelFilterConfig.paramB           = bag.GetDouble("pixel_filter_paramB");

					SMSConfig smsConfig;
					if( bag.Has("sms_enabled") )          smsConfig.enabled         = bag.GetBool("sms_enabled");
					if( bag.Has("sms_max_iterations") )   smsConfig.maxIterations   = bag.GetUInt("sms_max_iterations");
					if( bag.Has("sms_threshold") )        smsConfig.threshold       = bag.GetDouble("sms_threshold");
					if( bag.Has("sms_max_chain_depth") )  smsConfig.maxChainDepth   = bag.GetUInt("sms_max_chain_depth");
					if( bag.Has("sms_biased") )           smsConfig.biased          = bag.GetBool("sms_biased");
					if( bag.Has("sms_bernoulli_trials") ) smsConfig.bernoulliTrials = bag.GetUInt("sms_bernoulli_trials");
					if( bag.Has("sms_multi_trials") )     smsConfig.multiTrials     = bag.GetUInt("sms_multi_trials");
					if( bag.Has("sms_photon_count") )     smsConfig.photonCount     = bag.GetUInt("sms_photon_count");
					if( bag.Has("sms_two_stage") )        smsConfig.twoStage        = bag.GetBool("sms_two_stage");
					if( bag.Has("sms_target_bounces") )   smsConfig.targetBounces   = bag.GetUInt("sms_target_bounces");
					if( bag.Has("sms_seeding") ) {
						std::string sv = StripSurroundingQuotes( bag.GetString("sms_seeding") );
						std::transform( sv.begin(), sv.end(), sv.begin(),
							[]( unsigned char c ){ return std::tolower( c ); } );
						if( sv == "uniform" )      smsConfig.seedingMode = SMSSeedingMode::Uniform;
						else if( sv == "snell" )   smsConfig.seedingMode = SMSSeedingMode::Snell;
						else                       smsConfig.seedingMode = SMSSeedingMode::Snell;   // unknown → snell fallback
					}

					PathGuidingConfig guidingConfig;
					if( bag.Has("pathguiding") )                                    guidingConfig.enabled              = bag.GetBool("pathguiding");
					if( bag.Has("pathguiding_iterations") )                         guidingConfig.trainingIterations   = bag.GetUInt("pathguiding_iterations");
					if( bag.Has("pathguiding_spp") )                                guidingConfig.trainingSPP          = bag.GetUInt("pathguiding_spp");
					if( bag.Has("pathguiding_combine_training") )                   guidingConfig.combineTrainingIterations = bag.GetBool("pathguiding_combine_training");
					if( bag.Has("pathguiding_online") )                             guidingConfig.online               = bag.GetBool("pathguiding_online");
					if( bag.Has("pathguiding_warmup_iterations") )                  guidingConfig.warmupIterations     = bag.GetUInt("pathguiding_warmup_iterations");
					if( bag.Has("pathguiding_alpha") )                              guidingConfig.alpha                = bag.GetDouble("pathguiding_alpha");
					if( bag.Has("pathguiding_learned_alpha") )                      guidingConfig.learnedAlpha         = bag.GetBool("pathguiding_learned_alpha");
					if( bag.Has("pathguiding_max_depth") )                          guidingConfig.maxGuidingDepth      = bag.GetUInt("pathguiding_max_depth");
					if( bag.Has("pathguiding_light_max_depth") )                    guidingConfig.maxLightGuidingDepth = bag.GetUInt("pathguiding_light_max_depth");
					if( bag.Has("pathguiding_sampling_type") ) {
						const std::string st = bag.GetString("pathguiding_sampling_type");
						guidingConfig.samplingType = ( st == "ris" || st == "RIS" ) ? eGuidingRIS : eGuidingOneSampleMIS;
					}
					if( bag.Has("pathguiding_ris_candidates") )                     guidingConfig.risCandidates                 = std::max( 2u, bag.GetUInt("pathguiding_ris_candidates") );
					if( bag.Has("pathguiding_complete_paths") )                     guidingConfig.completePathGuiding           = bag.GetBool("pathguiding_complete_paths");
					if( bag.Has("pathguiding_complete_path_strategy_selection") )   guidingConfig.completePathStrategySelection = bag.GetBool("pathguiding_complete_path_strategy_selection");
					if( bag.Has("pathguiding_complete_path_strategy_samples") )     guidingConfig.completePathStrategySamples   = bag.GetUInt("pathguiding_complete_path_strategy_samples");

					AdaptiveSamplingConfig adaptiveConfig;
					if( bag.Has("adaptive_max_samples") ) adaptiveConfig.maxSamples = bag.GetUInt("adaptive_max_samples");
					if( bag.Has("adaptive_threshold") )   adaptiveConfig.threshold  = bag.GetDouble("adaptive_threshold");
					if( bag.Has("show_adaptive_map") )    adaptiveConfig.showMap    = bag.GetBool("show_adaptive_map");

					StabilityConfig stabilityConfig;
					if( bag.Has("direct_clamp") )                    stabilityConfig.directClamp                  = bag.GetDouble("direct_clamp");
					if( bag.Has("indirect_clamp") )                  stabilityConfig.indirectClamp                = bag.GetDouble("indirect_clamp");
					if( bag.Has("rr_min_depth") )                    stabilityConfig.rrMinDepth                   = bag.GetUInt("rr_min_depth");
					if( bag.Has("rr_threshold") )                    stabilityConfig.rrThreshold                  = bag.GetDouble("rr_threshold");
					if( bag.Has("max_diffuse_bounce") )              stabilityConfig.maxDiffuseBounce             = bag.GetUInt("max_diffuse_bounce");
					if( bag.Has("max_glossy_bounce") )               stabilityConfig.maxGlossyBounce              = bag.GetUInt("max_glossy_bounce");
					if( bag.Has("max_transmission_bounce") )         stabilityConfig.maxTransmissionBounce        = bag.GetUInt("max_transmission_bounce");
					if( bag.Has("max_translucent_bounce") )          stabilityConfig.maxTranslucentBounce         = bag.GetUInt("max_translucent_bounce");
					if( bag.Has("max_volume_bounce") )               stabilityConfig.maxVolumeBounce              = bag.GetUInt("max_volume_bounce");
					if( bag.Has("light_bvh") )                       stabilityConfig.useLightBVH                  = bag.GetBool("light_bvh");
					if( bag.Has("optimal_mis") )                     stabilityConfig.optimalMIS                   = bag.GetBool("optimal_mis");
					if( bag.Has("optimal_mis_training_iterations") ) stabilityConfig.optimalMISTrainingIterations = bag.GetUInt("optimal_mis_training_iterations");
					if( bag.Has("optimal_mis_tile_size") )           stabilityConfig.optimalMISTileSize           = bag.GetUInt("optimal_mis_tile_size");
					// Transparent (Fresnel-attenuated) shadow rays — PT-only opt-in.
					if( bag.Has("transparent_shadows") )             stabilityConfig.transparentShadows           = bag.GetBool("transparent_shadows");

					ProgressiveConfig progressiveConfig;
					if( bag.Has("progressive_rendering") )      progressiveConfig.enabled = bag.GetBool("progressive_rendering");
					if( bag.Has("progressive_samples_per_pass") ) {
						const unsigned int spp = bag.GetUInt("progressive_samples_per_pass");
						progressiveConfig.samplesPerPass = spp > 0 ? spp : 1;
					}

					return pJob.SetPathTracingPelRasterizer( numSamples,
						defaultshader.c_str(), radianceMapConfig,
						pixelFilterConfig,
						showLuminaires,
						smsConfig, oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter, guidingConfig, adaptiveConfig, stabilityConfig, progressiveConfig );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						PathTracingPelDefaults dflt;
						ChunkDescriptor cd;
						cd.keyword = "pathtracing_pel_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "Pure unidirectional RGB path tracer (bypasses shader-op chain).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddBaseRasterizerParams( P, dflt );
						{ auto& p = P(); p.name = "choose_one_light";p.kind = ValueKind::Bool;   p.description = "Legacy — ignored (unified LightSampler always selects one light per NEE)"; p.defaultValueHint = ""; }
						AddPixelFilterParams( P );
						AddRadianceMapParams( P );
						AddSMSConfigParams( P );
						AddPathGuidingParams( P );
						AddAdaptiveSamplingParams( P );
						AddStabilityConfigParams( P );
						// Transparent (Fresnel-attenuated) shadow rays — PT-only
						// opt-in; not part of the shared StabilityConfig params
						// because BDPT/VCM/auto don't honour it.
						{ auto& p = P(); p.name = "transparent_shadows"; p.kind = ValueKind::Bool; p.description = "NEE shadow rays pass through specular dielectrics with Fresnel transmittance (PT only)"; p.defaultValueHint = to_hint(false); }
						AddOptimalMISParams( P );
						AddProgressiveParams( P );
						return cd;
					}();
					return d;
				}
			};

			struct PathTracingSpectralRasterizerAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					PathTracingSpectralDefaults dflt;
					std::string defaultshader   = bag.GetString( "defaultshader",  dflt.defaultShader );
					unsigned int numSamples     = bag.GetUInt(   "samples",        dflt.numPixelSamples );
					bool showLuminaires         = bag.GetBool(   "show_luminaires", dflt.showLuminaires );
					bool oidnDenoise            = bag.GetBool(   "oidn_denoise",    dflt.oidnDenoise );
					OidnQuality oidnQuality     = ParseOidnQuality(   bag.GetString( "oidn_quality",   to_hint(dflt.oidnQuality) ) );
					OidnDevice  oidnDevice      = ParseOidnDevice(    bag.GetString( "oidn_device",    to_hint(dflt.oidnDevice) ) );
					OidnPrefilter oidnPrefilter = ParseOidnPrefilter( bag.GetString( "oidn_prefilter", to_hint(dflt.oidnPrefilter) ) );

					RadianceMapConfig radianceMapConfig;
					if( bag.Has("radiance_map") )        radianceMapConfig.name         = String(bag.GetString("radiance_map").c_str());
					if( bag.Has("radiance_scale") )      radianceMapConfig.scale        = bag.GetDouble("radiance_scale");
					if( bag.Has("radiance_background") ) radianceMapConfig.isBackground = bag.GetBool("radiance_background");
					if( bag.Has("radiance_orient") ) {
						bag.GetVec3( "radiance_orient", radianceMapConfig.orientation );
						radianceMapConfig.orientation[0] *= DEG_TO_RAD;
						radianceMapConfig.orientation[1] *= DEG_TO_RAD;
						radianceMapConfig.orientation[2] *= DEG_TO_RAD;
					}

					PixelFilterConfig pixelFilterConfig;
					if( bag.Has("blue_noise_sampler") )  pixelFilterConfig.blueNoiseSampler = bag.GetBool("blue_noise_sampler");
					if( bag.Has("pixel_sampler") )       pixelFilterConfig.pixelSampler     = String(bag.GetString("pixel_sampler").c_str());
					if( bag.Has("pixel_sampler_param") ) pixelFilterConfig.pixelSamplerParam= bag.GetDouble("pixel_sampler_param");
					if( bag.Has("pixel_filter") )        pixelFilterConfig.filter           = String(bag.GetString("pixel_filter").c_str());
					if( bag.Has("pixel_filter_width") )  pixelFilterConfig.width            = bag.GetDouble("pixel_filter_width");
					if( bag.Has("pixel_filter_height") ) pixelFilterConfig.height           = bag.GetDouble("pixel_filter_height");
					if( bag.Has("pixel_filter_paramA") ) pixelFilterConfig.paramA           = bag.GetDouble("pixel_filter_paramA");
					if( bag.Has("pixel_filter_paramB") ) pixelFilterConfig.paramB           = bag.GetDouble("pixel_filter_paramB");

					SpectralConfig spectralConfig;
					if( bag.Has("spectral_samples") ) spectralConfig.spectralSamples = bag.GetUInt("spectral_samples");
					if( bag.Has("num_wavelengths") )  spectralConfig.numWavelengths  = bag.GetUInt("num_wavelengths");
					if( bag.Has("nmbegin") )          spectralConfig.nmBegin         = bag.GetDouble("nmbegin");
					if( bag.Has("nmend") )            spectralConfig.nmEnd           = bag.GetDouble("nmend");
					// Accept both `hwss` (canonical, used by other spectral
					// rasterizers) and the legacy `use_hwss` spelling that
					// only this parser historically accepted.
					if( bag.Has("hwss") )             spectralConfig.useHWSS         = bag.GetBool("hwss");
					if( bag.Has("use_hwss") )         spectralConfig.useHWSS         = bag.GetBool("use_hwss");

					SMSConfig smsConfig;
					if( bag.Has("sms_enabled") )          smsConfig.enabled         = bag.GetBool("sms_enabled");
					if( bag.Has("sms_max_iterations") )   smsConfig.maxIterations   = bag.GetUInt("sms_max_iterations");
					if( bag.Has("sms_threshold") )        smsConfig.threshold       = bag.GetDouble("sms_threshold");
					if( bag.Has("sms_max_chain_depth") )  smsConfig.maxChainDepth   = bag.GetUInt("sms_max_chain_depth");
					if( bag.Has("sms_biased") )           smsConfig.biased          = bag.GetBool("sms_biased");
					if( bag.Has("sms_bernoulli_trials") ) smsConfig.bernoulliTrials = bag.GetUInt("sms_bernoulli_trials");
					if( bag.Has("sms_multi_trials") )     smsConfig.multiTrials     = bag.GetUInt("sms_multi_trials");
					if( bag.Has("sms_photon_count") )     smsConfig.photonCount     = bag.GetUInt("sms_photon_count");
				if( bag.Has("sms_two_stage") )        smsConfig.twoStage        = bag.GetBool("sms_two_stage");
				if( bag.Has("sms_target_bounces") )   smsConfig.targetBounces   = bag.GetUInt("sms_target_bounces");
				if( bag.Has("sms_seeding") ) {
					std::string sv = StripSurroundingQuotes( bag.GetString("sms_seeding") );
					std::transform( sv.begin(), sv.end(), sv.begin(),
						[]( unsigned char c ){ return std::tolower( c ); } );
					if( sv == "uniform" )      smsConfig.seedingMode = SMSSeedingMode::Uniform;
					else if( sv == "snell" )   smsConfig.seedingMode = SMSSeedingMode::Snell;
					else                       smsConfig.seedingMode = SMSSeedingMode::Snell;   // unknown → snell fallback
					// Adding a third mode (Specular Polynomials, MPG, ...) extends
					// the enum in `SMSConfig.h` and adds one branch here.
				}

					AdaptiveSamplingConfig adaptiveConfig;
					if( bag.Has("adaptive_max_samples") ) adaptiveConfig.maxSamples = bag.GetUInt("adaptive_max_samples");
					if( bag.Has("adaptive_threshold") )   adaptiveConfig.threshold  = bag.GetDouble("adaptive_threshold");
					if( bag.Has("show_adaptive_map") )    adaptiveConfig.showMap    = bag.GetBool("show_adaptive_map");

					StabilityConfig stabilityConfig;
					if( bag.Has("direct_clamp") )                    stabilityConfig.directClamp                  = bag.GetDouble("direct_clamp");
					if( bag.Has("indirect_clamp") )                  stabilityConfig.indirectClamp                = bag.GetDouble("indirect_clamp");
					if( bag.Has("rr_min_depth") )                    stabilityConfig.rrMinDepth                   = bag.GetUInt("rr_min_depth");
					if( bag.Has("rr_threshold") )                    stabilityConfig.rrThreshold                  = bag.GetDouble("rr_threshold");
					if( bag.Has("max_diffuse_bounce") )              stabilityConfig.maxDiffuseBounce             = bag.GetUInt("max_diffuse_bounce");
					if( bag.Has("max_glossy_bounce") )               stabilityConfig.maxGlossyBounce              = bag.GetUInt("max_glossy_bounce");
					if( bag.Has("max_transmission_bounce") )         stabilityConfig.maxTransmissionBounce        = bag.GetUInt("max_transmission_bounce");
					if( bag.Has("max_translucent_bounce") )          stabilityConfig.maxTranslucentBounce         = bag.GetUInt("max_translucent_bounce");
					if( bag.Has("max_volume_bounce") )               stabilityConfig.maxVolumeBounce              = bag.GetUInt("max_volume_bounce");
					if( bag.Has("light_bvh") )                       stabilityConfig.useLightBVH                  = bag.GetBool("light_bvh");
					// Transparent (Fresnel-attenuated) shadow rays — PT-only opt-in.
					// Honoured on the spectral (NM / HWSS) NEE path too.
					if( bag.Has("transparent_shadows") )             stabilityConfig.transparentShadows           = bag.GetBool("transparent_shadows");
					// Optimal MIS not parsed: PathTracingIntegrator's
					// NM/HWSS branches reference rc.pOptimalMIS, but
					// PixelBasedSpectralIntegratingRasterizer (the parent)
					// does not allocate the accumulator — the branch is
					// always-false on spectral.  Descriptor below also
					// omits AddOptimalMISParams.  See
					// docs/SPECTRAL_PARITY_AUDIT.md §1, §2.4.

					ProgressiveConfig progressiveConfig;
					if( bag.Has("progressive_rendering") )      progressiveConfig.enabled = bag.GetBool("progressive_rendering");
					if( bag.Has("progressive_samples_per_pass") ) {
						const unsigned int spp = bag.GetUInt("progressive_samples_per_pass");
						progressiveConfig.samplesPerPass = spp > 0 ? spp : 1;
					}

					return pJob.SetPathTracingSpectralRasterizer( numSamples,
						defaultshader.c_str(), radianceMapConfig,
						pixelFilterConfig,
						showLuminaires,
						spectralConfig,
						smsConfig, oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter, adaptiveConfig, stabilityConfig, progressiveConfig );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						PathTracingSpectralDefaults dflt;
						SpectralConfig spectralDflt;
						ChunkDescriptor cd;
						cd.keyword = "pathtracing_spectral_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "Pure unidirectional spectral path tracer (bypasses shader-op chain).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddBaseRasterizerParams( P, dflt );
						{ auto& p = P(); p.name = "choose_one_light";p.kind = ValueKind::Bool;   p.description = "Legacy — ignored (unified LightSampler always selects one light per NEE)"; p.defaultValueHint = ""; }
						AddPixelFilterParams( P );
						AddRadianceMapParams( P );
						// PT spectral consumes only the core spectral
						// fields; RGB-to-SPD conversion is done in the
						// painters pipeline.
						AddSpectralCoreParams( P );
						// Legacy alias for `hwss` accepted only by this
						// parser (other spectral integrators use `hwss`).
						{ auto& p = P(); p.name = "use_hwss";        p.kind = ValueKind::Bool; p.description = "Legacy alias for `hwss`"; p.defaultValueHint = to_hint(spectralDflt.useHWSS); }
						AddSMSConfigParams( P );
						AddAdaptiveSamplingParams( P );
						AddStabilityConfigParams( P );
						// Transparent (Fresnel-attenuated) shadow rays — PT-only
						// opt-in; honoured on the spectral NEE path too.
						{ auto& p = P(); p.name = "transparent_shadows"; p.kind = ValueKind::Bool; p.description = "NEE shadow rays pass through specular dielectrics with Fresnel transmittance (PT only)"; p.defaultValueHint = to_hint(false); }
						// Intentionally NO AddOptimalMISParams: spectral
						// parent doesn't allocate the accumulator.  See
						// Finalize note and SPECTRAL_PARITY_AUDIT.md §2.4.
						AddProgressiveParams( P );
						return cd;
					}();
					return d;
				}
			};

			struct MLTRasterizerAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					MLTDefaults dflt;
					std::string defaultshader     = bag.GetString( "defaultshader",       dflt.defaultShader );
					unsigned int maxEyeDepth      = bag.GetUInt(   "max_eye_depth",       dflt.maxEyeDepth );
					unsigned int maxLightDepth    = bag.GetUInt(   "max_light_depth",     dflt.maxLightDepth );
					unsigned int bootstrapSamples = bag.GetUInt(   "bootstrap_samples",   dflt.nBootstrap );
					unsigned int chains           = bag.GetUInt(   "chains",              dflt.nChains );
					unsigned int mutationsPerPixel= bag.GetUInt(   "mutations_per_pixel", dflt.nMutationsPerPixel );
					double largeStepProb          = bag.GetDouble( "large_step_prob",     dflt.largeStepProb );
					bool showLuminaires           = bag.GetBool(   "show_luminaires",     dflt.showLuminaires );
					// MLT defaults oidn_denoise to FALSE because the
					// entire MLT image lives in the splat film, so OIDN
					// would denoise an already-accumulated / filter-
					// reconstructed image.  That is precisely the case
					// the BDPT comment ("their splatted accumulation
					// pattern is incompatible with OIDN", see
					// BDPTRasterizerBase.cpp) warns about: OIDN is
					// trained on raw Monte Carlo noise, not on the
					// smoother distribution you get from splat film
					// resolve, and it can over-smooth caustics.
					// BDPT avoids this by denoising the primary image
					// first and ADDING splats afterward; MLT has no
					// separate "primary" path to split out, so we opt
					// out by default.  Users who specifically want
					// OIDN applied to their MLT result (e.g. to denoise
					// the residual Markov-chain noise in a long render)
					// can still enable it with `oidn_denoise true`.
					bool oidnDenoise              = bag.GetBool(   "oidn_denoise",        dflt.oidnDenoise );
					OidnQuality oidnQuality       = ParseOidnQuality(   bag.GetString( "oidn_quality",   to_hint(dflt.oidnQuality) ) );
					OidnDevice  oidnDevice        = ParseOidnDevice(    bag.GetString( "oidn_device",    to_hint(dflt.oidnDevice) ) );
					OidnPrefilter oidnPrefilter   = ParseOidnPrefilter( bag.GetString( "oidn_prefilter", to_hint(dflt.oidnPrefilter) ) );

					// Pixel filter for sub-pixel reconstruction.  Default
					// is Mitchell-Netravali (B=C=1/3, width/height=1.0)
					// so existing MLT scenes automatically get proper
					// sub-pixel reconstruction.  Before this fix MLT
					// splatted straight to integer pixels with no
					// filtering — the cause of the hard edges / aliasing
					// the user reported.  Scenes that genuinely want an
					// unfiltered image can specify pixel_filter none.
					PixelFilterConfig pixelFilterConfig;
					if( bag.Has("pixel_filter") )        pixelFilterConfig.filter = String(bag.GetString("pixel_filter").c_str());
					if( bag.Has("pixel_filter_width") )  pixelFilterConfig.width  = bag.GetDouble("pixel_filter_width");
					if( bag.Has("pixel_filter_height") ) pixelFilterConfig.height = bag.GetDouble("pixel_filter_height");
					if( bag.Has("pixel_filter_paramA") ) pixelFilterConfig.paramA = bag.GetDouble("pixel_filter_paramA");
					if( bag.Has("pixel_filter_paramB") ) pixelFilterConfig.paramB = bag.GetDouble("pixel_filter_paramB");

					StabilityConfig stabilityConfig;
					if( bag.Has("light_bvh") )           stabilityConfig.useLightBVH        = bag.GetBool("light_bvh");

					return pJob.SetMLTRasterizer( maxEyeDepth, maxLightDepth,
						bootstrapSamples, chains, mutationsPerPixel, largeStepProb,
						defaultshader.c_str(), showLuminaires, oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter,
						pixelFilterConfig,
						stabilityConfig );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						MLTDefaults dflt;
						StabilityConfig stabilityDflt;
						ChunkDescriptor cd;
						cd.keyword = "mlt_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "Metropolis Light Transport (RGB).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "defaultshader";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Shader}; p.description = "Default shader chain"; p.defaultValueHint = to_hint(dflt.defaultShader); }
						{ auto& p = P(); p.name = "max_eye_depth";    p.kind = ValueKind::UInt;   p.description = "Max eye subpath depth";                  p.defaultValueHint = to_hint(dflt.maxEyeDepth); }
						{ auto& p = P(); p.name = "max_light_depth";  p.kind = ValueKind::UInt;   p.description = "Max light subpath depth";                p.defaultValueHint = to_hint(dflt.maxLightDepth); }
						{ auto& p = P(); p.name = "bootstrap_samples";p.kind = ValueKind::UInt;   p.description = "Bootstrap samples";                      p.defaultValueHint = to_hint(dflt.nBootstrap); }
						{ auto& p = P(); p.name = "chains";           p.kind = ValueKind::UInt;   p.description = "Number of Markov chains";                p.defaultValueHint = to_hint(dflt.nChains); }
						{ auto& p = P(); p.name = "mutations_per_pixel"; p.kind = ValueKind::UInt;p.description = "Mutations per pixel";                     p.defaultValueHint = to_hint(dflt.nMutationsPerPixel); }
						{ auto& p = P(); p.name = "large_step_prob";  p.kind = ValueKind::Double; p.description = "Probability of large-step mutation";      p.defaultValueHint = to_hint(dflt.largeStepProb); }
						{ auto& p = P(); p.name = "show_luminaires";  p.kind = ValueKind::Bool;   p.description = "Show direct-visible luminaires";         p.defaultValueHint = to_hint(dflt.showLuminaires); }
						{ auto& p = P(); p.name = "oidn_denoise";     p.kind = ValueKind::Bool;   p.description = "Enable OIDN denoiser";                   p.defaultValueHint = to_hint(dflt.oidnDenoise); }
						{ auto& p = P(); p.name = "oidn_quality";     p.kind = ValueKind::Enum;   p.enumValues = {"auto","high","balanced","fast"}; p.description = "OIDN quality preset (auto picks from render-time / megapixels)"; p.defaultValueHint = to_hint(dflt.oidnQuality); }
						{ auto& p = P(); p.name = "oidn_device";      p.kind = ValueKind::Enum;   p.enumValues = {"auto","cpu","gpu"};              p.description = "OIDN device backend (auto = prefer GPU, fall back to CPU)";       p.defaultValueHint = to_hint(dflt.oidnDevice); }
					{ auto& p = P(); p.name = "oidn_prefilter";   p.kind = ValueKind::Enum;   p.enumValues = {"fast","accurate"};               p.description = "OIDN aux source mode (fast = retrace/first-hit, accurate = inline first-non-delta + prefilter)"; p.defaultValueHint = to_hint(dflt.oidnPrefilter); }
						{ auto& p = P(); p.name = "choose_one_light"; p.kind = ValueKind::Bool;   p.description = "Legacy — ignored (unified LightSampler always selects one light per NEE)"; p.defaultValueHint = ""; }
						AddPixelFilterParams( P );
						// MLT accepts only light_bvh from StabilityConfig.
						{ auto& p = P(); p.name = "light_bvh";            p.kind = ValueKind::Bool;   p.description = "Use a BVH over lights for NEE";         p.defaultValueHint = to_hint(stabilityDflt.useLightBVH); }
						return cd;
					}();
					return d;
				}
			};

			struct MLTSpectralRasterizerAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					MLTSpectralDefaults dflt;
					std::string defaultshader     = bag.GetString( "defaultshader",       dflt.defaultShader );
					unsigned int maxEyeDepth      = bag.GetUInt(   "max_eye_depth",       dflt.maxEyeDepth );
					unsigned int maxLightDepth    = bag.GetUInt(   "max_light_depth",     dflt.maxLightDepth );
					unsigned int bootstrapSamples = bag.GetUInt(   "bootstrap_samples",   dflt.nBootstrap );
					unsigned int chains           = bag.GetUInt(   "chains",              dflt.nChains );
					unsigned int mutationsPerPixel= bag.GetUInt(   "mutations_per_pixel", dflt.nMutationsPerPixel );
					double largeStepProb          = bag.GetDouble( "large_step_prob",     dflt.largeStepProb );
					bool showLuminaires           = bag.GetBool(   "show_luminaires",     dflt.showLuminaires );
					// MLT spectral also defaults OIDN off — see the Pel
					// MLT parser for the detailed rationale.
					bool oidnDenoise              = bag.GetBool(   "oidn_denoise",        dflt.oidnDenoise );
					OidnQuality oidnQuality       = ParseOidnQuality(   bag.GetString( "oidn_quality",   to_hint(dflt.oidnQuality) ) );
					OidnDevice  oidnDevice        = ParseOidnDevice(    bag.GetString( "oidn_device",    to_hint(dflt.oidnDevice) ) );
					OidnPrefilter oidnPrefilter   = ParseOidnPrefilter( bag.GetString( "oidn_prefilter", to_hint(dflt.oidnPrefilter) ) );

					SpectralConfig spectralConfig;
					if( bag.Has("nmbegin") )          spectralConfig.nmBegin         = bag.GetDouble("nmbegin");
					if( bag.Has("nmend") )            spectralConfig.nmEnd           = bag.GetDouble("nmend");
					if( bag.Has("spectral_samples") ) spectralConfig.spectralSamples = bag.GetUInt("spectral_samples");
					if( bag.Has("num_wavelengths") )  spectralConfig.numWavelengths  = bag.GetUInt("num_wavelengths");
					if( bag.Has("hwss") )             spectralConfig.useHWSS         = bag.GetBool("hwss");

					PixelFilterConfig pixelFilterConfig;
					if( bag.Has("pixel_filter") )        pixelFilterConfig.filter = String(bag.GetString("pixel_filter").c_str());
					if( bag.Has("pixel_filter_width") )  pixelFilterConfig.width  = bag.GetDouble("pixel_filter_width");
					if( bag.Has("pixel_filter_height") ) pixelFilterConfig.height = bag.GetDouble("pixel_filter_height");
					if( bag.Has("pixel_filter_paramA") ) pixelFilterConfig.paramA = bag.GetDouble("pixel_filter_paramA");
					if( bag.Has("pixel_filter_paramB") ) pixelFilterConfig.paramB = bag.GetDouble("pixel_filter_paramB");

					StabilityConfig stabilityConfig;
					if( bag.Has("light_bvh") )           stabilityConfig.useLightBVH        = bag.GetBool("light_bvh");

					return pJob.SetMLTSpectralRasterizer( maxEyeDepth, maxLightDepth,
						bootstrapSamples, chains, mutationsPerPixel, largeStepProb,
						defaultshader.c_str(), showLuminaires,
						spectralConfig, oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter,
						pixelFilterConfig,
						stabilityConfig );
				}

				const ChunkDescriptor& Describe() const override
				{
					static const ChunkDescriptor d = []{
						MLTSpectralDefaults dflt;
						StabilityConfig stabilityDflt;
						ChunkDescriptor cd;
						cd.keyword = "mlt_spectral_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "Metropolis Light Transport (spectral).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "defaultshader";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Shader}; p.description = "Default shader chain"; p.defaultValueHint = to_hint(dflt.defaultShader); }
						{ auto& p = P(); p.name = "max_eye_depth";    p.kind = ValueKind::UInt;   p.description = "Max eye subpath depth";                  p.defaultValueHint = to_hint(dflt.maxEyeDepth); }
						{ auto& p = P(); p.name = "max_light_depth";  p.kind = ValueKind::UInt;   p.description = "Max light subpath depth";                p.defaultValueHint = to_hint(dflt.maxLightDepth); }
						{ auto& p = P(); p.name = "bootstrap_samples";p.kind = ValueKind::UInt;   p.description = "Bootstrap samples";                      p.defaultValueHint = to_hint(dflt.nBootstrap); }
						{ auto& p = P(); p.name = "chains";           p.kind = ValueKind::UInt;   p.description = "Number of Markov chains";                p.defaultValueHint = to_hint(dflt.nChains); }
						{ auto& p = P(); p.name = "mutations_per_pixel"; p.kind = ValueKind::UInt;p.description = "Mutations per pixel";                     p.defaultValueHint = to_hint(dflt.nMutationsPerPixel); }
						{ auto& p = P(); p.name = "large_step_prob";  p.kind = ValueKind::Double; p.description = "Probability of large-step mutation";      p.defaultValueHint = to_hint(dflt.largeStepProb); }
						{ auto& p = P(); p.name = "show_luminaires";  p.kind = ValueKind::Bool;   p.description = "Show direct-visible luminaires";         p.defaultValueHint = to_hint(dflt.showLuminaires); }
						{ auto& p = P(); p.name = "oidn_denoise";     p.kind = ValueKind::Bool;   p.description = "Enable OIDN denoiser";                   p.defaultValueHint = to_hint(dflt.oidnDenoise); }
						{ auto& p = P(); p.name = "oidn_quality";     p.kind = ValueKind::Enum;   p.enumValues = {"auto","high","balanced","fast"}; p.description = "OIDN quality preset (auto picks from render-time / megapixels)"; p.defaultValueHint = to_hint(dflt.oidnQuality); }
						{ auto& p = P(); p.name = "oidn_device";      p.kind = ValueKind::Enum;   p.enumValues = {"auto","cpu","gpu"};              p.description = "OIDN device backend (auto = prefer GPU, fall back to CPU)";       p.defaultValueHint = to_hint(dflt.oidnDevice); }
					{ auto& p = P(); p.name = "oidn_prefilter";   p.kind = ValueKind::Enum;   p.enumValues = {"fast","accurate"};               p.description = "OIDN aux source mode (fast = retrace/first-hit, accurate = inline first-non-delta + prefilter)"; p.defaultValueHint = to_hint(dflt.oidnPrefilter); }
						AddPixelFilterParams( P );
						// MLT spectral consumes only the core spectral
						// fields; RGB-to-SPD conversion is done in the
						// painters pipeline.
						AddSpectralCoreParams( P );
						// MLT accepts only light_bvh from StabilityConfig.
						{ auto& p = P(); p.name = "light_bvh";            p.kind = ValueKind::Bool;   p.description = "Use a BVH over lights for NEE";         p.defaultValueHint = to_hint(stabilityDflt.useLightBVH); }
						return cd;
					}();
					return d;
				}
			};

			struct FileRasterizerOutputAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string pattern   = bag.GetString( "pattern",  "none" );
					bool        multiple  = bag.GetBool(   "multiple", false );
					int         bpp       = bag.GetInt(    "bpp",      8 );

					char type = 0;
					if( bag.Has("type") ) {
						std::string t = bag.GetString("type");
						if( t == "TGA" ) {
							type = 0;
						} else if( t == "PPM" ) {
							type = 1;
						} else if( t == "PNG" ) {
					#ifndef NO_PNG_SUPPORT
							type = 2;
					#else
							type = 0;
							GlobalLog()->PrintEasyWarning( "AsciiCommandParser::ParseAddRasterizeroutput::File: NO PNG SUPPORT was compiled, reverting to TGA instead" );
					#endif
						} else if( t == "TIFF" ) {
					#ifndef NO_TIFF_SUPPORT
							type = 4;
					#else
							type = 0;
							GlobalLog()->PrintEasyWarning( "AsciiCommandParser::ParseAddRasterizeroutput::File: NO TIFF SUPPORT was compiled, reverting to TGA instead" );
					#endif
						} else if( t == "HDR" ) {
							type = 3;
						} else if( t == "RGBEA" ) {
							type = 5;
						} else if( t == "EXR" ) {
					#ifndef NO_EXR_SUPPORT
							type = 6;
					#else
							type = 0;
							GlobalLog()->PrintEasyWarning( "AsciiCommandParser::ParseAddRasterizeroutput::File: NO EXR SUPPORT was compiled, reverting to TGA instead" );
					#endif
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Unknown output file type type `%s`", t.c_str() );
							return false;
						}
					}

					char color_space = 1;
					if( bag.Has("color_space") ) {
						std::string cs = bag.GetString("color_space");
						if( cs=="Rec709RGB_Linear" ) {
							color_space = 0;
						} else if( cs=="sRGB" ) {
							color_space = 1;
						} else if( cs=="ROMMRGB_Linear" ) {
							color_space = 2;
						} else if( cs=="ProPhotoRGB" ) {
							color_space = 3;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Unknown color space `%s`", cs.c_str() );
							return false;
						}
					}

					// Display pipeline (Landing 1).  exposure default = 0
					// EV (no scaling).  display_transform default
					// depends on the format:
					//   - HDR (EXR / HDR / RGBEA): "none" (write verbatim
					//     radiance; tone-mapping an archival output would
					//     corrupt it)
					//   - LDR (PNG / TGA / PPM / TIFF): "aces" (proper
					//     highlight rolloff for an LDR display; see
					//     docs/PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_1.md)
					// Picking the default per-type here means the user's
					// 86+ existing HDR scenes don't trip the constructor's
					// "display_transform ignored on HDR" warning at every
					// render; the warning now only fires when the user
					// explicitly sets a non-none value on an HDR output
					// (genuine misuse).
					const double exposureEV = bag.GetDouble( "exposure", 0.0 );

					const bool typeIsHDR = ( type == 3 /*HDR*/ ||
					                         type == 5 /*RGBEA*/ ||
					                         type == 6 /*EXR*/ );
					char display_transform = typeIsHDR ? 0 /*none*/ : 2 /*ACES*/;
					if( bag.Has("display_transform") ) {
						std::string s = bag.GetString("display_transform");
						if      ( s == "none"     ) display_transform = 0;
						else if ( s == "reinhard" ) display_transform = 1;
						else if ( s == "aces"     ) display_transform = 2;
						else if ( s == "agx"      ) display_transform = 3;
						else if ( s == "hable"    ) display_transform = 4;
						else {
							GlobalLog()->PrintEx( eLog_Error,
								"ChunkParser:: Unknown display_transform `%s` "
								"(expected: none|reinhard|aces|agx|hable)", s.c_str() );
							return false;
						}
					}

					// EXR-specific knobs.  Default piz + alpha.
					char exr_compression = 2;	// PIZ default
					if( bag.Has("exr_compression") ) {
						std::string s = bag.GetString("exr_compression");
						if      ( s == "none" ) exr_compression = 0;
						else if ( s == "zip"  ) exr_compression = 1;
						else if ( s == "piz"  ) exr_compression = 2;
						else if ( s == "dwaa" ) exr_compression = 3;
						else {
							GlobalLog()->PrintEx( eLog_Error,
								"ChunkParser:: Unknown exr_compression `%s` "
								"(expected: none|zip|piz|dwaa)", s.c_str() );
							return false;
						}
					}
					const bool exr_with_alpha = bag.GetBool( "exr_with_alpha", true );

					return pJob.AddFileRasterizerOutput(
						pattern.c_str(), multiple, type, (unsigned char)bpp, color_space,
						exposureEV, display_transform, exr_compression, exr_with_alpha );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "file_rasterizeroutput"; cd.category = ChunkCategory::RasterizerOutput;
						cd.description = "Writes rendered frames to disk.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "pattern";    p.kind = ValueKind::Filename; p.description = "Output path pattern (with optional frame placeholders)"; p.defaultValueHint = "out.exr"; }
						{ auto& p = P(); p.name = "multiple";   p.kind = ValueKind::Bool;    p.description = "Emit one file per frame (for animation)";                p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "type";       p.kind = ValueKind::Enum;    p.enumValues = {"TGA","PPM","PNG","HDR","TIFF","RGBEA","EXR"};             p.description = "File format";                                                p.defaultValueHint = "EXR"; }
						{ auto& p = P(); p.name = "bpp";        p.kind = ValueKind::Enum;    p.enumValues = {"8","16","32"};                                           p.description = "Bits per channel (format-dependent)";                        p.defaultValueHint = "32"; }
						{ auto& p = P(); p.name = "color_space";p.kind = ValueKind::Enum;    p.enumValues = {"Rec709RGB_Linear","sRGB","ROMMRGB_Linear","ProPhotoRGB"};p.description = "Output colour space";                                        p.defaultValueHint = "sRGB"; }
						{ auto& p = P(); p.name = "exposure";          p.kind = ValueKind::Double; p.description = "Exposure offset in EV stops; 0 = no scaling.  LDR formats only — HDR formats warn and ignore."; p.defaultValueHint = "0.0"; }
						{ auto& p = P(); p.name = "display_transform"; p.kind = ValueKind::Enum;   p.enumValues = {"none","reinhard","aces","agx","hable"}; p.description = "Tone-mapping curve applied between linear radiance and OETF.  LDR formats only — HDR formats warn and ignore.  Default: aces for LDR formats; none for HDR formats."; p.defaultValueHint = "aces (LDR) / none (HDR)"; }
						{ auto& p = P(); p.name = "exr_compression";   p.kind = ValueKind::Enum;   p.enumValues = {"none","zip","piz","dwaa"};              p.description = "EXR compression algorithm.  EXR only.";  p.defaultValueHint = "piz"; }
						{ auto& p = P(); p.name = "exr_with_alpha";    p.kind = ValueKind::Bool;   p.description = "Write alpha channel into the EXR.  EXR only.";  p.defaultValueHint = "TRUE"; }
						return cd;
					}();
					return d;
				}
			};


			//////////////////////////////////////////
			// Photon Mapping
			//////////////////////////////////////////

			struct CausticPelPhotonMapGenerateAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					unsigned int photons          = bag.GetUInt(   "num",                    10000 );
					double power_scale            = bag.GetDouble( "power_scale",            1.0 );
					unsigned int maxRecur         = bag.GetUInt(   "max_recursion",          10 );
					double minImportance          = bag.GetDouble( "min_importance",         0.01 );
					bool branch                   = bag.GetBool(   "branch",                 true );
					bool reflect                  = bag.GetBool(   "reflect",                true );
					bool refract                  = bag.GetBool(   "refract",                true );
					bool shootFromNonMeshLights   = bag.GetBool(   "shootFromNonMeshLights", true );
					bool shootFromMeshLights      = bag.GetBool(   "shootFromMeshLights",    true );
					unsigned int temporal_samples = bag.GetUInt(   "temporal_samples",       100 );
					bool regenerate               = bag.GetBool(   "regenerate",             true );

					std::cout << "Queued Caustic Pel Photons (will shoot at render time)" << std::endl;

					return pJob.ShootCausticPelPhotons( photons, power_scale, maxRecur, minImportance, branch, reflect, refract, shootFromNonMeshLights, temporal_samples, regenerate, shootFromMeshLights );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "caustic_pel_photonmap"; cd.category = ChunkCategory::PhotonMap;
						cd.description = "Caustic photon map generation (RGB).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddPhotonMapGenerateCommonParams( P );
						return cd;
					}();
					return d;
				}
			};

			struct CausticSpectralPhotonMapGenerateAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					unsigned int photons          = bag.GetUInt(   "num",              10000 );
					double power_scale            = bag.GetDouble( "power_scale",      1.0 );
					unsigned int maxRecur         = bag.GetUInt(   "max_recursion",    10 );
					double nmbegin                = bag.GetDouble( "nmbegin",          400.0 );
					double nmend                  = bag.GetDouble( "nmend",            700.0 );
					unsigned int numWavelengths   = bag.GetUInt(   "num_wavelengths",  30 );
					double minImportance          = bag.GetDouble( "min_importance",   0.01 );
					bool branch                   = bag.GetBool(   "branch",           true );
					bool reflect                  = bag.GetBool(   "reflect",          true );
					bool refract                  = bag.GetBool(   "refract",          true );
					unsigned int temporal_samples = bag.GetUInt(   "temporal_samples", 100 );
					bool regenerate               = bag.GetBool(   "regenerate",       true );

					std::cout << "Queued Caustic Spectral Photons (will shoot at render time)" << std::endl;

					return pJob.ShootCausticSpectralPhotons( photons, power_scale, maxRecur, minImportance, nmbegin, nmend, numWavelengths, branch, reflect, refract, temporal_samples, regenerate );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "caustic_spectral_photonmap"; cd.category = ChunkCategory::PhotonMap;
						cd.description = "Caustic photon map generation (spectral).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddPhotonMapGenerateCommonParams( P );
						{ auto& p = P(); p.name = "nmbegin";         p.kind = ValueKind::Double; p.description = "Start wavelength (nm)"; p.defaultValueHint = "400"; }
						{ auto& p = P(); p.name = "nmend";           p.kind = ValueKind::Double; p.description = "End wavelength (nm)"; p.defaultValueHint = "700"; }
						{ auto& p = P(); p.name = "num_wavelengths"; p.kind = ValueKind::UInt;   p.description = "Spectral sample count"; p.defaultValueHint = "16"; }
						return cd;
					}();
					return d;
				}
			};

			struct GlobalSpectralPhotonMapGenerateAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					unsigned int photons          = bag.GetUInt(   "num",              10000 );
					double power_scale            = bag.GetDouble( "power_scale",      1.0 );
					unsigned int maxRecur         = bag.GetUInt(   "max_recursion",    10 );
					double nmbegin                = bag.GetDouble( "nmbegin",          400.0 );
					double nmend                  = bag.GetDouble( "nmend",            700.0 );
					unsigned int numWavelengths   = bag.GetUInt(   "num_wavelengths",  30 );
					double minImportance          = bag.GetDouble( "min_importance",   0.01 );
					bool branch                   = bag.GetBool(   "branch",           true );
					unsigned int temporal_samples = bag.GetUInt(   "temporal_samples", 100 );
					bool regenerate               = bag.GetBool(   "regenerate",       true );

					std::cout << "Queued Global Spectral Photons (will shoot at render time)" << std::endl;

					return pJob.ShootGlobalSpectralPhotons( photons, power_scale, maxRecur, minImportance, nmbegin, nmend, numWavelengths, branch, temporal_samples, regenerate );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "global_spectral_photonmap"; cd.category = ChunkCategory::PhotonMap;
						cd.description = "Global photon map generation (spectral).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddPhotonMapGenerateCommonParams( P );
						{ auto& p = P(); p.name = "nmbegin";         p.kind = ValueKind::Double; p.description = "Start wavelength (nm)"; p.defaultValueHint = "400"; }
						{ auto& p = P(); p.name = "nmend";           p.kind = ValueKind::Double; p.description = "End wavelength (nm)"; p.defaultValueHint = "700"; }
						{ auto& p = P(); p.name = "num_wavelengths"; p.kind = ValueKind::UInt;   p.description = "Spectral sample count"; p.defaultValueHint = "16"; }
						return cd;
					}();
					return d;
				}
			};

			struct TranslucentPelPhotonMapGenerateAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					unsigned int photons          = bag.GetUInt(   "num",                    10000 );
					unsigned int maxRecur         = bag.GetUInt(   "max_recursion",          10 );
					double minImportance          = bag.GetDouble( "min_importance",         0.01 );
					double power_scale            = bag.GetDouble( "power_scale",            1.0 );
					bool shootFromNonMeshLights   = bag.GetBool(   "shootFromNonMeshLights", true );
					bool shootFromMeshLights      = bag.GetBool(   "shootFromMeshLights",    true );
					bool reflect                  = bag.GetBool(   "reflect",                true );
					bool refract                  = bag.GetBool(   "refract",                true );
					bool direct_translucent       = bag.GetBool(   "direct_translucent",     true );
					unsigned int temporal_samples = bag.GetUInt(   "temporal_samples",       100 );
					bool regenerate               = bag.GetBool(   "regenerate",             true );

					std::cout << "Queued Translucent Pel Photons (will shoot at render time)" << std::endl;

					return pJob.ShootTranslucentPelPhotons( photons, power_scale, maxRecur, minImportance, reflect, refract, direct_translucent, shootFromNonMeshLights, temporal_samples, regenerate, shootFromMeshLights );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "translucent_pel_photonmap"; cd.category = ChunkCategory::PhotonMap;
						cd.description = "Translucent photon map generation (RGB).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddPhotonMapGenerateCommonParams( P );
						{ auto& p = P(); p.name = "direct_translucent"; p.kind = ValueKind::Bool; p.description = "Include direct translucency"; p.defaultValueHint = "TRUE"; }
						return cd;
					}();
					return d;
				}
			};

			struct ShadowPhotonMapGenerateAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					unsigned int photons          = bag.GetUInt( "num",              10000 );
					unsigned int temporal_samples = bag.GetUInt( "temporal_samples", 100 );
					bool regenerate               = bag.GetBool( "regenerate",       true );

					std::cout << "Queued Shadow Photons (will shoot at render time)" << std::endl;

					return pJob.ShootShadowPhotons( photons, temporal_samples, regenerate );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "shadow_photonmap"; cd.category = ChunkCategory::PhotonMap;
						cd.description = "Shadow photon map (direct visibility-cache).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "num";              p.kind = ValueKind::UInt; p.description = "Photon count"; p.defaultValueHint = "10000"; }
						{ auto& p = P(); p.name = "temporal_samples"; p.kind = ValueKind::UInt; p.description = "Temporal samples"; p.defaultValueHint = "1"; }
						{ auto& p = P(); p.name = "regenerate";       p.kind = ValueKind::Bool; p.description = "Regenerate per frame"; p.defaultValueHint = "FALSE"; }
						return cd;
					}();
					return d;
				}
			};

			struct GlobalPelPhotonMapGenerateAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					unsigned int photons             = bag.GetUInt(   "num",                    10000 );
					double power_scale               = bag.GetDouble( "power_scale",            1.0 );
					unsigned int maxRecur            = bag.GetUInt(   "max_recursion",          10 );
					double minImportance             = bag.GetDouble( "min_importance",         0.01 );
					bool branch                      = bag.GetBool(   "branch",                 true );
					bool shootFromNonMeshLights      = bag.GetBool(   "shootFromNonMeshLights", true );
					bool shootFromMeshLights         = bag.GetBool(   "shootFromMeshLights",    true );
					unsigned int temporal_samples    = bag.GetUInt(   "temporal_samples",       100 );
					bool regenerate                  = bag.GetBool(   "regenerate",             true );

					std::cout << "Queued Global Pel Photons (will shoot at render time)" << std::endl;

					return pJob.ShootGlobalPelPhotons( photons, power_scale, maxRecur, minImportance, branch, shootFromNonMeshLights, temporal_samples, regenerate, shootFromMeshLights );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "global_pel_photonmap"; cd.category = ChunkCategory::PhotonMap;
						cd.description = "Global photon map generation (RGB).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddPhotonMapGenerateCommonParams( P );
						return cd;
					}();
					return d;
				}
			};

			struct CausticPelPhotonMapGatherAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					double radius        = bag.GetDouble( "radius",        0.0 );
					double ellipse_ratio = bag.GetDouble( "ellipse_ratio", 0.05 );
					unsigned int min     = bag.GetUInt(   "min_photons",   8 );
					unsigned int max     = bag.GetUInt(   "max_photons",   150 );

					return pJob.SetCausticPelGatherParameters( radius, ellipse_ratio, min, max );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "caustic_pel_gather"; cd.category = ChunkCategory::PhotonGather;
						cd.description = "Caustic photon gather (RGB).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddPhotonMapGatherCommonParams( P );
						return cd;
					}();
					return d;
				}
			};

			struct CausticSpectralPhotonMapGatherAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					double radius        = bag.GetDouble( "radius",        0.0 );
					double ellipse_ratio = bag.GetDouble( "ellipse_ratio", 0.05 );
					double nm_range      = bag.GetDouble( "nm_range",      10.0 );
					unsigned int min     = bag.GetUInt(   "min_photons",   8 );
					unsigned int max     = bag.GetUInt(   "max_photons",   150 );

					return pJob.SetCausticSpectralGatherParameters( radius, ellipse_ratio, min, max, nm_range );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "caustic_spectral_gather"; cd.category = ChunkCategory::PhotonGather;
						cd.description = "Caustic photon gather (spectral).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddPhotonMapGatherCommonParams( P );
						{ auto& p = P(); p.name = "nm_range"; p.kind = ValueKind::Double; p.description = "Wavelength gather range (nm)"; p.defaultValueHint = "10"; }
						return cd;
					}();
					return d;
				}
			};

			struct GlobalSpectralPhotonMapGatherAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					double radius        = bag.GetDouble( "radius",        0.0 );
					double ellipse_ratio = bag.GetDouble( "ellipse_ratio", 0.05 );
					double nm_range      = bag.GetDouble( "nm_range",      10.0 );
					unsigned int min     = bag.GetUInt(   "min_photons",   8 );
					unsigned int max     = bag.GetUInt(   "max_photons",   150 );

					return pJob.SetGlobalSpectralGatherParameters( radius, ellipse_ratio, min, max, nm_range );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "global_spectral_gather"; cd.category = ChunkCategory::PhotonGather;
						cd.description = "Global photon gather (spectral).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddPhotonMapGatherCommonParams( P );
						{ auto& p = P(); p.name = "nm_range"; p.kind = ValueKind::Double; p.description = "Wavelength gather range (nm)"; p.defaultValueHint = "10"; }
						return cd;
					}();
					return d;
				}
			};

			struct TranslucentPelPhotonMapGatherAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					double radius        = bag.GetDouble( "radius",        0.0 );
					double ellipse_ratio = bag.GetDouble( "ellipse_ratio", 0.05 );
					unsigned int min     = bag.GetUInt(   "min_photons",   8 );
					unsigned int max     = bag.GetUInt(   "max_photons",   150 );

					return pJob.SetTranslucentPelGatherParameters( radius, ellipse_ratio, min, max );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "translucent_pel_gather"; cd.category = ChunkCategory::PhotonGather;
						cd.description = "Translucent photon gather (RGB).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddPhotonMapGatherCommonParams( P );
						return cd;
					}();
					return d;
				}
			};

			struct ShadowPhotonMapGatherAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					double radius        = bag.GetDouble( "radius",        0.0 );
					double ellipse_ratio = bag.GetDouble( "ellipse_ratio", 0.05 );
					unsigned int min     = bag.GetUInt(   "min_photons",   1 );
					unsigned int max     = bag.GetUInt(   "max_photons",   100 );

					return pJob.SetShadowGatherParameters( radius, ellipse_ratio, min, max );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "shadow_gather"; cd.category = ChunkCategory::PhotonGather;
						cd.description = "Shadow photon gather parameters.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddPhotonMapGatherCommonParams( P );
						return cd;
					}();
					return d;
				}
			};

			struct GlobalPelPhotonMapGatherAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					double radius        = bag.GetDouble( "radius",        0.0 );
					double ellipse_ratio = bag.GetDouble( "ellipse_ratio", 0.05 );
					unsigned int min     = bag.GetUInt(   "min_photons",   8 );
					unsigned int max     = bag.GetUInt(   "max_photons",   150 );

					return pJob.SetGlobalPelGatherParameters( radius, ellipse_ratio, min, max );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "global_pel_gather"; cd.category = ChunkCategory::PhotonGather;
						cd.description = "Global photon gather (RGB).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddPhotonMapGatherCommonParams( P );
						return cd;
					}();
					return d;
				}
			};

			struct IrradianceCacheAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					double tolerance              = bag.GetDouble( "tolerance",              0.1 );
					unsigned int size             = bag.GetUInt(   "size",                   100000 );
					double min_spacing            = bag.GetDouble( "min_spacing",            0.05 );
					double max_spacing            = bag.GetDouble( "max_spacing",            min_spacing * 100 );
					double query_threshold_scale  = bag.GetDouble( "query_threshold_scale",  0.5 );
					double neighbor_spacing_scale = bag.GetDouble( "neighbor_spacing_scale", 2.0 );

					return pJob.SetIrradianceCacheParameters( size, tolerance, min_spacing, max_spacing, query_threshold_scale, neighbor_spacing_scale );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "irradiance_cache"; cd.category = ChunkCategory::IrradianceCache;
						cd.description = "Global irradiance-cache parameters.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "size";                    p.kind = ValueKind::UInt;   p.description = "Cache capacity (entries)"; p.defaultValueHint = "100000"; }
						{ auto& p = P(); p.name = "tolerance";               p.kind = ValueKind::Double; p.description = "Reuse tolerance"; p.defaultValueHint = "0.2"; }
						{ auto& p = P(); p.name = "min_spacing";             p.kind = ValueKind::Double; p.description = "Min sample spacing"; p.defaultValueHint = "0.1"; }
						{ auto& p = P(); p.name = "max_spacing";             p.kind = ValueKind::Double; p.description = "Max sample spacing"; p.defaultValueHint = "10.0"; }
						{ auto& p = P(); p.name = "query_threshold_scale";   p.kind = ValueKind::Double; p.description = "Query-threshold scaling"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "neighbor_spacing_scale"; p.kind = ValueKind::Double; p.description = "Neighbor-spacing scaling"; p.defaultValueHint = "1.0"; }
						return cd;
					}();
					return d;
				}
			};

			struct KeyframeAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					// NOTE: legacy ParseChunk had two bugs preserved here for
					// backwards-compat: `element_type` set `param = element_type`
					// (never updating element_type), and AddKeyframe was called
					// with `element_type` for both element_type and element
					// arguments, so the `element` parameter is effectively ignored.
					std::string element_type   = "object";
					std::string param          = bag.GetString( "param", "none" );
					std::string value          = bag.GetString( "value", "none" );
					std::string interp         = bag.GetString( "interpolator", "none" );
					std::string interp_params  = bag.GetString( "interpolator_params", "none" );
					double      time           = bag.GetDouble( "time", 0 );

					// Legacy behavior: when element_type is provided, override `param` to "object".
					if( bag.Has("element_type") ) {
						param = element_type;
					}

					return pJob.AddKeyframe( element_type.c_str(), element_type.c_str(), param.c_str(), value.c_str(), time, interp=="none"?0:interp.c_str(), interp_params=="none"?0:interp_params.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "keyframe"; cd.category = ChunkCategory::Animation;
						cd.description = "Single keyframe for an element's parameter over time.";
						// APPEND-class derive: Finalize -> Job::AddKeyframe inserts one independent keyframe
						// into an element's timeline; a scene legitimately carries many unnamed `keyframe`
						// chunks (no `name` param exists) -- so unnamed multiplicity is legal, not a singleton.
						cd.unnamedRepeatable = true;
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						// Legacy chunk: `element` and `element_type` are declared below for
						// scene-authoring familiarity, but Finalize (see the NOTE at the top of
						// this struct's Finalize) never reads `element` and never propagates a
						// user-supplied `element_type` into the AddKeyframe call -- both of
						// AddKeyframe's targeting arguments are hardcoded to the literal string
						// "object", so this chunk ALWAYS attempts to animate an object whose
						// `name` is literally "object", regardless of what either field below is
						// set to.  It CANNOT target a camera (or light/geometry/painter) --
						// there is no reachable camera branch from this chunk.  Use `timeline`
						// for real element/param targeting, including camera-by-name (see its
						// own descriptor).
						{ auto& p = P(); p.name = "element";             p.kind = ValueKind::String; p.description = "Legacy field -- NOT read by Finalize; has no effect on which entity is animated (this chunk always targets an object literally named \"object\"). Use `timeline`'s `element` for real targeting."; }
						{ auto& p = P(); p.name = "element_type";        p.kind = ValueKind::Enum;   p.enumValues = {"object","camera","light"}; p.description = "Legacy field -- Finalize hardcodes the actual target type to \"object\" regardless of this value; the only observable effect of supplying it is a preserved legacy quirk that also forces `param` to \"object\". Use `timeline` for real element-type targeting (object/camera/light/geometry/painter)."; p.defaultValueHint = "object (fixed; not user-selectable in practice)"; }
						{ auto& p = P(); p.name = "param";               p.kind = ValueKind::String; p.description = "Parameter name (e.g. position, orientation, scale)"; }
						{ auto& p = P(); p.name = "value";               p.kind = ValueKind::String; p.description = "Value at this keyframe (whitespace-separated tokens)"; }
						{ auto& p = P(); p.name = "time";                p.kind = ValueKind::Double; p.description = "Time (seconds) of the keyframe"; }
						{ auto& p = P(); p.name = "interpolator";        p.kind = ValueKind::String; p.description = "Interpolator type"; p.defaultValueHint = "linear"; }
						{ auto& p = P(); p.name = "interpolator_params"; p.kind = ValueKind::String; p.description = "Interpolator parameters"; }
						return cd;
					}();
					return d;
				}
			};

			struct TimelineAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string element_type = bag.GetString( "element_type", "object" );
					std::string element      = bag.GetString( "element",      "none" );
					std::string param        = bag.GetString( "param",        "none" );
					std::string animation    = bag.GetString( "animation",    "" );

					// `time`, `value`, `interpolator`, `interpolator_params` are
					// declared repeatable and zipped here.  Each `value`
					// appearance emits one keyframe.  `time` is read positionally
					// (time[i] applies to value[i]); `interpolator` and
					// `interpolator_params` are sticky — the last seen up to
					// position i applies to value[i], matching the legacy
					// in-order parser's update-then-emit behavior.
					const std::vector<std::string>& values  = bag.GetRepeatable( "value" );
					const std::vector<std::string>& times   = bag.GetRepeatable( "time" );
					const std::vector<std::string>& interps = bag.GetRepeatable( "interpolator" );
					const std::vector<std::string>& iparams = bag.GetRepeatable( "interpolator_params" );

					for( size_t i = 0; i < values.size(); ++i ) {
						double time = (i < times.size()) ? RISE::String(times[i].c_str()).toDouble() : 0.0;
						const char* interp_c  = interps.empty() ? 0 :
							(interps[std::min(i, interps.size()-1)] == "none" ? 0 : interps[std::min(i, interps.size()-1)].c_str());
						const char* iparams_c = iparams.empty() ? 0 :
							(iparams[std::min(i, iparams.size()-1)] == "none" ? 0 : iparams[std::min(i, iparams.size()-1)].c_str());

						if( !pJob.AddKeyframeToAnimation( element_type.c_str(), element.c_str(), param.c_str(), values[i].c_str(), time, interp_c, iparams_c, animation.c_str() ) ) {
							return false;
						}
					}

					return true;
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "timeline"; cd.category = ChunkCategory::Animation;
						cd.description = "Sequence of keyframes for one element/parameter.";
						// APPEND-class derive: Finalize -> Job::AddKeyframeToAnimation appends keyframes for one
						// element/param; a scene legitimately carries many unnamed `timeline` chunks (no `name`
						// param exists), one per animated element/param -- so unnamed multiplicity is legal, not
						// a singleton (e.g. scenes/FeatureBased/SDF/sdf_morph_torture.RISEscene carries ~14).
						cd.unnamedRepeatable = true;
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						// The camera clause below is LOAD-BEARING agent guidance: for element_type
						// camera, `element` names the TARGET camera; omit it to animate the active
						// camera (the common single-, often unnamed-, camera case needs no rename).
						// Naming a camera that does not exist is a LOUD derive failure, so the agent
						// must reference a real camera `name` (or leave `element` off) rather than
						// inventing one.
						{ auto& p = P(); p.name = "element";             p.kind = ValueKind::String; p.description = "Element name; for element_type camera this is the target camera's `name` (empty == the active camera; a name that matches no camera fails the derive)"; }
						{ auto& p = P(); p.name = "element_type";        p.kind = ValueKind::Enum;   p.enumValues = {"object","camera","light","geometry","painter"}; p.description = "Element kind (camera animates the `element`-named camera, or the ACTIVE camera when `element` is empty; geometry/painter animate a named geometry's or painter's intrinsic params, e.g. an sdf_geometry's part fields)"; p.defaultValueHint = "object"; }
						{ auto& p = P(); p.name = "param";               p.kind = ValueKind::String; p.description = "Parameter name"; }
						{ auto& p = P(); p.name = "animation";           p.kind = ValueKind::String; p.description = "Owning named animation (default = the implicit default animation)"; p.defaultValueHint = "(default)"; }
						{ auto& p = P(); p.name = "value";               p.kind = ValueKind::String; p.repeatable = true; p.description = "Value at the corresponding `time` (emits one keyframe per appearance)"; }
						{ auto& p = P(); p.name = "time";                p.kind = ValueKind::Double; p.repeatable = true; p.description = "Time of the matching value (positional, paired 1:1 with `value`)"; }
						{ auto& p = P(); p.name = "interpolator";        p.kind = ValueKind::String; p.repeatable = true; p.description = "Interpolator type (sticky — last-seen up to a given `value` applies)"; p.defaultValueHint = "linear"; }
						{ auto& p = P(); p.name = "interpolator_params"; p.kind = ValueKind::String; p.repeatable = true; p.description = "Interpolator parameters (sticky)"; }
						return cd;
					}();
					return d;
				}
			};

			struct AnimationOptionsAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					double       time_start    = bag.GetDouble( "time_start",    0 );
					double       time_end      = bag.GetDouble( "time_end",      1.0 );
					unsigned int num_frames    = bag.GetUInt(   "num_frames",    30 );
					bool         do_fields     = bag.GetBool(   "do_fields",     false );
					bool         invert_fields = bag.GetBool(   "invert_fields", false );

					return pJob.SetAnimationOptions( time_start, time_end, num_frames, do_fields, invert_fields );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "animation_options"; cd.category = ChunkCategory::Animation;
						cd.description = "Global animation time range, frame count, and field options.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "time_start";    p.kind = ValueKind::Double; p.description = "Animation start time"; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "time_end";      p.kind = ValueKind::Double; p.description = "Animation end time"; p.defaultValueHint = "1"; }
						{ auto& p = P(); p.name = "num_frames";    p.kind = ValueKind::UInt;   p.description = "Number of frames to render"; p.defaultValueHint = "30"; }
						{ auto& p = P(); p.name = "do_fields";     p.kind = ValueKind::Bool;   p.description = "Emit interlaced fields"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "invert_fields"; p.kind = ValueKind::Bool;   p.description = "Invert field order"; p.defaultValueHint = "FALSE"; }
						return cd;
					}();
					return d;
				}
			};

			struct SceneVariantAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "" );
					std::string cam  = bag.GetString( "active_camera", "" );
					return pJob.DeclareSceneVariant( name.c_str(), cam.c_str() );   // empty name -> false
				}
				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "scene_variant"; cd.category = ChunkCategory::SceneVariant;
						cd.description = "Declares a named, selectable scene variant (an overlay).  variant-tagged chunks override their base when this variant is active; active_camera selects a camera.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";          p.kind = ValueKind::String;    p.description = "Variant name (REQUIRED; the UI handle / switch key)"; }
						{ auto& p = P(); p.name = "active_camera"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Camera}; p.description = "Camera to activate when this variant is active"; }
						return cd;
					}();
					return d;
				}
			};

			struct ActiveSceneVariantAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name = bag.GetString( "name", "" );
					return pJob.SetActiveSceneVariant( name.c_str() );
				}
				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "active_scene_variant"; cd.category = ChunkCategory::SceneVariant;
						cd.description = "Selects the active scene variant by name (empty / none / absent => the base default).  Single-select.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name"; p.kind = ValueKind::String; p.description = "Active variant name (empty / none => base default)"; }
						return cd;
					}();
					return d;
				}
			};

			struct AnimationAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string  name          = bag.GetString( "name",          "(default)" );
					double       time_start    = bag.GetDouble( "time_start",    0 );
					double       time_end      = bag.GetDouble( "time_end",      1.0 );
					unsigned int num_frames    = bag.GetUInt(   "num_frames",    30 );
					bool         do_fields     = bag.GetBool(   "do_fields",     false );
					bool         invert_fields = bag.GetBool(   "invert_fields", false );
					bool         active        = bag.GetBool(   "active",        false );

					return pJob.DeclareAnimation( name.c_str(), time_start, time_end, num_frames, do_fields, invert_fields, active );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "animation"; cd.category = ChunkCategory::Animation;
						cd.description = "Declares a named animation path (a group of timelines sharing a name); each `timeline` joins it via its `animation <name>` tag.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";          p.kind = ValueKind::String; p.description = "Animation name (referenced by timeline `animation` tags)"; p.defaultValueHint = "(default)"; }
						{ auto& p = P(); p.name = "active";        p.kind = ValueKind::Bool;   p.description = "Make this the active animation (rendered/scrubbed by default)"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "time_start";    p.kind = ValueKind::Double; p.description = "Animation start time"; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "time_end";      p.kind = ValueKind::Double; p.description = "Animation end time"; p.defaultValueHint = "1"; }
						{ auto& p = P(); p.name = "num_frames";    p.kind = ValueKind::UInt;   p.description = "Number of frames to render"; p.defaultValueHint = "30"; }
						{ auto& p = P(); p.name = "do_fields";     p.kind = ValueKind::Bool;   p.description = "Emit interlaced fields"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "invert_fields"; p.kind = ValueKind::Bool;   p.description = "Invert field order"; p.defaultValueHint = "FALSE"; }
						return cd;
					}();
					return d;
				}
			};
		}
	}

	// Factory that creates one instance of every chunk parser the scene
	// grammar supports.  Ownership transfers to the caller; when the
	// returned vector goes out of scope all parsers are destroyed.  The
	// same list powers AsciiSceneParser's dispatch map and
	// SceneEditorSuggestions' grammar enumeration.
	std::vector<ChunkParserEntry> CreateAllChunkParsers()
	{
		using namespace Implementation::ChunkParsers;
		std::vector<ChunkParserEntry> entries;
		entries.reserve( 128 );

		auto add = [&entries]( const char* keyword, IAsciiChunkParser* parser ) {
			ChunkParserEntry e;
			e.keyword = keyword;
			e.parser.reset( parser );
			entries.push_back( std::move(e) );
		};

		// Painters
		add( "uniformcolor_painter",                  new UniformColorPainterAsciiChunkParser() );
		add( "vertex_color_painter",                  new VertexColorPainterAsciiChunkParser() );
		add( "spectral_painter",                      new SpectralPainterAsciiChunkParser() );
		add( "scalar_painter",                        new ScalarPainterAsciiChunkParser() );
		add( "png_painter",                           new PngPainterAsciiChunkParser() );
		add( "jpg_painter",                           new JpegPainterAsciiChunkParser() );
		add( "hdr_painter",                           new HdrPainterAsciiChunkParser() );
		add( "exr_painter",                           new ExrPainterAsciiChunkParser() );
		add( "tiff_painter",                          new TiffPainterAsciiChunkParser() );
		add( "checker_painter",                       new CheckerPainterAsciiChunkParser() );
		add( "lines_painter",                         new LinesPainterAsciiChunkParser() );
		add( "mandelbrot_painter",                    new MandelbrotPainterAsciiChunkParser() );
		add( "perlin2d_painter",                      new Perlin2DPainterAsciiChunkParser() );
		add( "controlled_smoothness2d_painter",       new ControlledSmoothness2DPainterAsciiChunkParser() );
		add( "polynomial_function2d_painter",         new PolynomialFunction2DPainterAsciiChunkParser() );
		add( "composite_function2d_painter",          new CompositeFunction2DPainterAsciiChunkParser() );
		add( "gerstnerwave_painter",                  new GerstnerWavePainterAsciiChunkParser() );
		add( "perlin3d_painter",                      new Perlin3DPainterAsciiChunkParser() );
		add( "turbulence3d_painter",                  new Turbulence3DPainterAsciiChunkParser() );
		add( "wavelet3d_painter",                     new Wavelet3DPainterAsciiChunkParser() );
		add( "reactiondiffusion3d_painter",           new ReactionDiffusion3DPainterAsciiChunkParser() );
		add( "gabor3d_painter",                       new Gabor3DPainterAsciiChunkParser() );
		add( "simplex3d_painter",                     new Simplex3DPainterAsciiChunkParser() );
		add( "sdf3d_painter",                         new SDF3DPainterAsciiChunkParser() );
		add( "curlnoise3d_painter",                   new CurlNoise3DPainterAsciiChunkParser() );
		add( "domainwarp3d_painter",                  new DomainWarp3DPainterAsciiChunkParser() );
		add( "perlinworley3d_painter",                new PerlinWorley3DPainterAsciiChunkParser() );
		add( "worley3d_painter",                      new Worley3DPainterAsciiChunkParser() );
		add( "voronoi2d_painter",                     new Voronoi2DPainterAsciiChunkParser() );
		add( "voronoi3d_painter",                     new Voronoi3DPainterAsciiChunkParser() );
		add( "iridescent_painter",                    new IridescentPainterAsciiChunkParser() );
		add( "blackbody_painter",                     new BlackBodyPainterAsciiChunkParser() );
		add( "blend_painter",                         new BlendPainterAsciiChunkParser() );
		add( "function2d_painter",                    new Function2DColorPainterAsciiChunkParser() );
		add( "expression_function2d",                 new ExpressionFunction2DPainterAsciiChunkParser() );
		add( "expression_painter",                    new ExpressionPainterAsciiChunkParser() );
		add( "ramp_painter",                          new RampPainterAsciiChunkParser() );
		add( "mapping_painter",                       new MappingPainterAsciiChunkParser() );
		add( "stochastic_tile_painter",               new StochasticTilePainterAsciiChunkParser() );
		add( "scatter_painter",                       new ScatterPainterAsciiChunkParser() );
		add( "channel_painter",                       new ChannelPainterAsciiChunkParser() );

		// Functions
		add( "piecewise_linear_function",             new PiecewiseLinearFunctionChunkParser() );
		add( "piecewise_linear_function2d",           new PiecewiseLinearFunction2DChunkParser() );

		// Materials
		add( "lambertian_material",                   new LambertianMaterialAsciiChunkParser() );
		add( "perfectreflector_material",             new PerfectReflectorMaterialAsciiChunkParser() );
		add( "perfectrefractor_material",             new PerfectRefractorMaterialAsciiChunkParser() );
		add( "polished_material",                     new PolishedMaterialAsciiChunkParser() );
		add( "dielectric_material",                   new DielectricMaterialAsciiChunkParser() );
		add( "subsurfacescattering_material",         new SubSurfaceScatteringMaterialAsciiChunkParser() );
		add( "randomwalk_sss_material",               new RandomWalkSSSMaterialAsciiChunkParser() );
		add( "lambertian_luminaire_material",         new LambertianLuminaireMaterialAsciiChunkParser() );
		add( "phong_luminaire_material",              new PhongLuminaireMaterialAsciiChunkParser() );
		add( "ashikminshirley_anisotropicphong_material", new AshikminShirleyAnisotropicPhongMaterialAsciiChunkParser() );
		add( "isotropic_phong_material",              new IsotropicPhongMaterialAsciiChunkParser() );
		add( "translucent_material",                  new TranslucentMaterialAsciiChunkParser() );
		add( "biospec_skin_material",                 new BioSpecSkinMaterialAsciiChunkParser() );
		add( "donner_jensen_skin_bssrdf_material",    new DonnerJensenSkinBSSRDFMaterialAsciiChunkParser() );
		add( "generic_human_tissue_material",         new GenericHumanTissueMaterialAsciiChunkParser() );
		add( "coated_material",                       new CoatedMaterialAsciiChunkParser() );
		add( "fabric_material",                       new FabricMaterialAsciiChunkParser() );
		add( "weave_material",                        new WeaveMaterialAsciiChunkParser() );
		add( "composite_material",                    new CompositeMaterialAsciiChunkParser() );
		add( "ward_isotropic_material",               new WardIsotropicGaussianMaterialAsciiChunkParser() );
		add( "ward_anisotropic_material",             new WardAnisotropicEllipticalGaussianMaterialAsciiChunkParser() );
		add( "ggx_material",                          new GGXMaterialAsciiChunkParser() );
		add( "pbr_metallic_roughness_material",       new PBRMetallicRoughnessMaterialAsciiChunkParser() );
		add( "cooktorrance_material",                 new CookTorranceMaterialAsciiChunkParser() );
		add( "orennayar_material",                    new OrenNayarMaterialAsciiChunkParser() );
		add( "sheen_material",                        new SheenMaterialAsciiChunkParser() );
		add( "schlick_material",                      new SchlickMaterialAsciiChunkParser() );
		add( "datadriven_material",                   new DataDrivenMaterialAsciiChunkParser() );
		add( "hair_material",                         new HairMaterialAsciiChunkParser() );

		// Cameras
		add( "scene_options",                         new SceneOptionsAsciiChunkParser() );
		add( "camera_defaults",                       new CameraDefaultsAsciiChunkParser() );
		// Film — pixel-grid descriptor.  See FilmAsciiChunkParser
		// definition for the contract; the chunk should be authored
		// BEFORE any camera chunk in the scene file.
		add( "film",                                  new FilmAsciiChunkParser() );

		add( "pinhole_camera",                        new PinholeCameraAsciiChunkParser() );
		add( "onb_pinhole_camera",                    new ONBPinholeCameraAsciiChunkParser() );
		add( "thinlens_camera",                       new ThinlensCameraAsciiChunkParser() );
		// realistic_camera reserved for Phase 4 (multi-element lens).
		add( "fisheye_camera",                        new FisheyeCameraAsciiChunkParser() );
		add( "orthographic_camera",                   new OrthographicCameraAsciiChunkParser() );

		// Geometry
		add( "sphere_geometry",                       new SphereGeometryAsciiChunkParser() );
		add( "ellipsoid_geometry",                    new EllipsoidGeometryAsciiChunkParser() );
		add( "cylinder_geometry",                     new CylinderGeometryAsciiChunkParser() );
		add( "torus_geometry",                        new TorusGeometryAsciiChunkParser() );
		add( "infiniteplane_geometry",                new InfinitePlaneGeometryAsciiChunkParser() );
		add( "box_geometry",                          new BoxGeometryAsciiChunkParser() );
		add( "clippedplane_geometry",                 new ClippedPlaneGeometryAsciiChunkParser() );
		add( "3dsmesh_geometry",                      new Mesh3DSGeometryAsciiChunkParser() );
		add( "rawmesh_geometry",                      new RAWMeshGeometryAsciiChunkParser() );
		add( "rawmesh2_geometry",                     new RAWMesh2GeometryAsciiChunkParser() );
		add( "risemesh_geometry",                     new RISEMeshGeometryAsciiChunkParser() );
		add( "plymesh_geometry",                      new PLYMeshGeometryAsciiChunkParser() );
		add( "gltfmesh_geometry",                     new GLTFMeshGeometryAsciiChunkParser() );
		add( "gltf_import",                           new GLTFImportAsciiChunkParser() );
		add( "circulardisk_geometry",                 new CircularDiskGeometryAsciiChunkParser() );
		add( "bezierpatch_geometry",                  new BezierPatchGeometryAsciiChunkParser() );
		add( "bilinearpatch_geometry",                new BilinearPatchGeometryAsciiChunkParser() );
		add( "sdf_geometry",                          new SDFGeometryAsciiChunkParser() );
		add( "skeleton_geometry",                     new SkeletonGeometryAsciiChunkParser() );
		add( "cartesian_disk_geometry",               new CartesianDiskGeometryAsciiChunkParser() );
		add( "sweep_geometry",                        new SweepGeometryAsciiChunkParser() );
		add( "lathe_geometry",                        new LatheGeometryAsciiChunkParser() );
		add( "skin_geometry",                         new SkinGeometryAsciiChunkParser() );
		add( "path_instances_geometry",               new PathInstancesGeometryAsciiChunkParser() );
		add( "displaced_geometry",                    new DisplacedGeometryAsciiChunkParser() );
		add( "hair_geometry",                         new HairGeometryAsciiChunkParser() );
		add( "hair_guides",                           new HairGuidesAsciiChunkParser() );

		// Modifiers
		add( "relief_modifier",                       new ReliefModifierAsciiChunkParser() );
		add( "modifier_stack",                        new ModifierStackAsciiChunkParser() );
		add( "normal_map_modifier",                   new NormalMapModifierAsciiChunkParser() );
		add( "glint_modifier",                        new GlintModifierAsciiChunkParser() );

		// Media
		add( "homogeneous_medium",                    new HomogeneousMediumAsciiChunkParser() );
		add( "global_medium",                         new GlobalMediumAsciiChunkParser() );
		add( "light_rr_threshold",                   new LightRRThresholdAsciiChunkParser() );
		add( "heterogeneous_medium",                  new HeterogeneousMediumAsciiChunkParser() );
		add( "painter_heterogeneous_medium",          new PainterHeterogeneousMediumAsciiChunkParser() );

		// Objects
		add( "standard_object",                       new StandardObjectAsciiChunkParser() );
		add( "override_object",                       new OverrideObjectAsciiChunkParser() );
		add( "csg_object",                            new CSGObjectAsciiChunkParser() );

		// Shader ops
		add( "ambientocclusion_shaderop",             new AmbientOcclusionShaderOpAsciiChunkParser() );
		add( "directlighting_shaderop",               new DirectLightingShaderOpAsciiChunkParser() );
		add( "pathtracing_shaderop",                  new PathTracingShaderOpAsciiChunkParser() );
		add( "mis_pathtracing_shaderop",              new PathTracingShaderOpAsciiChunkParser() );  // Legacy alias
		add( "sms_shaderop",                          new SMSShaderOpAsciiChunkParser() );
		add( "distributiontracing_shaderop",          new DistributionTracingShaderOpAsciiChunkParser() );
		add( "finalgather_shaderop",                  new FinalGatherShaderOpAsciiChunkParser() );
		add( "simple_sss_shaderop",                   new SimpleSubSurfaceScatteringShaderOpAsciiChunkParser() );
		add( "diffusion_approximation_sss_shaderop",  new DiffusionApproximationSubSurfaceScatteringShaderOpAsciiChunkParser() );
		add( "donner_jensen_skin_sss_shaderop",       new DonnerJensenSkinSSSShaderOpAsciiChunkParser() );
		add( "arealight_shaderop",                    new AreaLightShaderOpAsciiChunkParser() );
		add( "transparency_shaderop",                 new TransparencyShaderOpAsciiChunkParser() );
		add( "alpha_test_shaderop",                   new AlphaTestShaderOpAsciiChunkParser() );

		// Shaders
		add( "standard_shader",                       new StandardShaderAsciiChunkParser() );
		add( "advanced_shader",                       new AdvancedShaderAsciiChunkParser() );
		add( "directvolumerendering_shader",          new DirectVolumeRenderingShaderAsciiChunkParser() );
		add( "spectraldirectvolumerendering_shader",  new SpectralDirectVolumeRenderingShaderAsciiChunkParser() );

		// Rasterizers
		add( "pixelpel_rasterizer",                   new PixelPelRasterizerAsciiChunkParser() );
		add( "pixelintegratingspectral_rasterizer",   new PixelIntegratingSpectralRasterizerAsciiChunkParser() );
		add( "bdpt_pel_rasterizer",                   new BDPTPelRasterizerAsciiChunkParser() );
		add( "bdpt_spectral_rasterizer",              new BDPTSpectralRasterizerAsciiChunkParser() );
		add( "vcm_pel_rasterizer",                    new VCMPelRasterizerAsciiChunkParser() );
		add( "vcm_spectral_rasterizer",               new VCMSpectralRasterizerAsciiChunkParser() );
		add( "pathtracing_pel_rasterizer",            new PathTracingPelRasterizerAsciiChunkParser() );
		add( "pathtracing_spectral_rasterizer",       new PathTracingSpectralRasterizerAsciiChunkParser() );
		add( "auto_rasterizer",                       new AutoRasterizerAsciiChunkParser() );
		add( "auto_spectral_rasterizer",              new AutoSpectralRasterizerAsciiChunkParser() );
		add( "mlt_rasterizer",                        new MLTRasterizerAsciiChunkParser() );
		add( "mlt_spectral_rasterizer",               new MLTSpectralRasterizerAsciiChunkParser() );

		// Rasterizer output
		add( "file_rasterizeroutput",                 new FileRasterizerOutputAsciiChunkParser() );

		// Lights
		add( "ambient_light",                         new AmbientLightAsciiChunkParser() );
		add( "omni_light",                            new OmniLightAsciiChunkParser() );
		add( "spot_light",                            new SpotLightAsciiChunkParser() );
		add( "directional_light",                     new DirectionalLightAsciiChunkParser() );
		add( "hosek_wilkie_skylight",                 new HosekWilkieSkylightAsciiChunkParser() );
		add( "rect_light",                            new RectLightAsciiChunkParser() );
		add( "shape_light",                           new ShapeLightAsciiChunkParser() );

		// Photon maps & gather
		add( "caustic_pel_photonmap",                 new CausticPelPhotonMapGenerateAsciiChunkParser() );
		add( "translucent_pel_photonmap",             new TranslucentPelPhotonMapGenerateAsciiChunkParser() );
		add( "caustic_spectral_photonmap",            new CausticSpectralPhotonMapGenerateAsciiChunkParser() );
		add( "global_pel_photonmap",                  new GlobalPelPhotonMapGenerateAsciiChunkParser() );
		add( "global_spectral_photonmap",             new GlobalSpectralPhotonMapGenerateAsciiChunkParser() );
		add( "shadow_photonmap",                      new ShadowPhotonMapGenerateAsciiChunkParser() );
		add( "caustic_pel_gather",                    new CausticPelPhotonMapGatherAsciiChunkParser() );
		add( "translucent_pel_gather",                new TranslucentPelPhotonMapGatherAsciiChunkParser() );
		add( "caustic_spectral_gather",               new CausticSpectralPhotonMapGatherAsciiChunkParser() );
		add( "global_pel_gather",                     new GlobalPelPhotonMapGatherAsciiChunkParser() );
		add( "global_spectral_gather",                new GlobalSpectralPhotonMapGatherAsciiChunkParser() );
		add( "shadow_gather",                         new ShadowPhotonMapGatherAsciiChunkParser() );

		// Irradiance cache
		add( "irradiance_cache",                      new IrradianceCacheAsciiChunkParser() );

		// Animation
		add( "keyframe",                              new KeyframeAsciiChunkParser() );
		add( "timeline",                              new TimelineAsciiChunkParser() );
		add( "animation_options",                     new AnimationOptionsAsciiChunkParser() );
		add( "animation",                             new AnimationAsciiChunkParser() );
		add( "scene_variant",                         new SceneVariantAsciiChunkParser() );
		add( "active_scene_variant",                  new ActiveSceneVariantAsciiChunkParser() );

		return entries;
	}

	//////////////////////////////////////////////////
	// IAsciiChunkParser default implementations
	//////////////////////////////////////////////////

	// Default ParseChunk dispatches via the chunk's descriptor: every
	// input line is validated against Describe().parameters, matched
	// values are stored in a ParseStateBag, and Finalize() is invoked
	// to emit the pJob.AddX call.  This is the single source of truth
	// — a parameter that is not in the descriptor cannot be parsed,
	// and a parameter that is in the descriptor flows automatically
	// to Finalize().  Every chunk parser overrides only Finalize;
	// none overrides ParseChunk.  This means the descriptor IS the
	// parser's accepted-parameter set — drift between "what the
	// parser parses" and "what the descriptor advertises" is
	// structurally impossible.
	bool IAsciiChunkParser::ParseChunk( const ParamsList& in, IJob& pJob ) const
	{
		ParseStateBag bag( &Describe() );
		if( !Implementation::ChunkParsers::DispatchChunkParameters( Describe(), bag, in ) ) {
			return false;
		}
		// scene_variant is CST-native (doc 63): the legacy streaming reader can't pre-scan for the active variant,
		// so it renders the BASE -- skip a variant-tagged material's registration (else it dup-name-collides with
		// the base material).  `variant none` is the no-variant sentinel (cf. `material none`) -- an ordinary base
		// material, so it is NOT skipped.  The CST DeriveToJob bakes real variants properly.  Gate the `variant`
		// lookup behind the material category: GetString on a chunk whose descriptor does not declare `variant`
		// trips ValidateAccess's "ChunkParser Bug" diagnostic, which would otherwise fire for every non-material
		// chunk on this (default) legacy path.
		if( Describe().category == ChunkCategory::Material ) {
			const std::string variantTag = bag.GetString( "variant", "" );
			if( !variantTag.empty() && variantTag != "none" ) {
				return true;
			}
		}
		return Finalize( bag, pJob );
	}

	// Public wrapper (declared in IAsciiChunkParser.h) over the inline
	// Implementation::ChunkParsers::DispatchChunkParameters, so the CST derive
	// path can validate+populate a bag through the SAME live validation the
	// default ParseChunk dispatch uses, then Finalize() separately (refuse-all
	// boundary).
	bool DispatchChunkParameters( const ChunkDescriptor& desc, ParseStateBag& bag, const IAsciiChunkParser::ParamsList& params )
	{
		return Implementation::ChunkParsers::DispatchChunkParameters( desc, bag, params );
	}

	// Public wrapper (declared in IAsciiChunkParser.h) over the per-parse
	// state reset, so the CST derive path can reset the chunk parsers' cross-
	// chunk caches before deriving -- the same reset the legacy ParseAndLoadScene
	// performed at its start (streaming loader deleted, Slice 6c) -- preventing
	// parse state from leaking between successive derives.  Sole live caller:
	// Cst::DeriveToJob (Cst/Cst.cpp), always with the default `true`.  The
	// `resetTopLevelState` argument forwards to the private ClearParseState;
	// `false` was the deleted streaming loader's recursive `> load` / `> run`
	// mode (keep the camera-name dedup across the nested parse) and is
	// currently unused (no callers pass it); retained pending deletion.
	void ClearChunkParserState( bool resetTopLevelState )
	{
		Implementation::ChunkParsers::ClearParseState( resetTopLevelState );
	}

	// Public wrapper (declared in IAsciiChunkParser.h) over the private
	// ChunkParsers::LastAllocatedCameraName.  HISTORICAL: existed so the
	// streaming loader (a separate translation unit, deleted Slice 6c) could
	// read the camera name the most recent camera-chunk Finalize allocated.
	// Currently unused (no callers); retained pending deletion.
	const std::string& LastAllocatedCameraName()
	{
		return Implementation::ChunkParsers::LastAllocatedCameraName();
	}
}
