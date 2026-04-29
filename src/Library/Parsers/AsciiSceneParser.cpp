//////////////////////////////////////////////////////////////////////
//
//  AsciiSceneParser.cpp - Implementation of the AsciiSceneParser
//    class plus every concrete IAsciiChunkParser subclass.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 22, 2003
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
//
//  ARCHITECTURE — descriptor-driven chunk parsing (April 2026)
//
//  Every chunk parser in this file derives from `IAsciiChunkParser`
//  and overrides exactly TWO virtual methods:
//
//    1. `Describe()` returns a `ChunkDescriptor` enumerating every
//       parameter the chunk accepts (name, kind, enum values,
//       reference categories, defaults, descriptions).  This
//       descriptor IS the parser's accepted-parameter set.
//
//    2. `Finalize(const ParseStateBag& bag, IJob& pJob)` reads
//       typed values out of the bag and emits the corresponding
//       `pJob.AddX(...)` / `pJob.SetX(...)` call.
//
//  No chunk parser overrides `ParseChunk` directly.  The default
//  `ParseChunk` impl (defined below, just after `CreateAllChunkParsers`)
//  walks the input lines, validates each name against
//  `Describe().parameters`, stores matched values in a `ParseStateBag`,
//  then invokes `Finalize` to emit the AddX call.  An input parameter
//  whose name is not in the descriptor fails the parse — exactly the
//  same behaviour as the legacy hand-rolled `if/else` chain's else
//  branch, but driven from a single source of truth.
//
//  THE INVARIANT this enforces:  drift between "what the parser
//  parses" and "what the descriptor advertises" is structurally
//  impossible.  The same descriptor feeds:
//    - the parser (via `DispatchChunkParameters` below)
//    - the syntax highlighters (Qt + AppKit, via `SceneGrammar`)
//    - the scene-editor suggestion engine (right-click menu and
//      inline autocomplete in both GUI apps)
//    - any future grammar consumer (linters, doc generators, …).
//
//  Adding a new chunk parser:
//
//    1. Define `struct YourAsciiChunkParser : public IAsciiChunkParser`
//       inside the `RISE::Implementation::ChunkParsers` namespace,
//       with `Describe()` (static `ChunkDescriptor` constructed via the
//       `auto P = [&cd]() -> ParameterDescriptor& { ... }` lambda
//       idiom) and `Finalize(const ParseStateBag&, IJob&) const override`.
//    2. Register it in `CreateAllChunkParsers()` further down in this
//       file — `add("your_chunk_keyword", new YourAsciiChunkParser());`.
//    3. The Library build will fail until both `Describe()` AND
//       `Finalize()` are implemented (both are pure-virtual on
//       `IAsciiChunkParser`).
//    4. The chunk now appears automatically in:
//          - syntax highlighting on Windows + macOS
//          - the right-click context menu and inline autocomplete
//          - any future grammar consumer.
//       No second site to update.
//
//  Adding a parameter to an existing chunk:
//
//    1. Add one entry to that chunk's `Describe()` parameter list:
//          { auto& p = P(); p.name = "..."; p.kind = ValueKind::...;
//            p.description = "..."; p.defaultValueHint = "..."; }
//       (set `p.repeatable = true` for repeatable params, populate
//       `p.enumValues` for `ValueKind::Enum`, populate
//       `p.referenceCategories` for `ValueKind::Reference`).
//    2. Read the new value in `Finalize`:
//          double x = bag.GetDouble("...", default_value);
//       (or `GetString` / `GetUInt` / `GetBool` / `GetVec3` /
//       `GetRepeatable` as appropriate.)  Pass the same default the
//       legacy code used as the local-variable initial value.
//    3. Pass it to the appropriate `pJob.AddX(...)` / `pJob.SetX(...)`
//       overload.  No third site to update.
//
//  Removing a parameter from an existing chunk:
//
//    1. Delete the descriptor entry.  Done.  Every consumer updates
//       in lock-step.  If scene-file backwards compatibility is
//       required, leave the entry but mark it "Legacy — ignored" in
//       the description and skip reading it in `Finalize`.
//
//  Helper templates that bundle parameter sets shared across many
//  chunks (`AddCameraCommonParams`, `AddSpectralCoreParams`,
//  `AddPhotonMapGenerateCommonParams`, …) live in this file just
//  after `DispatchChunkParameters` and just before the painter
//  parsers; reuse them rather than copy-pasting parameter lists.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include <vector>
#include <map>
#include <stack>
#include <string>
#include <sstream>
#include <algorithm>
#include <sys/types.h>
#include <sys/stat.h>
#include "AsciiSceneParser.h"
#include "AsciiCommandParser.h"
#include "IAsciiChunkParser.h"
#include "ChunkParserRegistry.h"
#include "StdOutProgress.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/OrthonormalBasis3D.h"
#include "../Utilities/MediaPathLocator.h"
#include "../Sampling/HaltonPoints.h"
#include "MathExpressionEvaluator.h"

#ifdef WIN32
#include <malloc.h>
#else
#include <alloca.h>
#endif

using namespace RISE;
using namespace RISE::Implementation;

#define MAX_CHARS_PER_LINE		8192
#define CURRENT_SCENE_VERSION	5

// std_string_npos no longer needed - using std::string::npos directly
static MultiHalton mh;

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

inline void make_string_from_tokens( String& s, String* tokens, const unsigned int num_tokens, const char* ch )
{
	// Concatenate all the tokens together with the given character between each
	// of the tokens

	if( num_tokens < 1 ) {
		return;
	}

	s.clear();
	s.concatenate(tokens[0]);

	for( unsigned int i=1; i<num_tokens; i++ ) {
		s.concatenate( ch );
		s.concatenate( tokens[i] );
	}
}

inline char evaluate_first_function_in_expression( String& token )
{
	std::string str( token.c_str() );
	std::string processed;

	std::string::size_type x = str.find_first_of( "scth" );

	if( x == std::string::npos ) {
		return 0;
	}

	if( x > 0 ) {
		processed = str.substr( 0, x );
		str = str.substr( x, str.size()-1 );
	}

	x = str.find_first_of( "(" );
	if( x == std::string::npos ) {
		return 2;
	}

	std::string::size_type y = str.find_first_of( ")" );
	if( y == std::string::npos ) {
		return 2;
	}

	// Take the expression from to y and evaluate it
	std::string szexpr = str.substr( x, y-x+1 );

	MathExpressionEvaluator::Expression expr( szexpr.c_str() );
	if( expr.error() ) {
		return 2;
	}

	Scalar val = 0;

	switch( str[0] )
	{
	case 's':
		// Sin
		if( str[1] == 'i' && str[2] == 'n' ) {
			val = sin( expr.eval() );
		} else if( str[1] == 'q' && str[2] == 'r' && str[3] == 't' ) {
			val = sqrt( expr.eval() );
		} else {
			return 2;
		}
		break;
	case 'c':
		// Cos
		if( str[1] == 'o' && str[2] == 's' ) {
			val = cos( expr.eval() );
		} else {
			return 2;
		}
		break;
	case 't':
		// Tan
		if( str[1] == 'a' && str[2] == 'n' ) {
			val = tan( expr.eval() );
		} else {
			return 2;
		}
		break;
	case 'h':
		// Halton random number sequence
		if( str[1] == 'a' && str[2] == 'l' ) {
			val = mh.next_halton(int(expr.eval()));
		} else {
			return 2;
		}
		break;
	}

	// assemble together
	static const unsigned int MAX_CHARS = 512;
	char evaluated[MAX_CHARS] = {0};
	snprintf( evaluated, MAX_CHARS, "%.17f", val );

	processed.append( evaluated );
	processed.append( str.substr( y+1, str.length()-1 ) );

	token = String(processed.c_str());

	return 1;
}

inline bool evaluate_functions_in_expression( String& token )
{
	for(;;) {
		char c = evaluate_first_function_in_expression( token );

		if( c==0 ) {
			return true;
		}

		if( c==2 ) {
			return false;
		}
	}
}

inline bool evaluate_expression( String& token )
{
	// The definition of an expression is very simple
	//   All it is is a sequence of numbers seperated by either a +, -, / or *
	//   Brackets may be used to ensure processing order
	//
	// All expressions are evaluated as double precision floating point
    //
	// Expressions must be in the form $(expr)

	// Before evaluating the expression, we should first go through and
	//   evaluate all the functional stuff like sin, cos, tan, sqrt, etc

	if( token.size() <= 4 ) {
		return false;
	}

	if( token[0] != '$' ||
		token[1] != '(' ||
		token[strlen(token.c_str())-1] != ')' ) {
		return false;
	}

	if( !evaluate_functions_in_expression( token ) ) {
		return false;
	}

	// We clamp out string
	const char * str = token.c_str();
	char* s = (char*)&str[1];

	MathExpressionEvaluator::Expression expr( s );
	if( expr.error() ) {
		return false;
	}

	static const unsigned int MAX_CHARS = 512;
	char evaluated[MAX_CHARS] = {0};
	snprintf( evaluated, MAX_CHARS, "%.17f", expr.eval() );

	token = String(evaluated);
	return true;
}

inline bool evaluate_expressions_in_tokens( String* tokens, const unsigned int num_tokens )
{
	for( unsigned int i=0; i<num_tokens; i++ ) {
		// Check to see if we have an expression
		if( tokens[i][0] == '$' ) {
			// This token contains an expression
			if( !evaluate_expression( tokens[i] ) ) {
				return false;
			}
		}
	}

	return true;
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
			// Tracks uniform color painter values so that material parsers
			// can validate energy conservation at scene-definition time.
			struct PainterColor { double c[3]; };
			static thread_local std::map<std::string, PainterColor> s_painterColors;

			static void ClearParseState() {
				s_painterColors.clear();
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
							"ChunkParser:: Failed to parse parameter name `%s` (not declared in `%s` descriptor)",
							pname.c_str(),
							desc.keyword.empty() ? "(unknown)" : desc.keyword.c_str() );
						return false;
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

			// Optional rasterizer params accepted only by a subset.
			template<typename PushFn>
			static void AddOptimalMISParams( PushFn P ) {
				{ auto& p = P(); p.name = "optimal_mis";                     p.kind = ValueKind::Bool; p.description = "Enable optimal MIS";                  p.defaultValueHint = "FALSE"; }
				{ auto& p = P(); p.name = "optimal_mis_training_iterations"; p.kind = ValueKind::UInt; p.description = "Optimal-MIS training iterations";    p.defaultValueHint = "4"; }
				{ auto& p = P(); p.name = "optimal_mis_tile_size";           p.kind = ValueKind::UInt; p.description = "Optimal-MIS tile size";              p.defaultValueHint = "32"; }
			}

			// StabilityConfig params accepted by all non-MLT rasterizers.
			template<typename PushFn>
			static void AddStabilityConfigParams( PushFn P ) {
				{ auto& p = P(); p.name = "direct_clamp";                           p.kind = ValueKind::Double; p.description = "Clamp on direct-lighting contribution"; p.defaultValueHint = "0 (disabled)"; }
				{ auto& p = P(); p.name = "indirect_clamp";                         p.kind = ValueKind::Double; p.description = "Clamp on indirect contribution";         p.defaultValueHint = "0 (disabled)"; }
				{ auto& p = P(); p.name = "rr_min_depth";                           p.kind = ValueKind::UInt;   p.description = "Min depth before Russian roulette";     p.defaultValueHint = "5"; }
				{ auto& p = P(); p.name = "rr_threshold";                           p.kind = ValueKind::Double; p.description = "Throughput threshold for RR";           p.defaultValueHint = "0.01"; }
				{ auto& p = P(); p.name = "max_diffuse_bounce";                     p.kind = ValueKind::UInt;   p.description = "Max diffuse bounce depth";              p.defaultValueHint = "-1"; }
				{ auto& p = P(); p.name = "max_glossy_bounce";                      p.kind = ValueKind::UInt;   p.description = "Max glossy bounce depth";               p.defaultValueHint = "-1"; }
				{ auto& p = P(); p.name = "max_transmission_bounce";                p.kind = ValueKind::UInt;   p.description = "Max transmission bounce depth";         p.defaultValueHint = "-1"; }
				{ auto& p = P(); p.name = "max_translucent_bounce";                 p.kind = ValueKind::UInt;   p.description = "Max translucent bounce depth";          p.defaultValueHint = "-1"; }
				{ auto& p = P(); p.name = "max_volume_bounce";                      p.kind = ValueKind::UInt;   p.description = "Max volume bounce depth";               p.defaultValueHint = "-1"; }
				{ auto& p = P(); p.name = "light_bvh";                              p.kind = ValueKind::Bool;   p.description = "Use a BVH over lights for NEE";         p.defaultValueHint = "TRUE"; }
				{ auto& p = P(); p.name = "branching_threshold";                    p.kind = ValueKind::Double; p.description = "Normalized throughput gate for subpath splitting at multi-lobe delta vertices (0 = always branch, 1 = never)"; p.defaultValueHint = "0.5"; }
			}
			template<typename PushFn>
			static void AddPathGuidingParams( PushFn P ) {
				{ auto& p = P(); p.name = "pathguiding";                            p.kind = ValueKind::Bool;   p.description = "Enable path guiding";                   p.defaultValueHint = "FALSE"; }
				{ auto& p = P(); p.name = "pathguiding_iterations";                 p.kind = ValueKind::UInt;   p.description = "Training iterations";                   p.defaultValueHint = "4"; }
				{ auto& p = P(); p.name = "pathguiding_spp";                        p.kind = ValueKind::UInt;   p.description = "Samples per pixel during training";     p.defaultValueHint = "4"; }
				{ auto& p = P(); p.name = "pathguiding_alpha";                      p.kind = ValueKind::Double; p.description = "Mixing factor with BSDF sampling";      p.defaultValueHint = "0.5"; }
				{ auto& p = P(); p.name = "pathguiding_learned_alpha";              p.kind = ValueKind::Bool;   p.description = "Per-cell Adam-learned mixing alpha (Müller 2017 v2); modest win at SPP >= 256, neutral at low SPP";  p.defaultValueHint = "TRUE"; }
				{ auto& p = P(); p.name = "pathguiding_max_depth";                  p.kind = ValueKind::UInt;   p.description = "Max depth to apply guiding";            p.defaultValueHint = "8"; }
				{ auto& p = P(); p.name = "pathguiding_light_max_depth";            p.kind = ValueKind::UInt;   p.description = "Max light subpath depth";               p.defaultValueHint = "8"; }
				{ auto& p = P(); p.name = "pathguiding_sampling_type";              p.kind = ValueKind::Enum;   p.enumValues = {"ris","RIS","OneSampleMIS"}; p.description = "Sampling strategy (any string other than ris/RIS selects OneSampleMIS)";  p.defaultValueHint = "OneSampleMIS"; }
				{ auto& p = P(); p.name = "pathguiding_ris_candidates";             p.kind = ValueKind::UInt;   p.description = "RIS candidate count";                   p.defaultValueHint = "8"; }
				{ auto& p = P(); p.name = "pathguiding_complete_paths";             p.kind = ValueKind::Bool;   p.description = "Enable complete-path guiding";          p.defaultValueHint = "FALSE"; }
				{ auto& p = P(); p.name = "pathguiding_complete_path_strategy_selection"; p.kind = ValueKind::Bool; p.description = "Enable complete-path strategy selection"; p.defaultValueHint = "FALSE"; }
				{ auto& p = P(); p.name = "pathguiding_complete_path_strategy_samples";   p.kind = ValueKind::UInt; p.description = "Complete-path strategy samples";          p.defaultValueHint = "64"; }
			}
			template<typename PushFn>
			static void AddAdaptiveSamplingParams( PushFn P ) {
				{ auto& p = P(); p.name = "adaptive_max_samples";                   p.kind = ValueKind::UInt;   p.description = "Max adaptive samples per pixel";        p.defaultValueHint = "0 (disabled)"; }
				{ auto& p = P(); p.name = "adaptive_threshold";                     p.kind = ValueKind::Double; p.description = "Relative-error threshold";              p.defaultValueHint = "0.05"; }
				{ auto& p = P(); p.name = "show_adaptive_map";                      p.kind = ValueKind::Bool;   p.description = "Visualize the adaptive sample map";     p.defaultValueHint = "FALSE"; }
			}
			template<typename PushFn>
			static void AddPixelFilterParams( PushFn P ) {
				{ auto& p = P(); p.name = "pixel_sampler";                          p.kind = ValueKind::String; p.description = "Pixel sampler strategy";                p.defaultValueHint = "stratified"; }
				{ auto& p = P(); p.name = "pixel_sampler_param";                    p.kind = ValueKind::Double; p.description = "Sampler-specific parameter";            p.defaultValueHint = "1.0"; }
				{ auto& p = P(); p.name = "pixel_filter";                           p.kind = ValueKind::String; p.description = "Reconstruction filter";                 p.defaultValueHint = "box"; }
				{ auto& p = P(); p.name = "pixel_filter_width";                     p.kind = ValueKind::Double; p.description = "Filter width";                          p.defaultValueHint = "1.0"; }
				{ auto& p = P(); p.name = "pixel_filter_height";                    p.kind = ValueKind::Double; p.description = "Filter height";                         p.defaultValueHint = "1.0"; }
				{ auto& p = P(); p.name = "pixel_filter_paramA";                    p.kind = ValueKind::Double; p.description = "Filter paramA";                         p.defaultValueHint = "0"; }
				{ auto& p = P(); p.name = "pixel_filter_paramB";                    p.kind = ValueKind::Double; p.description = "Filter paramB";                         p.defaultValueHint = "0"; }
				{ auto& p = P(); p.name = "blue_noise_sampler";                     p.kind = ValueKind::Bool;   p.description = "Use blue-noise sampler";                p.defaultValueHint = "FALSE"; }
			}
			template<typename PushFn>
			static void AddRadianceMapParams( PushFn P ) {
				{ auto& p = P(); p.name = "radiance_map";                           p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Environment radiance painter"; }
				{ auto& p = P(); p.name = "radiance_scale";                         p.kind = ValueKind::Double; p.description = "Scale applied to radiance map";         p.defaultValueHint = "1.0"; }
				{ auto& p = P(); p.name = "radiance_background";                    p.kind = ValueKind::Bool;   p.description = "Also use as camera background";         p.defaultValueHint = "TRUE"; }
				{ auto& p = P(); p.name = "radiance_orient";                        p.kind = ValueKind::DoubleVec3; p.description = "Rotation (degrees) X Y Z";           p.defaultValueHint = "0 0 0"; }
			}
			template<typename PushFn>
			static void AddProgressiveParams( PushFn P ) {
				{ auto& p = P(); p.name = "progressive_rendering";                  p.kind = ValueKind::Bool;   p.description = "Enable progressive rendering";          p.defaultValueHint = "FALSE"; }
				{ auto& p = P(); p.name = "progressive_samples_per_pass";           p.kind = ValueKind::UInt;   p.description = "Samples per progressive pass";          p.defaultValueHint = "1"; }
			}
			// Core spectral params — exactly the fields of SpectralConfig.
			// Used by every spectral rasterizer (pixelintegratingspectral,
			// PT/BDPT/VCM spectral, MLT spectral).
			template<typename PushFn>
			static void AddSpectralCoreParams( PushFn P ) {
				{ auto& p = P(); p.name = "spectral_samples";  p.kind = ValueKind::UInt;   p.description = "Number of spectral samples per pixel";       p.defaultValueHint = "1"; }
				{ auto& p = P(); p.name = "nmbegin";           p.kind = ValueKind::Double; p.description = "Start wavelength (nm)";                      p.defaultValueHint = "380"; }
				{ auto& p = P(); p.name = "nmend";             p.kind = ValueKind::Double; p.description = "End wavelength (nm)";                        p.defaultValueHint = "780"; }
				{ auto& p = P(); p.name = "num_wavelengths";   p.kind = ValueKind::UInt;   p.description = "Discrete wavelengths sampled";               p.defaultValueHint = "10"; }
				{ auto& p = P(); p.name = "hwss";              p.kind = ValueKind::Bool;   p.description = "Enable hero-wavelength stratified sampling"; p.defaultValueHint = "FALSE"; }
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
			template<typename PushFn>
			static void AddSMSConfigParams( PushFn P ) {
				{ auto& p = P(); p.name = "sms_enabled";                            p.kind = ValueKind::Bool;   p.description = "Enable Specular Manifold Sampling";     p.defaultValueHint = "FALSE"; }
				{ auto& p = P(); p.name = "sms_max_iterations";                     p.kind = ValueKind::UInt;   p.description = "Max SMS Newton iterations";             p.defaultValueHint = "20"; }
				{ auto& p = P(); p.name = "sms_threshold";                          p.kind = ValueKind::Double; p.description = "SMS convergence threshold";             p.defaultValueHint = "1e-5"; }
				{ auto& p = P(); p.name = "sms_max_chain_depth";                    p.kind = ValueKind::UInt;   p.description = "Max SMS manifold-chain depth";          p.defaultValueHint = "2"; }
				{ auto& p = P(); p.name = "sms_biased";                             p.kind = ValueKind::Bool;   p.description = "Use biased SMS estimator";              p.defaultValueHint = "FALSE"; }
				{ auto& p = P(); p.name = "sms_bernoulli_trials";                   p.kind = ValueKind::UInt;   p.description = "Bernoulli trials per vertex";           p.defaultValueHint = "1"; }
				{ auto& p = P(); p.name = "sms_multi_trials";                       p.kind = ValueKind::UInt;   p.description = "Multi-trials per vertex";               p.defaultValueHint = "1"; }
				{ auto& p = P(); p.name = "sms_photon_count";                       p.kind = ValueKind::UInt;   p.description = "SMS photon budget";                     p.defaultValueHint = "10000"; }
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
				{ auto& p = P(); p.name = "location";           p.kind = ValueKind::DoubleVec3; p.description = "World-space position"; }
				{ auto& p = P(); p.name = "lookat";             p.kind = ValueKind::DoubleVec3; p.description = "Look-at target point"; }
				{ auto& p = P(); p.name = "up";                 p.kind = ValueKind::DoubleVec3; p.description = "Up vector"; p.defaultValueHint = "0 1 0"; }
				{ auto& p = P(); p.name = "width";              p.kind = ValueKind::UInt;       p.description = "Image width (pixels)"; p.defaultValueHint = "640"; }
				{ auto& p = P(); p.name = "height";             p.kind = ValueKind::UInt;       p.description = "Image height (pixels)"; p.defaultValueHint = "480"; }
				{ auto& p = P(); p.name = "pixelAR";            p.kind = ValueKind::Double;     p.description = "Pixel aspect ratio"; p.defaultValueHint = "1.0"; }
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
			template<typename PushFn>
			static void AddNoisePainterCommonParams( PushFn P ) {
				{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;     p.description = "Unique name";                p.defaultValueHint = "noname"; }
				{ auto& p = P(); p.name = "colora";      p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "First colour"; }
				{ auto& p = P(); p.name = "colorb";      p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "Second colour"; }
				{ auto& p = P(); p.name = "persistence"; p.kind = ValueKind::Double;     p.description = "Octave amplitude falloff";   p.defaultValueHint = "0.5"; }
				{ auto& p = P(); p.name = "octaves";     p.kind = ValueKind::UInt;       p.description = "Number of noise octaves";    p.defaultValueHint = "4"; }
				{ auto& p = P(); p.name = "scale";       p.kind = ValueKind::DoubleVec3; p.description = "Per-axis scale";             p.defaultValueHint = "1 1 1"; }
				{ auto& p = P(); p.name = "shift";       p.kind = ValueKind::DoubleVec3; p.description = "Per-axis shift";             p.defaultValueHint = "0 0 0"; }
			}
			template<typename PushFn>
			static void AddBaseRasterizerParams( PushFn P ) {
				{ auto& p = P(); p.name = "defaultshader";                          p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Shader}; p.description = "Default shader chain for hit points"; p.defaultValueHint = "global"; }
				{ auto& p = P(); p.name = "samples";                                p.kind = ValueKind::UInt;   p.description = "Samples per pixel";                     p.defaultValueHint = "1"; }
				{ auto& p = P(); p.name = "show_luminaires";                        p.kind = ValueKind::Bool;   p.description = "Show direct-visible luminaires";        p.defaultValueHint = "TRUE"; }
				{ auto& p = P(); p.name = "oidn_denoise";                           p.kind = ValueKind::Bool;   p.description = "Enable OIDN denoiser";                  p.defaultValueHint = "TRUE"; }
				{ auto& p = P(); p.name = "oidn_quality";                           p.kind = ValueKind::Enum;   p.enumValues = {"auto","high","balanced","fast"}; p.description = "OIDN quality preset (auto picks from render-time / megapixels)"; p.defaultValueHint = "auto"; }
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
					std::string color_space = bag.GetString( "colorspace", "sRGB" );

					PainterColor pc = { {color[0], color[1], color[2]} };
					s_painterColors[name] = pc;

					return pJob.AddUniformColorPainter( name.c_str(), color, color_space.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "uniformcolor_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Constant RGB painter.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";      p.kind = ValueKind::String;     p.description = "Unique name";                          p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "color";     p.kind = ValueKind::DoubleVec3; p.description = "R G B values";                         p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "colorspace";p.kind = ValueKind::String;    p.description = "Interpretation of R G B";             p.defaultValueHint = "sRGB"; }
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
					std::string color_space = bag.GetString( "colorspace", "sRGB" );

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
						{ auto& p = P(); p.name = "colorspace"; p.kind = ValueKind::String;     p.description = "Interpretation of the fallback RGB";          p.defaultValueHint = "sRGB"; }
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
						FILE* f = fopen( GlobalMediaPathLocator().Find( String( fname.c_str() ) ).c_str(), "r" );
						if( f ) {
							while( !feof( f ) ) {
								double nm, amp;
								fscanf( f, "%lf %lf", &nm, &amp );
								wavelengths.push_back( nm );
								amplitudes.push_back( amp );
							}
							fclose( f );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to open file `%s`", fname.c_str() );
							return false;
						}
					}

					// Optional file-loaded wavelengths
					if( bag.Has( "nmfile" ) ) {
						std::string fname = bag.GetString( "nmfile" );
						FILE* f = fopen( GlobalMediaPathLocator().Find( String( fname.c_str() ) ).c_str(), "r" );
						if( f ) {
							while( !feof( f ) ) {
								double nm;
								fscanf( f, "%lf", &nm );
								wavelengths.push_back( nm );
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
							while( !feof( f ) ) {
								double amp;
								fscanf( f, "%lf", &amp );
								amplitudes.push_back( amp );
							}
							fclose( f );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to open file `%s`", fname.c_str() );
							return false;
						}
					}

					return pJob.AddSpectralColorPainter( name.c_str(), &amplitudes[0], &wavelengths[0], nmbegin, nmend, static_cast<unsigned int>(amplitudes.size()), scale );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "spectral_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Spectral painter defined by wavelength/amplitude samples.";
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
					if( bag.Has( "filter_type" ) ) {
						std::string ft = bag.GetString( "filter_type" );
						if(      ft == "NNB" )            filter_type = 0;
						else if( ft == "Bilinear" )       filter_type = 1;
						else if( ft == "CatmullRom" )     filter_type = 2;
						else if( ft == "UniformBSpline" ) filter_type = 3;
						else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Unknown filter type `%s`", ft.c_str() );
							return false;
						}
					}

					return pJob.AddPNGTexturePainter( name.c_str(), filename.c_str(), color_space, filter_type, lowmemory, scale, shift );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "png_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Texture painter that loads a PNG image.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "file";        p.kind = ValueKind::Filename;   p.description = "PNG file path"; }
						{ auto& p = P(); p.name = "color_space"; p.kind = ValueKind::Enum;       p.enumValues = {"sRGB","Rec709RGB_Linear","ROMMRGB_Linear","ProPhotoRGB"}; p.description = "Source colour space"; p.defaultValueHint = "sRGB"; }
						{ auto& p = P(); p.name = "filter_type"; p.kind = ValueKind::Enum;       p.enumValues = {"nearest","bilinear","catmull-rom","box","cubic-bspline","gaussian"}; p.description = "Texture filter"; p.defaultValueHint = "bilinear"; }
						{ auto& p = P(); p.name = "lowmemory";   p.kind = ValueKind::Bool;       p.description = "Lower memory footprint (8-bit in-core)"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "scale";       p.kind = ValueKind::DoubleVec3; p.description = "R G B scale multipliers"; p.defaultValueHint = "1 1 1"; }
						{ auto& p = P(); p.name = "shift";       p.kind = ValueKind::DoubleVec3; p.description = "R G B additive shift"; p.defaultValueHint = "0 0 0"; }
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
					if( bag.Has( "filter_type" ) ) {
						std::string ft = bag.GetString( "filter_type" );
						if(      ft == "NNB" )            filter_type = 0;
						else if( ft == "Bilinear" )       filter_type = 1;
						else if( ft == "CatmullRom" )     filter_type = 2;
						else if( ft == "UniformBSpline" ) filter_type = 3;
						else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Unknown filter type `%s`", ft.c_str() );
							return false;
						}
					}

					return pJob.AddHDRTexturePainter( name.c_str(), filename.c_str(), filter_type, lowmemory, scale, shift );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "hdr_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Texture painter that loads a Radiance HDR image.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "file";        p.kind = ValueKind::Filename;   p.description = "HDR file path"; }
						{ auto& p = P(); p.name = "filter_type"; p.kind = ValueKind::Enum;       p.enumValues = {"nearest","bilinear","catmull-rom","box","cubic-bspline","gaussian"}; p.description = "Texture filter"; p.defaultValueHint = "bilinear"; }
						{ auto& p = P(); p.name = "lowmemory";   p.kind = ValueKind::Bool;       p.description = "Lower memory footprint"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "scale";       p.kind = ValueKind::DoubleVec3; p.description = "R G B scale"; p.defaultValueHint = "1 1 1"; }
						{ auto& p = P(); p.name = "shift";       p.kind = ValueKind::DoubleVec3; p.description = "R G B shift"; p.defaultValueHint = "0 0 0"; }
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
					if( bag.Has( "filter_type" ) ) {
						std::string ft = bag.GetString( "filter_type" );
						if(      ft == "NNB" )            filter_type = 0;
						else if( ft == "Bilinear" )       filter_type = 1;
						else if( ft == "CatmullRom" )     filter_type = 2;
						else if( ft == "UniformBSpline" ) filter_type = 3;
						else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Unknown filter type `%s`", ft.c_str() );
							return false;
						}
					}

					return pJob.AddEXRTexturePainter( name.c_str(), filename.c_str(), color_space, filter_type, lowmemory, scale, shift );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "exr_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Texture painter that loads an OpenEXR image.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "file";        p.kind = ValueKind::Filename;   p.description = "EXR file path"; }
						{ auto& p = P(); p.name = "color_space"; p.kind = ValueKind::Enum;       p.enumValues = {"sRGB","Rec709RGB_Linear","ROMMRGB_Linear","ProPhotoRGB"}; p.description = "Source colour space"; p.defaultValueHint = "Rec709RGB_Linear"; }
						{ auto& p = P(); p.name = "filter_type"; p.kind = ValueKind::Enum;       p.enumValues = {"nearest","bilinear","catmull-rom","box","cubic-bspline","gaussian"}; p.description = "Texture filter"; p.defaultValueHint = "bilinear"; }
						{ auto& p = P(); p.name = "lowmemory";   p.kind = ValueKind::Bool;       p.description = "Lower memory footprint"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "scale";       p.kind = ValueKind::DoubleVec3; p.description = "R G B scale"; p.defaultValueHint = "1 1 1"; }
						{ auto& p = P(); p.name = "shift";       p.kind = ValueKind::DoubleVec3; p.description = "R G B shift"; p.defaultValueHint = "0 0 0"; }
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
					if( bag.Has( "filter_type" ) ) {
						std::string ft = bag.GetString( "filter_type" );
						if(      ft == "NNB" )            filter_type = 0;
						else if( ft == "Bilinear" )       filter_type = 1;
						else if( ft == "CatmullRom" )     filter_type = 2;
						else if( ft == "UniformBSpline" ) filter_type = 3;
						else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Unknown filter type `%s`", ft.c_str() );
							return false;
						}
					}

					return pJob.AddTIFFTexturePainter( name.c_str(), filename.c_str(), color_space, filter_type, lowmemory, scale, shift );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "tiff_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Texture painter that loads a TIFF image.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "file";        p.kind = ValueKind::Filename;   p.description = "TIFF file path"; }
						{ auto& p = P(); p.name = "color_space"; p.kind = ValueKind::Enum;       p.enumValues = {"sRGB","Rec709RGB_Linear","ROMMRGB_Linear","ProPhotoRGB"}; p.description = "Source colour space"; p.defaultValueHint = "sRGB"; }
						{ auto& p = P(); p.name = "filter_type"; p.kind = ValueKind::Enum;       p.enumValues = {"nearest","bilinear","catmull-rom","box","cubic-bspline","gaussian"}; p.description = "Texture filter"; p.defaultValueHint = "bilinear"; }
						{ auto& p = P(); p.name = "lowmemory";   p.kind = ValueKind::Bool;       p.description = "Lower memory footprint"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "scale";       p.kind = ValueKind::DoubleVec3; p.description = "R G B scale"; p.defaultValueHint = "1 1 1"; }
						{ auto& p = P(); p.name = "shift";       p.kind = ValueKind::DoubleVec3; p.description = "R G B shift"; p.defaultValueHint = "0 0 0"; }
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
						cd.description = "Two-colour checkerboard painter.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";   p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "First colour (painter)"; }
						{ auto& p = P(); p.name = "colorb"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Second colour (painter)"; }
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
						cd.description = "Two-colour stripe painter.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";     p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "First colour (painter)"; }
						{ auto& p = P(); p.name = "colorb";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Second colour (painter)"; }
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
						cd.description = "Procedural Mandelbrot-fractal painter.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";     p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Inside-set colour"; }
						{ auto& p = P(); p.name = "colorb";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Outside-set colour"; }
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
						cd.description = "2D Perlin noise painter.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddNoisePainterCommonParams( P );
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
						cd.description = "Procedural ocean-wave painter (Gerstner waves).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";                p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora";              p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Trough colour"; }
						{ auto& p = P(); p.name = "colorb";              p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Crest colour"; }
						{ auto& p = P(); p.name = "num_waves";           p.kind = ValueKind::UInt;      p.description = "Number of wave components"; p.defaultValueHint = "8"; }
						{ auto& p = P(); p.name = "median_wavelength";   p.kind = ValueKind::Double;    p.description = "Median wavelength"; }
						{ auto& p = P(); p.name = "wavelength_range";    p.kind = ValueKind::Double;    p.description = "Wavelength spread"; }
						{ auto& p = P(); p.name = "median_amplitude";    p.kind = ValueKind::Double;    p.description = "Median amplitude"; }
						{ auto& p = P(); p.name = "amplitude_power";     p.kind = ValueKind::Double;    p.description = "Amplitude falloff exponent"; }
						{ auto& p = P(); p.name = "wind_dir";            p.kind = ValueKind::DoubleVec3;p.description = "Wind direction"; }
						{ auto& p = P(); p.name = "directional_spread";  p.kind = ValueKind::Double;    p.description = "Angular spread"; }
						{ auto& p = P(); p.name = "dispersion_speed";    p.kind = ValueKind::Double;    p.description = "Dispersion coefficient"; }
						{ auto& p = P(); p.name = "seed";                p.kind = ValueKind::UInt;      p.description = "RNG seed"; }
						{ auto& p = P(); p.name = "time";                p.kind = ValueKind::Double;    p.description = "Time variable"; }
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
						cd.description = "3D Perlin noise painter.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddNoisePainterCommonParams( P );
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
						cd.description = "3D wavelet noise painter.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddNoisePainterCommonParams( P );
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
						cd.description = "Reaction-diffusion procedural texture.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";       p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora";     p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "First colour"; }
						{ auto& p = P(); p.name = "colorb";     p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "Second colour"; }
						{ auto& p = P(); p.name = "grid_size";  p.kind = ValueKind::UInt;       p.description = "Simulation grid edge";        p.defaultValueHint = "64"; }
						{ auto& p = P(); p.name = "da";         p.kind = ValueKind::Double;     p.description = "Diffusion rate of A";          p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "db";         p.kind = ValueKind::Double;     p.description = "Diffusion rate of B";          p.defaultValueHint = "0.5"; }
						{ auto& p = P(); p.name = "feed";       p.kind = ValueKind::Double;     p.description = "Feed rate";                    p.defaultValueHint = "0.055"; }
						{ auto& p = P(); p.name = "kill";       p.kind = ValueKind::Double;     p.description = "Kill rate";                    p.defaultValueHint = "0.062"; }
						{ auto& p = P(); p.name = "iterations"; p.kind = ValueKind::UInt;       p.description = "Simulation iterations";        p.defaultValueHint = "5000"; }
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
						cd.description = "3D Gabor noise painter.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";            p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora";          p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "First colour"; }
						{ auto& p = P(); p.name = "colorb";          p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "Second colour"; }
						{ auto& p = P(); p.name = "frequency";       p.kind = ValueKind::Double;     p.description = "Carrier frequency";   p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "bandwidth";       p.kind = ValueKind::Double;     p.description = "Gaussian bandwidth";  p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "orientation";     p.kind = ValueKind::DoubleVec3; p.description = "Orientation vector"; }
						{ auto& p = P(); p.name = "impulse_density"; p.kind = ValueKind::Double;     p.description = "Impulses per unit volume"; p.defaultValueHint = "64"; }
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
						cd.description = "3D simplex noise painter.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddNoisePainterCommonParams( P );
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
						cd.description = "Signed-distance-field procedural painter.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";             p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora";           p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "Inside colour"; }
						{ auto& p = P(); p.name = "colorb";           p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "Outside colour"; }
						{ auto& p = P(); p.name = "type";             p.kind = ValueKind::Enum;       p.enumValues = {"sphere","box","torus","cylinder","plane","gyroid","menger"}; p.description = "SDF primitive"; }
						{ auto& p = P(); p.name = "param1";           p.kind = ValueKind::Double;     p.description = "Shape parameter 1"; }
						{ auto& p = P(); p.name = "param2";           p.kind = ValueKind::Double;     p.description = "Shape parameter 2"; }
						{ auto& p = P(); p.name = "param3";           p.kind = ValueKind::Double;     p.description = "Shape parameter 3"; }
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
						cd.description = "3D curl-noise painter.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddNoisePainterCommonParams( P );
						{ auto& p = P(); p.name = "epsilon"; p.kind = ValueKind::Double; p.description = "Finite-difference step"; p.defaultValueHint = "1e-4"; }
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
						cd.description = "3D domain-warped noise painter.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddNoisePainterCommonParams( P );
						{ auto& p = P(); p.name = "warp_amplitude"; p.kind = ValueKind::Double; p.description = "Warp displacement amplitude"; p.defaultValueHint = "0.5"; }
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
						cd.description = "Hybrid Perlin + Worley noise painter.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddNoisePainterCommonParams( P );
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
						cd.description = "3D Worley (cellular / Voronoi) noise painter.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";   p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora"; p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "First colour"; }
						{ auto& p = P(); p.name = "colorb"; p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "Second colour"; }
						{ auto& p = P(); p.name = "jitter"; p.kind = ValueKind::Double;     p.description = "Feature jitter";               p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "metric"; p.kind = ValueKind::Enum;       p.enumValues = {"euclidean","manhattan","chebyshev","minkowski"}; p.description = "Distance metric"; p.defaultValueHint = "euclidean"; }
						{ auto& p = P(); p.name = "output"; p.kind = ValueKind::Enum;       p.enumValues = {"f1","f2","f2_minus_f1"};      p.description = "Value function"; p.defaultValueHint = "f1"; }
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
						cd.description = "3D turbulence (absolute-valued Perlin) painter.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddNoisePainterCommonParams( P );
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
						FILE* f = fopen( GlobalMediaPathLocator().Find( String( fname.c_str() ) ).c_str(), "r" );
						if( f ) {
							while( !feof( f ) ) {
								double x, y;
								char painter[256] = {0};
								fscanf( f, "%lf %lf %255s", &x, &y, painter );
								ptx.push_back( x );
								pty.push_back( y );
								painters.push_back( painter );
							}
							fclose( f );
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
						cd.description = "2D Voronoi painter with per-cell colours.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";       p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "gen";        p.kind = ValueKind::String;    p.repeatable = true; p.description = "Voronoi generator: x y paintername (repeatable)"; }
						{ auto& p = P(); p.name = "file";       p.kind = ValueKind::Filename;  p.description = "Generator list file (each line: x y paintername)"; }
						{ auto& p = P(); p.name = "border";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Border colour (painter)"; p.defaultValueHint = "none"; }
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
						FILE* f = fopen( GlobalMediaPathLocator().Find( String( fname.c_str() ) ).c_str(), "r" );
						if( f ) {
							int num=0;
							fscanf( f, "%d", &num );
							for( int i=0; i<num; i++ ) {
								double x, y, z;
								char painter[256] = {0};
								fscanf( f, "%lf %lf %lf %255s", &x, &y, &z, painter );
								ptx.push_back( x );
								pty.push_back( y );
								ptz.push_back( z );
								painters.push_back( painter );
							}
							fclose( f );
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

					bool bRet = pJob.AddVoronoi3DPainter( name.c_str(), &ptx[0], &pty[0], &ptz[0], (const char**)pntrs, num, border=="none"?0:border.c_str(), bordersize );

					delete [] pntrs;
					delete [] pntrmem;

					return bRet;
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "voronoi3d_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "3D Voronoi painter with per-cell colours.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";       p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "gen";        p.kind = ValueKind::String;    p.repeatable = true; p.description = "Voronoi generator: x y z paintername (repeatable)"; }
						{ auto& p = P(); p.name = "file";       p.kind = ValueKind::Filename;  p.description = "Generator list file (count-prefixed: N then N lines of x y z paintername)"; }
						{ auto& p = P(); p.name = "border";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Border colour (painter)"; p.defaultValueHint = "none"; }
						{ auto& p = P(); p.name = "bordersize"; p.kind = ValueKind::Double;    p.description = "Border width"; p.defaultValueHint = "0"; }
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
						cd.description = "Angle-dependent iridescent painter.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";   p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Normal-incidence colour"; }
						{ auto& p = P(); p.name = "colorb"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Grazing-angle colour"; }
						{ auto& p = P(); p.name = "bias";   p.kind = ValueKind::Double;    p.description = "View-angle bias";               p.defaultValueHint = "0.5"; }
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
						cd.description = "Planckian blackbody spectrum painter.";
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

					return pJob.AddBlendPainter( name.c_str(), colora.c_str(), colorb.c_str(), mask.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "blend_painter"; cd.category = ChunkCategory::Painter;
						cd.description = "Blend two painters using a third painter as mask.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";   p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "colora"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "First colour"; }
						{ auto& p = P(); p.name = "colorb"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Second colour"; }
						{ auto& p = P(); p.name = "mask";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Blend-weight painter"; }
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
						FILE* f = fopen( GlobalMediaPathLocator().Find( String( fname.c_str() ) ).c_str(), "r" );
						if( f ) {
							while( !feof( f ) ) {
								double x, y;
								fscanf( f, "%lf %lf", &x, &y );
								cp_x.push_back( x );
								cp_y.push_back( y );
							}
							fclose( f );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to open file `%s`", fname.c_str() );
							return false;
						}
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

					// Setup the array of strings
					char** func = new char*[cp_x.size()];
					for( unsigned int i=0; i<cp_x.size(); i++ ) {
						func[i] = (char*)(&(*(cp_y[i].begin())));
					}

					bool bRet = pJob.AddPiecewiseLinearFunction2D( name.c_str(), &cp_x[0], func, static_cast<unsigned int>(cp_x.size()) );

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
						{ auto& p = P(); p.name = "reflectance"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Albedo painter"; }
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
						{ auto& p = P(); p.name = "reflectance"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Reflectance painter"; }
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
						{ auto& p = P(); p.name = "refractance"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Transmittance painter"; }
						{ auto& p = P(); p.name = "ior";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter,ChunkCategory::Function}; p.description = "Index of refraction"; }
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
					std::string tau         = bag.GetString( "tau",         "none" );
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
						{ auto& p = P(); p.name = "reflectance";       p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Diffuse substrate"; }
						{ auto& p = P(); p.name = "tau";               p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Transmittance"; }
						{ auto& p = P(); p.name = "ior";               p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter,ChunkCategory::Function}; p.description = "Index of refraction"; }
						{ auto& p = P(); p.name = "scattering";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Scattering coefficient"; p.defaultValueHint = "64"; }
						{ auto& p = P(); p.name = "henyey-greenstein"; p.kind = ValueKind::Bool;      p.description = "Use Henyey-Greenstein phase"; p.defaultValueHint = "FALSE"; }
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
					std::string tau  = bag.GetString( "tau",        "none" );
					std::string ior  = bag.GetString( "ior",        "1.33" );
					std::string scat = bag.GetString( "scattering", "10000" );
					bool hg          = bag.GetBool(   "henyey-greenstein", false );

					return pJob.AddDielectricMaterial( name.c_str(), tau.c_str(), ior.c_str(), scat.c_str(), hg );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "dielectric_material"; cd.category = ChunkCategory::Material;
						cd.description = "Fresnel dielectric (reflect + refract) with optional volumetric scattering.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";              p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "tau";               p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Transmittance"; }
						{ auto& p = P(); p.name = "ior";               p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter,ChunkCategory::Function}; p.description = "Index of refraction"; }
						{ auto& p = P(); p.name = "scattering";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Scattering coefficient"; p.defaultValueHint = "10000"; }
						{ auto& p = P(); p.name = "henyey-greenstein"; p.kind = ValueKind::Bool;      p.description = "Use Henyey-Greenstein phase"; p.defaultValueHint = "FALSE"; }
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
						{ auto& p = P(); p.name = "ior";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter,ChunkCategory::Function}; p.description = "Index of refraction"; }
						{ auto& p = P(); p.name = "absorption"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Absorption coefficient"; }
						{ auto& p = P(); p.name = "scattering"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Scattering coefficient"; }
						{ auto& p = P(); p.name = "g";          p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Henyey-Greenstein g"; }
						{ auto& p = P(); p.name = "roughness";  p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Surface roughness"; }
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
						{ auto& p = P(); p.name = "ior";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter,ChunkCategory::Function}; p.description = "Index of refraction"; }
						{ auto& p = P(); p.name = "absorption";  p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Absorption"; }
						{ auto& p = P(); p.name = "scattering";  p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Scattering"; }
						{ auto& p = P(); p.name = "g";           p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Henyey-Greenstein g"; }
						{ auto& p = P(); p.name = "roughness";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Surface roughness"; }
						{ auto& p = P(); p.name = "max_bounces"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Max volume bounces per ray"; }
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
						{ auto& p = P(); p.name = "exitance"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Emitted radiance"; }
						{ auto& p = P(); p.name = "material"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Material}; p.description = "Underlying material"; }
						{ auto& p = P(); p.name = "scale";    p.kind = ValueKind::Double;    p.description = "Exitance multiplier"; p.defaultValueHint = "1.0"; }
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
						{ auto& p = P(); p.name = "exitance"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Emitted radiance"; }
						{ auto& p = P(); p.name = "material"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Material}; p.description = "Underlying material"; }
						{ auto& p = P(); p.name = "N";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Phong exponent"; }
						{ auto& p = P(); p.name = "scale";    p.kind = ValueKind::Double;    p.description = "Exitance multiplier"; p.defaultValueHint = "1.0"; }
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
						{ auto& p = P(); p.name = "rd";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Diffuse reflectance"; }
						{ auto& p = P(); p.name = "rs";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Specular reflectance"; }
						{ auto& p = P(); p.name = "nu";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "U-direction exponent"; }
						{ auto& p = P(); p.name = "nv";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "V-direction exponent"; }
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
						{ auto& p = P(); p.name = "rd";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Diffuse reflectance"; }
						{ auto& p = P(); p.name = "rs";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Specular reflectance"; }
						{ auto& p = P(); p.name = "N";    p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Phong exponent"; }
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
					std::string ext  = bag.GetString( "ext",        "none" );
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
						{ auto& p = P(); p.name = "ref";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Reflectance"; }
						{ auto& p = P(); p.name = "tau";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Transmittance"; }
						{ auto& p = P(); p.name = "ext";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Extinction"; }
						{ auto& p = P(); p.name = "N";          p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Phong exponent"; }
						{ auto& p = P(); p.name = "scattering"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Scattering"; }
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
						static const char* painterRefs[] = {
							"melanin_fraction","melanin_blend","hemoglobin_epidermis","carotene_fraction",
							"hemoglobin_dermis","epidermis_thickness","ior_epidermis","ior_dermis",
							"blood_oxygenation","roughness"
						};
						{ auto& p = P(); p.name = "name"; p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						for (const char* n : painterRefs) {
							auto& p = P(); p.name = n; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Skin-model painter parameter";
						}
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
					std::string g                 = bag.GetString( "g",                         "none" );
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
						{ auto& p = P(); p.name = "sca";                      p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Scattering amplitude"; }
						{ auto& p = P(); p.name = "g";                        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Phase-function asymmetry"; }
						{ auto& p = P(); p.name = "whole_blood";              p.kind = ValueKind::Double;    p.description = "Blood volume fraction"; p.defaultValueHint = "0.012"; }
						{ auto& p = P(); p.name = "hb_ratio";                 p.kind = ValueKind::Double;    p.description = "Oxygenated hemoglobin ratio"; p.defaultValueHint = "0.75"; }
						{ auto& p = P(); p.name = "bilirubin_concentration";  p.kind = ValueKind::Double;    p.description = "Bilirubin concentration"; p.defaultValueHint = "0.05"; }
						{ auto& p = P(); p.name = "betacarotene_concentration";p.kind = ValueKind::Double;   p.description = "Beta-carotene concentration"; p.defaultValueHint = "7.0e-5"; }
						{ auto& p = P(); p.name = "diffuse";                  p.kind = ValueKind::Bool;      p.description = "Use diffuse approximation"; p.defaultValueHint = "TRUE"; }
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
					std::string extinction     = bag.GetString( "extinction",                "none" );

					return pJob.AddCompositeMaterial( name.c_str(), top.c_str(), bottom.c_str(), max_recur, max_refl_recur, max_refr_recur, max_diff_recur, max_tran_recur, thickness, extinction.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "composite_material"; cd.category = ChunkCategory::Material;
						cd.description = "Layered composite of two materials separated by a translucent interior.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";                 p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "top";                  p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Material}; p.description = "Top material"; }
						{ auto& p = P(); p.name = "bottom";               p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Material}; p.description = "Bottom material"; }
						{ auto& p = P(); p.name = "max_recursion";            p.kind = ValueKind::UInt; p.description = "Max composite recursion"; p.defaultValueHint = "3"; }
						{ auto& p = P(); p.name = "max_reflection_recursion"; p.kind = ValueKind::UInt; p.description = "Max reflection recursion"; p.defaultValueHint = "3"; }
						{ auto& p = P(); p.name = "max_refraction_recursion"; p.kind = ValueKind::UInt; p.description = "Max refraction recursion"; p.defaultValueHint = "3"; }
						{ auto& p = P(); p.name = "max_diffuse_recursion";    p.kind = ValueKind::UInt; p.description = "Max diffuse recursion"; p.defaultValueHint = "3"; }
						{ auto& p = P(); p.name = "max_translucent_recursion";p.kind = ValueKind::UInt; p.description = "Max translucent recursion"; p.defaultValueHint = "3"; }
						{ auto& p = P(); p.name = "thickness";            p.kind = ValueKind::Double;    p.description = "Layer thickness"; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "extinction";           p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Interior extinction"; }
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
						{ auto& p = P(); p.name = "rd";    p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Diffuse reflectance"; }
						{ auto& p = P(); p.name = "rs";    p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Specular reflectance"; }
						{ auto& p = P(); p.name = "alpha"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Surface roughness"; }
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
						{ auto& p = P(); p.name = "rd";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Diffuse reflectance"; }
						{ auto& p = P(); p.name = "rs";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Specular reflectance"; }
						{ auto& p = P(); p.name = "alphax"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "X-direction roughness"; }
						{ auto& p = P(); p.name = "alphay"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Y-direction roughness"; }
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

					return pJob.AddGGXMaterial( name.c_str(), rd.c_str(), rs.c_str(), alphax.c_str(), alphay.c_str(), ior.c_str(), extinction.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "ggx_material"; cd.category = ChunkCategory::Material;
						cd.description = "GGX (Trowbridge-Reitz) microfacet BRDF with optional Fresnel.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";       p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "rd";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Diffuse reflectance"; }
						{ auto& p = P(); p.name = "rs";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Specular reflectance (F0)"; }
						{ auto& p = P(); p.name = "alphax";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "X roughness"; }
						{ auto& p = P(); p.name = "alphay";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Y roughness"; }
						{ auto& p = P(); p.name = "ior";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter,ChunkCategory::Function}; p.description = "Fresnel IOR"; }
						{ auto& p = P(); p.name = "extinction"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Fresnel extinction"; }
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

					return pJob.AddCookTorranceMaterial( name.c_str(), rd.c_str(), rs.c_str(), facets.c_str(), ior.c_str(), extinction.c_str() );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "cooktorrance_material"; cd.category = ChunkCategory::Material;
						cd.description = "Cook-Torrance microfacet BRDF.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";       p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "rd";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Diffuse reflectance"; }
						{ auto& p = P(); p.name = "rs";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Specular reflectance"; }
						{ auto& p = P(); p.name = "facets";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Microfacet slope distribution"; }
						{ auto& p = P(); p.name = "ior";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter,ChunkCategory::Function}; p.description = "Fresnel IOR"; }
						{ auto& p = P(); p.name = "extinction"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Fresnel extinction"; }
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
						{ auto& p = P(); p.name = "reflectance"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Albedo"; }
						{ auto& p = P(); p.name = "roughness";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Surface roughness (sigma)"; }
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
						{ auto& p = P(); p.name = "rd";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Diffuse reflectance"; }
						{ auto& p = P(); p.name = "rs";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Specular reflectance"; }
						{ auto& p = P(); p.name = "roughness"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Surface roughness"; }
						{ auto& p = P(); p.name = "isotropy";  p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Isotropy factor"; }
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
						return cd;
					}();
					return d;
				}
			};


			//////////////////////////////////////////
			// Cameras
			//////////////////////////////////////////

			struct PinholeCameraAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					double fov          = 30.0 * DEG_TO_RAD;
					if( bag.Has( "fov" ) ) fov = bag.GetDouble( "fov" ) * DEG_TO_RAD;
					unsigned int xres   = bag.GetUInt(   "width",         256 );
					unsigned int yres   = bag.GetUInt(   "height",        256 );
					double pixelAR      = bag.GetDouble( "pixelAR",       1.0 );
					double exposure     = bag.GetDouble( "exposure",      0 );
					double scanningRate = bag.GetDouble( "scanning_rate", 0 );
					double pixelRate    = bag.GetDouble( "pixel_rate",    0 );

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

					return pJob.SetPinholeCamera( loc, lookat, up, fov, xres, yres, pixelAR, exposure, scanningRate, pixelRate, orientation, target_orientation );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "pinhole_camera"; cd.category = ChunkCategory::Camera;
						cd.description = "Standard perspective (pinhole) camera.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "fov"; p.kind = ValueKind::Double; p.description = "Field of view (degrees)"; p.defaultValueHint = "45"; }
						AddCameraCommonParams( P );
						return cd;
					}();
					return d;
				}
			};

			struct ONBPinholeCameraAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					double fov          = 30.0 * DEG_TO_RAD;
					if( bag.Has( "fov" ) ) fov = bag.GetDouble( "fov" ) * DEG_TO_RAD;
					unsigned int xres   = bag.GetUInt(   "width",         256 );
					unsigned int yres   = bag.GetUInt(   "height",        256 );
					double pixelAR      = bag.GetDouble( "pixelAR",       1.0 );
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
					return pJob.SetPinholeCameraONB( ONB_U, ONB_V, ONB_W, loc, fov, xres, yres, pixelAR, exposure, scanningRate, pixelRate );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "onb_pinhole_camera"; cd.category = ChunkCategory::Camera;
						cd.description = "Pinhole camera oriented via an orthonormal basis built from two axes (va, vb) plus a `components` selector.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "location";      p.kind = ValueKind::DoubleVec3; p.description = "Eye position"; }
						{ auto& p = P(); p.name = "va";            p.kind = ValueKind::DoubleVec3; p.description = "First ONB axis (used per `components`)"; }
						{ auto& p = P(); p.name = "vb";            p.kind = ValueKind::DoubleVec3; p.description = "Second ONB axis (used per `components`)"; }
						{ auto& p = P(); p.name = "components";    p.kind = ValueKind::Enum;       p.enumValues = {"UV","VU","UW","WU","VW","WV"}; p.description = "Which ONB components va/vb represent"; }
						{ auto& p = P(); p.name = "fov";           p.kind = ValueKind::Double;     p.description = "Field of view (degrees)"; p.defaultValueHint = "30"; }
						{ auto& p = P(); p.name = "width";         p.kind = ValueKind::UInt;       p.description = "Image width"; p.defaultValueHint = "256"; }
						{ auto& p = P(); p.name = "height";        p.kind = ValueKind::UInt;       p.description = "Image height"; p.defaultValueHint = "256"; }
						{ auto& p = P(); p.name = "pixelAR";       p.kind = ValueKind::Double;     p.description = "Pixel aspect ratio"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "exposure";      p.kind = ValueKind::Double;     p.description = "Shutter exposure"; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "scanning_rate"; p.kind = ValueKind::Double;     p.description = "Rolling-shutter rate"; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "pixel_rate";    p.kind = ValueKind::Double;     p.description = "Per-pixel time offset"; p.defaultValueHint = "0"; }
						return cd;
					}();
					return d;
				}
			};

			struct ThinlensCameraAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					double fov          = 30.0 * DEG_TO_RAD;
					if( bag.Has( "fov" ) ) fov = bag.GetDouble( "fov" ) * DEG_TO_RAD;
					unsigned int xres   = bag.GetUInt(   "width",          256 );
					unsigned int yres   = bag.GetUInt(   "height",         256 );
					double aperture     = bag.GetDouble( "aperture_size",  1.0 );
					double focal        = bag.GetDouble( "focal_length",   0.1 );
					double focus        = bag.GetDouble( "focus_distance", 1.0 );
					double pixelAR      = bag.GetDouble( "pixelAR",        1.0 );
					double exposure     = bag.GetDouble( "exposure",       0 );
					double scanningRate = bag.GetDouble( "scanning_rate",  0 );
					double pixelRate    = bag.GetDouble( "pixel_rate",     0 );

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

					if( focal >= focus ) {
						GlobalLog()->PrintEx( eLog_Error, "Focal length is >= focus distance, that makes no sense!" );
						return false;
					}

					orientation[0] *= DEG_TO_RAD;
					orientation[1] *= DEG_TO_RAD;
					orientation[2] *= DEG_TO_RAD;

					target_orientation[0] *= DEG_TO_RAD;
					target_orientation[1] *= DEG_TO_RAD;

					return pJob.SetThinlensCamera( loc, lookat, up, fov, xres, yres, pixelAR, exposure, scanningRate, pixelRate, orientation, target_orientation, aperture, focal, focus );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "thinlens_camera"; cd.category = ChunkCategory::Camera;
						cd.description = "Perspective camera with thin-lens depth of field.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "fov";             p.kind = ValueKind::Double; p.description = "Field of view (degrees)"; p.defaultValueHint = "45"; }
						{ auto& p = P(); p.name = "aperture_size";  p.kind = ValueKind::Double; p.description = "Aperture radius"; p.defaultValueHint = "0.01"; }
						{ auto& p = P(); p.name = "focus_distance"; p.kind = ValueKind::Double; p.description = "Focal distance"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "focal_length";  p.kind = ValueKind::Double; p.description = "Focal length (legacy alias)"; }
						AddCameraCommonParams( P );
						return cd;
					}();
					return d;
				}
			};

			struct RealisticCameraAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					unsigned int xres = bag.GetUInt(   "width",          256 );
					unsigned int yres = bag.GetUInt(   "height",         256 );

					// Default film sizes are 35mm
					double film_size  = bag.GetDouble( "film_size",      35 );
					double fstop      = bag.GetDouble( "fstop",          2.8 );
					double focal      = bag.GetDouble( "focal_length",   0.1 );
					double focus      = bag.GetDouble( "focus_distance", 1.0 );

					double pixelAR      = bag.GetDouble( "pixelAR",       1.0 );
					double exposure     = bag.GetDouble( "exposure",      0 );
					double scanningRate = bag.GetDouble( "scanning_rate", 0 );
					double pixelRate    = bag.GetDouble( "pixel_rate",    0 );

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

					if( focal >= focus ) {
						GlobalLog()->PrintEx( eLog_Error, "Focal length is >= focus distance, that makes no sense!" );
						return false;
					}

					// From ThinLensCamera.cpp
					// Angle of View = 2 * ArcTan(Film Dimension / (2 * Focal Length))
					const double fov = 2.0 * atan(film_size / (2.0 * focal));
					const double aperture = focal / fstop;

					orientation[0] *= DEG_TO_RAD;
					orientation[1] *= DEG_TO_RAD;
					orientation[2] *= DEG_TO_RAD;

					target_orientation[0] *= DEG_TO_RAD;
					target_orientation[1] *= DEG_TO_RAD;

					return pJob.SetThinlensCamera( loc, lookat, up, fov, xres, yres, pixelAR, exposure, scanningRate, pixelRate, orientation, target_orientation, aperture, focal, focus );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "realistic_camera"; cd.category = ChunkCategory::Camera;
						cd.description = "Thin-lens camera parameterised by film size and f-stop (defaults to thin-lens internally).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "film_size";      p.kind = ValueKind::Double; p.description = "Film diagonal (mm)"; p.defaultValueHint = "35"; }
						{ auto& p = P(); p.name = "fstop";          p.kind = ValueKind::Double; p.description = "Aperture f-stop"; p.defaultValueHint = "2.8"; }
						{ auto& p = P(); p.name = "focus_distance"; p.kind = ValueKind::Double; p.description = "Focus distance"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "focal_length";   p.kind = ValueKind::Double; p.description = "Focal length"; p.defaultValueHint = "0.1"; }
						AddCameraCommonParams( P );
						return cd;
					}();
					return d;
				}
			};

			struct FisheyeCameraAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					unsigned int xres   = bag.GetUInt(   "width",         256 );
					unsigned int yres   = bag.GetUInt(   "height",        256 );
					double pixelAR      = bag.GetDouble( "pixelAR",       1.0 );
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

					return pJob.SetFisheyeCamera( loc, lookat, up, xres, yres, pixelAR, exposure, scanningRate, pixelRate, orientation, target_orientation, scale );
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
					unsigned int xres = bag.GetUInt( "width",  256 );
					unsigned int yres = bag.GetUInt( "height", 256 );
					double pixelAR    = bag.GetDouble( "pixelAR",       1.0 );
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

					return pJob.SetOrthographicCamera( loc, lookat, up, xres, yres, vpscale, pixelAR, exposure, scanningRate, pixelRate, orientation, target_orientation );
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
						{ auto& p = P(); p.name = "radii"; p.kind = ValueKind::DoubleVec3; p.description = "Per-axis radii"; p.defaultValueHint = "1 1 1"; }
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
					std::string axisStr = bag.GetString( "axis", "x" );
					char axis        = axisStr.empty() ? 'x' : axisStr[0];
					return pJob.AddCylinderGeometry( name.c_str(), axis, radius, height );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "cylinder_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Implicit cylinder.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";   p.kind = ValueKind::String; p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "axis";   p.kind = ValueKind::Enum;   p.enumValues = {"x","y","z"}; p.description = "Cylinder axis"; p.defaultValueHint = "x"; }
						{ auto& p = P(); p.name = "radius"; p.kind = ValueKind::Double; p.description = "Cylinder radius"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "height"; p.kind = ValueKind::Double; p.description = "Cylinder height"; p.defaultValueHint = "1.0"; }
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
					double disp_scale         = bag.GetDouble( "disp_scale",    1.0 );
					bool double_sided         = bag.GetBool(   "double_sided",  false );
					bool face_normals         = bag.GetBool(   "face_normals",  false );
					// Legacy maxpolygons/maxdepth/bsp keys accepted but ignored (Tier A2).

					if( base_geometry.empty() ) {
						GlobalLog()->Print( eLog_Error, "DisplacedGeometry:: `base_geometry` is required" );
						return false;
					}

					return pJob.AddDisplacedGeometry(
						name.c_str(),
						base_geometry.c_str(),
						detail,
						displacement == "none" ? 0 : displacement.c_str(),
						disp_scale,
						double_sided,
						face_normals );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "displaced_geometry"; cd.category = ChunkCategory::Geometry;
						cd.description = "Tessellated geometry with painter-driven vertex displacement.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";          p.kind = ValueKind::String;    p.description = "Unique name"; p.required = true; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "base_geometry"; p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Geometry}; p.required = true; p.description = "Geometry to displace"; }
						{ auto& p = P(); p.name = "detail";        p.kind = ValueKind::UInt;      p.description = "Subdivision detail level"; p.defaultValueHint = "32"; }
						{ auto& p = P(); p.name = "displacement";  p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Displacement painter"; }
						{ auto& p = P(); p.name = "disp_scale";    p.kind = ValueKind::Double;    p.description = "Displacement scale"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "double_sided";  p.kind = ValueKind::Bool;      p.description = "Render both sides"; p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "face_normals";  p.kind = ValueKind::Bool;      p.description = "Flat per-face normals"; p.defaultValueHint = "FALSE"; }
						// Retired: accepted for backward compat with pre-A2 scene files; ignored.
						{ auto& p = P(); p.name = "maxpolygons";   p.kind = ValueKind::UInt;      p.description = "Retired (BVH is sole accelerator)"; }
						{ auto& p = P(); p.name = "maxdepth";      p.kind = ValueKind::UInt;      p.description = "Retired (BVH is sole accelerator)"; }
						{ auto& p = P(); p.name = "bsp";           p.kind = ValueKind::Bool;      p.description = "Retired (BVH is sole accelerator)"; }
						return cd;
					}();
					return d;
				}
			};

			//////////////////////////////////////////
			// Modifiers
			//////////////////////////////////////////

			struct BumpmapModifierAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string name     = bag.GetString( "name",       "noname" );
					std::string function = bag.GetString( "function",   "none" );
					double scale         = bag.GetDouble( "scale",      1.0 );
					double window        = bag.GetDouble( "windowsize", 0.01 );
					return pJob.AddBumpMapModifier( name.c_str(), function.c_str(), scale, window );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "bumpmap_modifier"; cd.category = ChunkCategory::Modifier;
						cd.description = "Bump-map modifier perturbing surface normal via a painter.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";       p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "function";   p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Heightfield painter"; }
						{ auto& p = P(); p.name = "scale";      p.kind = ValueKind::Double;    p.description = "Displacement scale"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "windowsize"; p.kind = ValueKind::Double;    p.description = "Finite-difference step"; p.defaultValueHint = "0.01"; }
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

					return pJob.AddHomogeneousMedium( name.c_str(), sigma_a, sigma_s, phase_type.c_str(), phase_g );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "homogeneous_medium"; cd.category = ChunkCategory::Medium;
						cd.description = "Uniform participating medium.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";       p.kind = ValueKind::String;     p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "absorption"; p.kind = ValueKind::DoubleVec3; p.description = "Absorption coefficient (R G B)"; p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "scattering"; p.kind = ValueKind::DoubleVec3; p.description = "Scattering coefficient (R G B)"; p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "phase";      p.kind = ValueKind::String;     p.description = "Phase function: either `isotropic` or `hg <g>` (Henyey-Greenstein with asymmetry g)"; p.tupleKinds = {ValueKind::Enum, ValueKind::Double}; p.enumValues = {"isotropic","hg"}; p.defaultValueHint = "isotropic"; }
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
					std::string geometry = bag.GetString( "geometry", "none" );
					std::string material = bag.GetString( "material", "none" );
					std::string modifier = bag.GetString( "modifier", "none" );
					std::string shader   = bag.GetString( "shader",   "none" );
					std::string interior_medium = bag.GetString( "interior_medium", "none" );

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

					bool bRet = pJob.AddObject( name.c_str(), geometry.c_str(), material=="none"?0:material.c_str(), modifier=="none"?0:modifier.c_str(), shader=="none"?0:shader.c_str(), radianceMapConfig, pos, orient, scale, bCastsShadows, bReceivesShadows );

					if( bRet && !(interior_medium == "none") ) {
						bRet = pJob.SetObjectInteriorMedium( name.c_str(), interior_medium.c_str() );
					}

					return bRet;
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "standard_object"; cd.category = ChunkCategory::Object;
						cd.description = "Scene object instancing a geometry with material, modifier, and shader.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";             p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "geometry";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Geometry}; p.required = true; p.description = "Geometry to instance"; }
						{ auto& p = P(); p.name = "material";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Material}; p.description = "Surface material"; }
						{ auto& p = P(); p.name = "modifier";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Modifier}; p.description = "Geometry modifier"; }
						{ auto& p = P(); p.name = "shader";           p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Shader}; p.description = "Shader override"; }
						{ auto& p = P(); p.name = "position";         p.kind = ValueKind::DoubleVec3;p.description = "World-space position"; p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "orientation";      p.kind = ValueKind::DoubleVec3;p.description = "Euler orientation (degrees)"; p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "scale";            p.kind = ValueKind::DoubleVec3;p.description = "Per-axis scale"; p.defaultValueHint = "1 1 1"; }
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

					return pJob.AddCSGObject( name.c_str(), obja.c_str(), objb.c_str(), op, material=="none"?0:material.c_str(), modifier=="none"?0:modifier.c_str(), shader=="none"?0:shader.c_str(), radianceMapConfig, pos, orient, bCastsShadows, bReceivesShadows );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "csg_object"; cd.category = ChunkCategory::Object;
						cd.description = "Constructive solid geometry combining two objects by a boolean operation.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "name";        p.kind = ValueKind::String;    p.description = "Unique name"; p.defaultValueHint = "noname"; }
						{ auto& p = P(); p.name = "obja";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Object}; p.description = "First operand object"; }
						{ auto& p = P(); p.name = "objb";        p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Object}; p.description = "Second operand object"; }
						{ auto& p = P(); p.name = "operation";   p.kind = ValueKind::Enum;      p.enumValues = {"union","intersection","subtraction"}; p.description = "CSG operation"; }
						{ auto& p = P(); p.name = "material";    p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Material}; p.description = "Override material"; }
						{ auto& p = P(); p.name = "modifier";    p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Modifier}; p.description = "Override modifier"; }
						{ auto& p = P(); p.name = "shader";      p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Shader}; p.description = "Override shader"; }
						{ auto& p = P(); p.name = "position";        p.kind = ValueKind::DoubleVec3;p.description = "World-space position"; p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "orientation";     p.kind = ValueKind::DoubleVec3;p.description = "Euler orientation (degrees)"; p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "casts_shadows";   p.kind = ValueKind::Bool;      p.description = "Casts shadows"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "receives_shadows";p.kind = ValueKind::Bool;      p.description = "Receives shadows"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "radiance_map";    p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Painter}; p.description = "Per-object radiance map"; }
						{ auto& p = P(); p.name = "radiance_scale";  p.kind = ValueKind::Double;    p.description = "Radiance-map scale"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "radiance_orient"; p.kind = ValueKind::DoubleVec3;p.description = "Radiance-map orientation (degrees)"; p.defaultValueHint = "0 0 0"; }
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
					return pJob.AddAmbientLight( name.c_str(), power, color );
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
						{ auto& p = P(); p.name = "color"; p.kind = ValueKind::DoubleVec3; p.description = "R G B in scene colour space"; p.defaultValueHint = "0 0 0"; }
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
					return pJob.AddPointOmniLight( name.c_str(), power, color, position, shootphotons );
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
						{ auto& p = P(); p.name = "color";        p.kind = ValueKind::DoubleVec3; p.description = "R G B emission colour";            p.defaultValueHint = "0 0 0"; }
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
					return pJob.AddPointSpotLight( name.c_str(), power, color, target, inner, outer, position, shootphotons );
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
						{ auto& p = P(); p.name = "color";        p.kind = ValueKind::DoubleVec3; p.description = "R G B emission colour";           p.defaultValueHint = "0 0 0"; }
						{ auto& p = P(); p.name = "shootphotons"; p.kind = ValueKind::Bool;       p.description = "Whether this light emits photons"; p.defaultValueHint = "TRUE"; }
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
					return pJob.AddDirectionalLight( name.c_str(), power, color, dir );
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
						{ auto& p = P(); p.name = "color";     p.kind = ValueKind::DoubleVec3; p.description = "R G B emission colour";           p.defaultValueHint = "0 0 0"; }
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
						// `branch` was retired in favor of `branching_threshold` on the rasterizer chunk.
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
						// `branch` was retired in favor of `branching_threshold` on the rasterizer chunk.
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
						cd.description = "Screen-space / hemisphere ambient occlusion.";
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
						{ auto& p = P(); p.name = "N";        p.kind = ValueKind::Reference;  p.referenceCategories = {ChunkCategory::Painter}; p.description = "Directionality exponent"; p.defaultValueHint = "1.0"; }
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
						{ auto& p = P(); p.name = "shaderop"; p.kind = ValueKind::String; p.repeatable = true; p.description = "Shader-op triple: <shaderop-name> <min-depth> <max-depth> <op>"; }
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
					std::string defaultshader   = bag.GetString( "defaultshader",  "global" );
					unsigned int maxRecur       = bag.GetUInt(   "max_recursion",  10 );
					unsigned int numSamples     = bag.GetUInt(   "samples",        1 );
					unsigned int numLumSamples  = bag.GetUInt(   "lum_samples",    1 );
					std::string luminarySampler = bag.GetString( "luminary_sampler", "none" );
					double luminarySamplerParam = bag.GetDouble( "luminary_sampler_param", 1.0 );
					bool showLuminaires         = bag.GetBool(   "show_luminaires", true );
					bool oidnDenoise            = bag.GetBool(   "oidn_denoise",    true );
					OidnQuality oidnQuality     = ParseOidnQuality( bag.GetString( "oidn_quality", "auto" ) );

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
					if( bag.Has("branching_threshold") )             stabilityConfig.branchingThreshold           = bag.GetDouble("branching_threshold");
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
						showLuminaires, oidnDenoise, oidnQuality, guidingConfig, adaptiveConfig, stabilityConfig, progressiveConfig );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "pixelpel_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "RGB pel-based unidirectional path-tracing integrator.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "defaultshader";         p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Shader}; p.description = "Default shader chain for hit points"; p.defaultValueHint = "global"; }
						{ auto& p = P(); p.name = "max_recursion";         p.kind = ValueKind::UInt;      p.description = "Maximum ray recursion depth";   p.defaultValueHint = "10"; }
						{ auto& p = P(); p.name = "samples";               p.kind = ValueKind::UInt;      p.description = "Samples per pixel";              p.defaultValueHint = "1"; }
						{ auto& p = P(); p.name = "lum_samples";           p.kind = ValueKind::UInt;      p.description = "Luminaire samples per hit";      p.defaultValueHint = "1"; }
						{ auto& p = P(); p.name = "luminary_sampler";      p.kind = ValueKind::String;    p.description = "Luminary sampling strategy";     p.defaultValueHint = "none"; }
						{ auto& p = P(); p.name = "luminary_sampler_param";p.kind = ValueKind::Double;    p.description = "Luminary sampler parameter";     p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "show_luminaires";       p.kind = ValueKind::Bool;      p.description = "Show direct-visible luminaires"; p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "oidn_denoise";          p.kind = ValueKind::Bool;      p.description = "Enable OIDN denoiser";           p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "oidn_quality";          p.kind = ValueKind::Enum;      p.enumValues = {"auto","high","balanced","fast"}; p.description = "OIDN quality preset (auto picks from render-time / megapixels)"; p.defaultValueHint = "auto"; }
						{ auto& p = P(); p.name = "choose_one_light";      p.kind = ValueKind::Bool;      p.description = "Legacy — ignored (unified LightSampler always selects one light per NEE)"; p.defaultValueHint = ""; }
						AddPixelFilterParams( P );
						AddRadianceMapParams( P );
						AddPathGuidingParams( P );
						AddAdaptiveSamplingParams( P );
						AddStabilityConfigParams( P );
						{ auto& p = P(); p.name = "filter_glossy";                    p.kind = ValueKind::Double; p.description = "Glossy roughness floor";                     p.defaultValueHint = "0 (disabled)"; }
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
					while( !feof( f ) ) {
						double v;
						fscanf( f, "%lf", &v );
						out.push_back( v );
					}
					fclose( f );
					return true;
				}

				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string defaultshader   = bag.GetString( "defaultshader",  "global" );
					unsigned int maxRecur       = bag.GetUInt(   "max_recursion",  10 );
					unsigned int numSamples     = bag.GetUInt(   "samples",        1 );
					unsigned int numLumSamples  = bag.GetUInt(   "lum_samples",    1 );
					std::string luminarySampler = bag.GetString( "luminary_sampler", "none" );
					double luminarySamplerParam = bag.GetDouble( "luminary_sampler_param", 1.0 );
					bool showLuminaires         = bag.GetBool(   "show_luminaires", true );
					bool oidnDenoise            = bag.GetBool(   "oidn_denoise",    true );
					OidnQuality oidnQuality     = ParseOidnQuality( bag.GetString( "oidn_quality", "auto" ) );
					bool integrateRGB           = bag.GetBool(   "integrate_rgb",   false );

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
							while( !feof( f ) ) {
								double nm, r, g, b;
								fscanf( f, "%lf %lf %lf %lf", &nm, &r, &g, &b );
								spd_wavelengths.push_back( nm );
								spd_r.push_back( r );
								spd_g.push_back( g );
								spd_b.push_back( b );
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
					if( bag.Has("branching_threshold") )     stabilityConfig.branchingThreshold    = bag.GetDouble("branching_threshold");

					return pJob.SetPixelBasedSpectralIntegratingRasterizer( numSamples, numLumSamples, spectralConfig, maxRecur, defaultshader.c_str(), radianceMapConfig,
						luminarySampler=="none"?0:luminarySampler.c_str(), luminarySamplerParam,
						pixelFilterConfig,
						showLuminaires,
						integrateRGB, static_cast<unsigned int>(spd_wavelengths.size()), integrateRGB?&spd_wavelengths[0]:0, integrateRGB?&spd_r[0]:0, integrateRGB?&spd_g[0]:0, integrateRGB?&spd_b[0]:0,
						oidnDenoise, oidnQuality, stabilityConfig
						);
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "pixelintegratingspectral_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "Spectral pel-based path-tracing integrator.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddBaseRasterizerParams( P );
						{ auto& p = P(); p.name = "max_recursion";   p.kind = ValueKind::UInt; p.description = "Maximum ray recursion depth"; p.defaultValueHint = "10"; }
						{ auto& p = P(); p.name = "lum_samples";     p.kind = ValueKind::UInt; p.description = "Luminaire samples per hit";   p.defaultValueHint = "1"; }
						{ auto& p = P(); p.name = "luminary_sampler";p.kind = ValueKind::String; p.description = "Luminary sampling strategy"; p.defaultValueHint = "none"; }
						{ auto& p = P(); p.name = "luminary_sampler_param"; p.kind = ValueKind::Double; p.description = "Luminary sampler parameter"; p.defaultValueHint = "1.0"; }
						{ auto& p = P(); p.name = "choose_one_light";p.kind = ValueKind::Bool;   p.description = "Legacy — ignored (unified LightSampler always selects one light per NEE)"; p.defaultValueHint = ""; }
						AddPixelFilterParams( P );
						AddRadianceMapParams( P );
						AddSpectralConfigParams( P );
						AddStabilityConfigParams( P );
						{ auto& p = P(); p.name = "filter_glossy"; p.kind = ValueKind::Double; p.description = "Glossy roughness floor"; p.defaultValueHint = "0 (disabled)"; }
						return cd;
					}();
					return d;
				}
			};

			struct BDPTPelRasterizerAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string defaultshader   = bag.GetString( "defaultshader",  "global" );
					unsigned int numSamples     = bag.GetUInt(   "samples",        1 );
					unsigned int maxEyeDepth    = bag.GetUInt(   "max_eye_depth",  8 );
					unsigned int maxLightDepth  = bag.GetUInt(   "max_light_depth",8 );
					bool showLuminaires         = bag.GetBool(   "show_luminaires", true );
					bool oidnDenoise            = bag.GetBool(   "oidn_denoise",    true );
					OidnQuality oidnQuality     = ParseOidnQuality( bag.GetString( "oidn_quality", "auto" ) );

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

					PathGuidingConfig guidingConfig;
					if( bag.Has("pathguiding") )                                    guidingConfig.enabled              = bag.GetBool("pathguiding");
					if( bag.Has("pathguiding_iterations") )                         guidingConfig.trainingIterations   = bag.GetUInt("pathguiding_iterations");
					if( bag.Has("pathguiding_spp") )                                guidingConfig.trainingSPP          = bag.GetUInt("pathguiding_spp");
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
					if( bag.Has("branching_threshold") )             stabilityConfig.branchingThreshold           = bag.GetDouble("branching_threshold");
					if( bag.Has("optimal_mis") )                     stabilityConfig.optimalMIS                   = bag.GetBool("optimal_mis");
					if( bag.Has("optimal_mis_training_iterations") ) stabilityConfig.optimalMISTrainingIterations = bag.GetUInt("optimal_mis_training_iterations");
					if( bag.Has("optimal_mis_tile_size") )           stabilityConfig.optimalMISTileSize           = bag.GetUInt("optimal_mis_tile_size");

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
						smsConfig, oidnDenoise, oidnQuality, guidingConfig, adaptiveConfig, stabilityConfig, progressiveConfig );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "bdpt_pel_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "RGB bidirectional path-tracing integrator.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddBaseRasterizerParams( P );
						{ auto& p = P(); p.name = "max_eye_depth";   p.kind = ValueKind::UInt; p.description = "Max eye subpath depth";   p.defaultValueHint = "8"; }
						{ auto& p = P(); p.name = "max_light_depth"; p.kind = ValueKind::UInt; p.description = "Max light subpath depth"; p.defaultValueHint = "8"; }
						{ auto& p = P(); p.name = "choose_one_light";p.kind = ValueKind::Bool; p.description = "Legacy — ignored (unified LightSampler always selects one light per NEE)"; p.defaultValueHint = ""; }
						AddPixelFilterParams( P );
						AddRadianceMapParams( P );
						AddSMSConfigParams( P );
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

			struct BDPTSpectralRasterizerAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string defaultshader   = bag.GetString( "defaultshader",  "global" );
					unsigned int numSamples     = bag.GetUInt(   "samples",        1 );
					unsigned int maxEyeDepth    = bag.GetUInt(   "max_eye_depth",  8 );
					unsigned int maxLightDepth  = bag.GetUInt(   "max_light_depth",8 );
					bool showLuminaires         = bag.GetBool(   "show_luminaires", true );
					bool oidnDenoise            = bag.GetBool(   "oidn_denoise",    true );
					OidnQuality oidnQuality     = ParseOidnQuality( bag.GetString( "oidn_quality", "auto" ) );

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

					SMSConfig smsConfig;
					if( bag.Has("sms_enabled") )          smsConfig.enabled         = bag.GetBool("sms_enabled");
					if( bag.Has("sms_max_iterations") )   smsConfig.maxIterations   = bag.GetUInt("sms_max_iterations");
					if( bag.Has("sms_threshold") )        smsConfig.threshold       = bag.GetDouble("sms_threshold");
					if( bag.Has("sms_max_chain_depth") )  smsConfig.maxChainDepth   = bag.GetUInt("sms_max_chain_depth");
					if( bag.Has("sms_biased") )           smsConfig.biased          = bag.GetBool("sms_biased");
					if( bag.Has("sms_bernoulli_trials") ) smsConfig.bernoulliTrials = bag.GetUInt("sms_bernoulli_trials");
					if( bag.Has("sms_multi_trials") )     smsConfig.multiTrials     = bag.GetUInt("sms_multi_trials");
					if( bag.Has("sms_photon_count") )     smsConfig.photonCount     = bag.GetUInt("sms_photon_count");

					PathGuidingConfig guidingConfig;
					if( bag.Has("pathguiding") )            guidingConfig.enabled            = bag.GetBool("pathguiding");
					if( bag.Has("pathguiding_iterations") ) guidingConfig.trainingIterations = bag.GetUInt("pathguiding_iterations");
					if( bag.Has("pathguiding_spp") )        guidingConfig.trainingSPP        = bag.GetUInt("pathguiding_spp");
					if( bag.Has("pathguiding_alpha") )      guidingConfig.alpha              = bag.GetDouble("pathguiding_alpha");
					if( bag.Has("pathguiding_learned_alpha") ) guidingConfig.learnedAlpha    = bag.GetBool("pathguiding_learned_alpha");
					if( bag.Has("pathguiding_max_depth") )  guidingConfig.maxGuidingDepth    = bag.GetUInt("pathguiding_max_depth");
					if( bag.Has("pathguiding_sampling_type") ) {
						const std::string st = bag.GetString("pathguiding_sampling_type");
						guidingConfig.samplingType = ( st == "ris" || st == "RIS" ) ? eGuidingRIS : eGuidingOneSampleMIS;
					}
					if( bag.Has("pathguiding_ris_candidates") ) guidingConfig.risCandidates       = std::max( 2u, bag.GetUInt("pathguiding_ris_candidates") );
					if( bag.Has("pathguiding_complete_paths") ) guidingConfig.completePathGuiding = bag.GetBool("pathguiding_complete_paths");

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
					if( bag.Has("branching_threshold") )             stabilityConfig.branchingThreshold           = bag.GetDouble("branching_threshold");
					if( bag.Has("optimal_mis") )                     stabilityConfig.optimalMIS                   = bag.GetBool("optimal_mis");
					if( bag.Has("optimal_mis_training_iterations") ) stabilityConfig.optimalMISTrainingIterations = bag.GetUInt("optimal_mis_training_iterations");
					if( bag.Has("optimal_mis_tile_size") )           stabilityConfig.optimalMISTileSize           = bag.GetUInt("optimal_mis_tile_size");

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
						smsConfig, oidnDenoise, oidnQuality, guidingConfig, stabilityConfig, progressiveConfig );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "bdpt_spectral_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "Spectral bidirectional path-tracing integrator.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddBaseRasterizerParams( P );
						{ auto& p = P(); p.name = "max_eye_depth";   p.kind = ValueKind::UInt; p.description = "Max eye subpath depth";   p.defaultValueHint = "8"; }
						{ auto& p = P(); p.name = "max_light_depth"; p.kind = ValueKind::UInt; p.description = "Max light subpath depth"; p.defaultValueHint = "8"; }
						{ auto& p = P(); p.name = "choose_one_light";p.kind = ValueKind::Bool; p.description = "Legacy — ignored (unified LightSampler always selects one light per NEE)"; p.defaultValueHint = ""; }
						AddPixelFilterParams( P );
						AddRadianceMapParams( P );
						// BDPT spectral consumes only the core spectral
						// fields; RGB-to-SPD conversion is done in the
						// painters pipeline.
						AddSpectralCoreParams( P );
						AddSMSConfigParams( P );
						// BDPTSpectral supports a subset of pathguiding params (no
						// light-max-depth, no complete-path-strategy).
						{ auto& p = P(); p.name = "pathguiding";                 p.kind = ValueKind::Bool;   p.description = "Enable path guiding";              p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "pathguiding_iterations";      p.kind = ValueKind::UInt;   p.description = "Training iterations";             p.defaultValueHint = "4"; }
						{ auto& p = P(); p.name = "pathguiding_spp";             p.kind = ValueKind::UInt;   p.description = "Samples per pixel during training"; p.defaultValueHint = "4"; }
						{ auto& p = P(); p.name = "pathguiding_alpha";           p.kind = ValueKind::Double; p.description = "Mixing factor";                   p.defaultValueHint = "0.5"; }
						{ auto& p = P(); p.name = "pathguiding_max_depth";       p.kind = ValueKind::UInt;   p.description = "Max guiding depth";               p.defaultValueHint = "8"; }
						{ auto& p = P(); p.name = "pathguiding_sampling_type";   p.kind = ValueKind::Enum;   p.enumValues = {"ris","RIS","OneSampleMIS"}; p.description = "Sampling strategy"; p.defaultValueHint = "OneSampleMIS"; }
						{ auto& p = P(); p.name = "pathguiding_ris_candidates";  p.kind = ValueKind::UInt;   p.description = "RIS candidate count";             p.defaultValueHint = "8"; }
						{ auto& p = P(); p.name = "pathguiding_complete_paths";  p.kind = ValueKind::Bool;   p.description = "Enable complete-path guiding";    p.defaultValueHint = "FALSE"; }
						AddStabilityConfigParams( P );
						AddOptimalMISParams( P );
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
					std::string defaultshader   = bag.GetString( "defaultshader",  "global" );
					unsigned int numSamples     = bag.GetUInt(   "samples",        1 );
					unsigned int maxEyeDepth    = bag.GetUInt(   "max_eye_depth",  8 );
					unsigned int maxLightDepth  = bag.GetUInt(   "max_light_depth",8 );
					bool showLuminaires         = bag.GetBool(   "show_luminaires", true );
					bool oidnDenoise            = bag.GetBool(   "oidn_denoise",    true );
					OidnQuality oidnQuality     = ParseOidnQuality( bag.GetString( "oidn_quality", "auto" ) );
					double mergeRadius          = bag.GetDouble( "merge_radius",    0.0 );
					bool enableVC               = bag.GetBool(   "vc_enabled",      true );
					bool enableVM               = bag.GetBool(   "vm_enabled",      true );

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
					if( bag.Has("branching_threshold") )     stabilityConfig.branchingThreshold    = bag.GetDouble("branching_threshold");

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
						mergeRadius, enableVC, enableVM, oidnDenoise, oidnQuality,
						guidingConfig, adaptiveConfig, stabilityConfig, progressiveConfig );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "vcm_pel_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "RGB vertex-connection-and-merging integrator.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddBaseRasterizerParams( P );
						{ auto& p = P(); p.name = "max_eye_depth";   p.kind = ValueKind::UInt;   p.description = "Max eye subpath depth";         p.defaultValueHint = "8"; }
						{ auto& p = P(); p.name = "max_light_depth"; p.kind = ValueKind::UInt;   p.description = "Max light subpath depth";       p.defaultValueHint = "8"; }
						{ auto& p = P(); p.name = "merge_radius";    p.kind = ValueKind::Double; p.description = "Photon merge radius (0=auto)"; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "vc_enabled";      p.kind = ValueKind::Bool;   p.description = "Enable vertex connection";      p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "vm_enabled";      p.kind = ValueKind::Bool;   p.description = "Enable vertex merging";         p.defaultValueHint = "TRUE"; }
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
					std::string defaultshader   = bag.GetString( "defaultshader",  "global" );
					unsigned int numSamples     = bag.GetUInt(   "samples",        1 );
					unsigned int maxEyeDepth    = bag.GetUInt(   "max_eye_depth",  8 );
					unsigned int maxLightDepth  = bag.GetUInt(   "max_light_depth",8 );
					bool showLuminaires         = bag.GetBool(   "show_luminaires", true );
					bool oidnDenoise            = bag.GetBool(   "oidn_denoise",    true );
					OidnQuality oidnQuality     = ParseOidnQuality( bag.GetString( "oidn_quality", "auto" ) );
					double mergeRadius          = bag.GetDouble( "merge_radius",    0.0 );
					bool enableVC               = bag.GetBool(   "vc_enabled",      true );
					bool enableVM               = bag.GetBool(   "vm_enabled",      true );

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
					if( bag.Has("branching_threshold") )     stabilityConfig.branchingThreshold    = bag.GetDouble("branching_threshold");

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
						mergeRadius, enableVC, enableVM, oidnDenoise, oidnQuality,
						guidingConfig, stabilityConfig, progressiveConfig );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "vcm_spectral_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "Spectral vertex-connection-and-merging integrator.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddBaseRasterizerParams( P );
						{ auto& p = P(); p.name = "max_eye_depth";   p.kind = ValueKind::UInt;   p.description = "Max eye subpath depth";         p.defaultValueHint = "8"; }
						{ auto& p = P(); p.name = "max_light_depth"; p.kind = ValueKind::UInt;   p.description = "Max light subpath depth";       p.defaultValueHint = "8"; }
						{ auto& p = P(); p.name = "merge_radius";    p.kind = ValueKind::Double; p.description = "Photon merge radius (0=auto)"; p.defaultValueHint = "0"; }
						{ auto& p = P(); p.name = "vc_enabled";      p.kind = ValueKind::Bool;   p.description = "Enable vertex connection";      p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "vm_enabled";      p.kind = ValueKind::Bool;   p.description = "Enable vertex merging";         p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "choose_one_light";p.kind = ValueKind::Bool;   p.description = "Legacy — ignored (unified LightSampler always selects one light per NEE)"; p.defaultValueHint = ""; }
						AddPixelFilterParams( P );
						AddRadianceMapParams( P );
						// VCM spectral consumes only the core spectral
						// fields; RGB-to-SPD conversion is done in the
						// painters pipeline.
						AddSpectralCoreParams( P );
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
					std::string defaultshader   = bag.GetString( "defaultshader",  "global" );
					unsigned int numSamples     = bag.GetUInt(   "samples",        1 );
					bool showLuminaires         = bag.GetBool(   "show_luminaires", true );
					bool oidnDenoise            = bag.GetBool(   "oidn_denoise",    true );
					OidnQuality oidnQuality     = ParseOidnQuality( bag.GetString( "oidn_quality", "auto" ) );

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

					PathGuidingConfig guidingConfig;
					if( bag.Has("pathguiding") )                                    guidingConfig.enabled              = bag.GetBool("pathguiding");
					if( bag.Has("pathguiding_iterations") )                         guidingConfig.trainingIterations   = bag.GetUInt("pathguiding_iterations");
					if( bag.Has("pathguiding_spp") )                                guidingConfig.trainingSPP          = bag.GetUInt("pathguiding_spp");
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
					if( bag.Has("branching_threshold") )             stabilityConfig.branchingThreshold           = bag.GetDouble("branching_threshold");
					if( bag.Has("optimal_mis") )                     stabilityConfig.optimalMIS                   = bag.GetBool("optimal_mis");
					if( bag.Has("optimal_mis_training_iterations") ) stabilityConfig.optimalMISTrainingIterations = bag.GetUInt("optimal_mis_training_iterations");
					if( bag.Has("optimal_mis_tile_size") )           stabilityConfig.optimalMISTileSize           = bag.GetUInt("optimal_mis_tile_size");

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
						smsConfig, oidnDenoise, oidnQuality, guidingConfig, adaptiveConfig, stabilityConfig, progressiveConfig );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "pathtracing_pel_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "Pure unidirectional RGB path tracer (bypasses shader-op chain).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddBaseRasterizerParams( P );
						{ auto& p = P(); p.name = "choose_one_light";p.kind = ValueKind::Bool;   p.description = "Legacy — ignored (unified LightSampler always selects one light per NEE)"; p.defaultValueHint = ""; }
						AddPixelFilterParams( P );
						AddRadianceMapParams( P );
						AddSMSConfigParams( P );
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

			struct PathTracingSpectralRasterizerAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string defaultshader   = bag.GetString( "defaultshader",  "global" );
					unsigned int numSamples     = bag.GetUInt(   "samples",        1 );
					bool showLuminaires         = bag.GetBool(   "show_luminaires", true );
					bool oidnDenoise            = bag.GetBool(   "oidn_denoise",    true );
					OidnQuality oidnQuality     = ParseOidnQuality( bag.GetString( "oidn_quality", "auto" ) );

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
					if( bag.Has("branching_threshold") )             stabilityConfig.branchingThreshold           = bag.GetDouble("branching_threshold");
					if( bag.Has("optimal_mis") )                     stabilityConfig.optimalMIS                   = bag.GetBool("optimal_mis");
					if( bag.Has("optimal_mis_training_iterations") ) stabilityConfig.optimalMISTrainingIterations = bag.GetUInt("optimal_mis_training_iterations");
					if( bag.Has("optimal_mis_tile_size") )           stabilityConfig.optimalMISTileSize           = bag.GetUInt("optimal_mis_tile_size");

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
						smsConfig, oidnDenoise, oidnQuality, adaptiveConfig, stabilityConfig, progressiveConfig );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "pathtracing_spectral_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "Pure unidirectional spectral path tracer (bypasses shader-op chain).";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						AddBaseRasterizerParams( P );
						{ auto& p = P(); p.name = "choose_one_light";p.kind = ValueKind::Bool;   p.description = "Legacy — ignored (unified LightSampler always selects one light per NEE)"; p.defaultValueHint = ""; }
						AddPixelFilterParams( P );
						AddRadianceMapParams( P );
						// PT spectral consumes only the core spectral
						// fields; RGB-to-SPD conversion is done in the
						// painters pipeline.
						AddSpectralCoreParams( P );
						// Legacy alias for `hwss` accepted only by this
						// parser (other spectral integrators use `hwss`).
						{ auto& p = P(); p.name = "use_hwss";        p.kind = ValueKind::Bool; p.description = "Legacy alias for `hwss`"; p.defaultValueHint = "FALSE"; }
						AddSMSConfigParams( P );
						AddAdaptiveSamplingParams( P );
						AddStabilityConfigParams( P );
						AddOptimalMISParams( P );
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
					std::string defaultshader     = bag.GetString( "defaultshader",       "global" );
					unsigned int maxEyeDepth      = bag.GetUInt(   "max_eye_depth",       10 );
					unsigned int maxLightDepth    = bag.GetUInt(   "max_light_depth",     10 );
					unsigned int bootstrapSamples = bag.GetUInt(   "bootstrap_samples",   100000 );
					unsigned int chains           = bag.GetUInt(   "chains",              512 );
					unsigned int mutationsPerPixel= bag.GetUInt(   "mutations_per_pixel", 100 );
					double largeStepProb          = bag.GetDouble( "large_step_prob",     0.3 );
					bool showLuminaires           = bag.GetBool(   "show_luminaires",     true );
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
					bool oidnDenoise              = bag.GetBool(   "oidn_denoise",        false );
					OidnQuality oidnQuality       = ParseOidnQuality( bag.GetString( "oidn_quality", "auto" ) );

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
					if( bag.Has("branching_threshold") ) stabilityConfig.branchingThreshold = bag.GetDouble("branching_threshold");

					return pJob.SetMLTRasterizer( maxEyeDepth, maxLightDepth,
						bootstrapSamples, chains, mutationsPerPixel, largeStepProb,
						defaultshader.c_str(), showLuminaires, oidnDenoise, oidnQuality,
						pixelFilterConfig,
						stabilityConfig );
				}

				const ChunkDescriptor& Describe() const override {
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "mlt_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "Metropolis Light Transport (RGB).  branching_threshold is forced to 1.0.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "defaultshader";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Shader}; p.description = "Default shader chain"; p.defaultValueHint = "global"; }
						{ auto& p = P(); p.name = "max_eye_depth";    p.kind = ValueKind::UInt;   p.description = "Max eye subpath depth";                  p.defaultValueHint = "10"; }
						{ auto& p = P(); p.name = "max_light_depth";  p.kind = ValueKind::UInt;   p.description = "Max light subpath depth";                p.defaultValueHint = "10"; }
						{ auto& p = P(); p.name = "bootstrap_samples";p.kind = ValueKind::UInt;   p.description = "Bootstrap samples";                      p.defaultValueHint = "100000"; }
						{ auto& p = P(); p.name = "chains";           p.kind = ValueKind::UInt;   p.description = "Number of Markov chains";                p.defaultValueHint = "512"; }
						{ auto& p = P(); p.name = "mutations_per_pixel"; p.kind = ValueKind::UInt;p.description = "Mutations per pixel";                     p.defaultValueHint = "100"; }
						{ auto& p = P(); p.name = "large_step_prob";  p.kind = ValueKind::Double; p.description = "Probability of large-step mutation";      p.defaultValueHint = "0.3"; }
						{ auto& p = P(); p.name = "show_luminaires";  p.kind = ValueKind::Bool;   p.description = "Show direct-visible luminaires";         p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "oidn_denoise";     p.kind = ValueKind::Bool;   p.description = "Enable OIDN denoiser";                   p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "oidn_quality";     p.kind = ValueKind::Enum;   p.enumValues = {"auto","high","balanced","fast"}; p.description = "OIDN quality preset (auto picks from render-time / megapixels)"; p.defaultValueHint = "auto"; }
						{ auto& p = P(); p.name = "choose_one_light"; p.kind = ValueKind::Bool;   p.description = "Legacy — ignored (unified LightSampler always selects one light per NEE)"; p.defaultValueHint = ""; }
						AddPixelFilterParams( P );
						// MLT accepts only light_bvh and branching_threshold from
						// StabilityConfig (branching_threshold is forced to 1.0
						// internally per CLAUDE.md).
						{ auto& p = P(); p.name = "light_bvh";            p.kind = ValueKind::Bool;   p.description = "Use a BVH over lights for NEE";         p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "branching_threshold";  p.kind = ValueKind::Double; p.description = "Accepted for parity with other rasterizers but forced to 1.0 (MLT requires a single-subpath proposal)"; p.defaultValueHint = "1.0"; }
						return cd;
					}();
					return d;
				}
			};

			struct MLTSpectralRasterizerAsciiChunkParser : public IAsciiChunkParser
			{
				bool Finalize( const ParseStateBag& bag, IJob& pJob ) const override
				{
					std::string defaultshader     = bag.GetString( "defaultshader",       "global" );
					unsigned int maxEyeDepth      = bag.GetUInt(   "max_eye_depth",       10 );
					unsigned int maxLightDepth    = bag.GetUInt(   "max_light_depth",     10 );
					unsigned int bootstrapSamples = bag.GetUInt(   "bootstrap_samples",   100000 );
					unsigned int chains           = bag.GetUInt(   "chains",              512 );
					unsigned int mutationsPerPixel= bag.GetUInt(   "mutations_per_pixel", 100 );
					double largeStepProb          = bag.GetDouble( "large_step_prob",     0.3 );
					bool showLuminaires           = bag.GetBool(   "show_luminaires",     true );
					// MLT spectral also defaults OIDN off — see the Pel
					// MLT parser for the detailed rationale.
					bool oidnDenoise              = bag.GetBool(   "oidn_denoise",        false );
					OidnQuality oidnQuality       = ParseOidnQuality( bag.GetString( "oidn_quality", "auto" ) );

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
					if( bag.Has("branching_threshold") ) stabilityConfig.branchingThreshold = bag.GetDouble("branching_threshold");

					return pJob.SetMLTSpectralRasterizer( maxEyeDepth, maxLightDepth,
						bootstrapSamples, chains, mutationsPerPixel, largeStepProb,
						defaultshader.c_str(), showLuminaires,
						spectralConfig, oidnDenoise, oidnQuality,
						pixelFilterConfig,
						stabilityConfig );
				}

				const ChunkDescriptor& Describe() const override
				{
					static const ChunkDescriptor d = []{
						ChunkDescriptor cd;
						cd.keyword = "mlt_spectral_rasterizer"; cd.category = ChunkCategory::Rasterizer;
						cd.description = "Metropolis Light Transport (spectral).  branching_threshold is forced to 1.0.";
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "defaultshader";     p.kind = ValueKind::Reference; p.referenceCategories = {ChunkCategory::Shader}; p.description = "Default shader chain"; p.defaultValueHint = "global"; }
						{ auto& p = P(); p.name = "max_eye_depth";    p.kind = ValueKind::UInt;   p.description = "Max eye subpath depth";                  p.defaultValueHint = "10"; }
						{ auto& p = P(); p.name = "max_light_depth";  p.kind = ValueKind::UInt;   p.description = "Max light subpath depth";                p.defaultValueHint = "10"; }
						{ auto& p = P(); p.name = "bootstrap_samples";p.kind = ValueKind::UInt;   p.description = "Bootstrap samples";                      p.defaultValueHint = "100000"; }
						{ auto& p = P(); p.name = "chains";           p.kind = ValueKind::UInt;   p.description = "Number of Markov chains";                p.defaultValueHint = "512"; }
						{ auto& p = P(); p.name = "mutations_per_pixel"; p.kind = ValueKind::UInt;p.description = "Mutations per pixel";                     p.defaultValueHint = "100"; }
						{ auto& p = P(); p.name = "large_step_prob";  p.kind = ValueKind::Double; p.description = "Probability of large-step mutation";      p.defaultValueHint = "0.3"; }
						{ auto& p = P(); p.name = "show_luminaires";  p.kind = ValueKind::Bool;   p.description = "Show direct-visible luminaires";         p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "oidn_denoise";     p.kind = ValueKind::Bool;   p.description = "Enable OIDN denoiser";                   p.defaultValueHint = "FALSE"; }
						{ auto& p = P(); p.name = "oidn_quality";     p.kind = ValueKind::Enum;   p.enumValues = {"auto","high","balanced","fast"}; p.description = "OIDN quality preset (auto picks from render-time / megapixels)"; p.defaultValueHint = "auto"; }
						AddPixelFilterParams( P );
						// MLT spectral consumes only the core spectral
						// fields; RGB-to-SPD conversion is done in the
						// painters pipeline.
						AddSpectralCoreParams( P );
						// MLT accepts only light_bvh and branching_threshold.
						{ auto& p = P(); p.name = "light_bvh";            p.kind = ValueKind::Bool;   p.description = "Use a BVH over lights for NEE";         p.defaultValueHint = "TRUE"; }
						{ auto& p = P(); p.name = "branching_threshold";  p.kind = ValueKind::Double; p.description = "Accepted for parity but forced to 1.0"; p.defaultValueHint = "1.0"; }
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

					return pJob.AddFileRasterizerOutput( pattern.c_str(), multiple, type, (unsigned char)bpp, color_space );
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
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "element";             p.kind = ValueKind::String; p.description = "Element name"; }
						{ auto& p = P(); p.name = "element_type";        p.kind = ValueKind::Enum;   p.enumValues = {"object","camera","light"}; p.description = "Element kind"; p.defaultValueHint = "object"; }
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

						if( !pJob.AddKeyframe( element_type.c_str(), element.c_str(), param.c_str(), values[i].c_str(), time, interp_c, iparams_c ) ) {
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
						auto P = [&cd]() -> ParameterDescriptor& { cd.parameters.emplace_back(); return cd.parameters.back(); };
						{ auto& p = P(); p.name = "element";             p.kind = ValueKind::String; p.description = "Element name"; }
						{ auto& p = P(); p.name = "element_type";        p.kind = ValueKind::Enum;   p.enumValues = {"object","camera","light"}; p.description = "Element kind"; p.defaultValueHint = "object"; }
						{ auto& p = P(); p.name = "param";               p.kind = ValueKind::String; p.description = "Parameter name"; }
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
		add( "png_painter",                           new PngPainterAsciiChunkParser() );
		add( "hdr_painter",                           new HdrPainterAsciiChunkParser() );
		add( "exr_painter",                           new ExrPainterAsciiChunkParser() );
		add( "tiff_painter",                          new TiffPainterAsciiChunkParser() );
		add( "checker_painter",                       new CheckerPainterAsciiChunkParser() );
		add( "lines_painter",                         new LinesPainterAsciiChunkParser() );
		add( "mandelbrot_painter",                    new MandelbrotPainterAsciiChunkParser() );
		add( "perlin2d_painter",                      new Perlin2DPainterAsciiChunkParser() );
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
		add( "composite_material",                    new CompositeMaterialAsciiChunkParser() );
		add( "ward_isotropic_material",               new WardIsotropicGaussianMaterialAsciiChunkParser() );
		add( "ward_anisotropic_material",             new WardAnisotropicEllipticalGaussianMaterialAsciiChunkParser() );
		add( "ggx_material",                          new GGXMaterialAsciiChunkParser() );
		add( "cooktorrance_material",                 new CookTorranceMaterialAsciiChunkParser() );
		add( "orennayar_material",                    new OrenNayarMaterialAsciiChunkParser() );
		add( "schlick_material",                      new SchlickMaterialAsciiChunkParser() );
		add( "datadriven_material",                   new DataDrivenMaterialAsciiChunkParser() );

		// Cameras
		add( "pinhole_camera",                        new PinholeCameraAsciiChunkParser() );
		add( "onb_pinhole_camera",                    new ONBPinholeCameraAsciiChunkParser() );
		add( "thinlens_camera",                       new ThinlensCameraAsciiChunkParser() );
		add( "realistic_camera",                      new RealisticCameraAsciiChunkParser() );
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
		add( "circulardisk_geometry",                 new CircularDiskGeometryAsciiChunkParser() );
		add( "bezierpatch_geometry",                  new BezierPatchGeometryAsciiChunkParser() );
		add( "bilinearpatch_geometry",                new BilinearPatchGeometryAsciiChunkParser() );
		add( "displaced_geometry",                    new DisplacedGeometryAsciiChunkParser() );

		// Modifiers
		add( "bumpmap_modifier",                      new BumpmapModifierAsciiChunkParser() );

		// Media
		add( "homogeneous_medium",                    new HomogeneousMediumAsciiChunkParser() );
		add( "heterogeneous_medium",                  new HeterogeneousMediumAsciiChunkParser() );
		add( "painter_heterogeneous_medium",          new PainterHeterogeneousMediumAsciiChunkParser() );

		// Objects
		add( "standard_object",                       new StandardObjectAsciiChunkParser() );
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
		add( "mlt_rasterizer",                        new MLTRasterizerAsciiChunkParser() );
		add( "mlt_spectral_rasterizer",               new MLTSpectralRasterizerAsciiChunkParser() );

		// Rasterizer output
		add( "file_rasterizeroutput",                 new FileRasterizerOutputAsciiChunkParser() );

		// Lights
		add( "ambient_light",                         new AmbientLightAsciiChunkParser() );
		add( "omni_light",                            new OmniLightAsciiChunkParser() );
		add( "spot_light",                            new SpotLightAsciiChunkParser() );
		add( "directional_light",                     new DirectionalLightAsciiChunkParser() );

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
		return Finalize( bag, pJob );
	}
}


//////////////////////////////////////////////////
// Implementation AsciiSceneParser itself
//////////////////////////////////////////////////
AsciiSceneParser::AsciiSceneParser( const char * szFilename_ )
{
	memset( szFilename, 0, 1024 );
	if( szFilename_ ) {
		strcpy( szFilename, GlobalMediaPathLocator().Find(szFilename_).c_str() );
	}

	// Populate the default macros
	macros["PI"] = PI;
	macros["E"] = E_;
}

AsciiSceneParser::~AsciiSceneParser( )
{
}

using namespace RISE::Implementation::ChunkParsers;

bool AsciiSceneParser::ParseAndLoadScene( IJob& pJob )
{
	Implementation::ChunkParsers::ClearParseState();

	// Build the dispatch map from the canonical parser registry.  The
	// parser_entries vector owns each chunk parser via unique_ptr for
	// the duration of this call; when it goes out of scope every parser
	// is destroyed automatically.  Same registry feeds SceneEditorSuggestions.
	std::vector<ChunkParserEntry> parser_entries = CreateAllChunkParsers();
	std::map<std::string, IAsciiChunkParser*> chunks;
	for( std::vector<ChunkParserEntry>::iterator it = parser_entries.begin(); it != parser_entries.end(); ++it ) {
		chunks[it->keyword] = it->parser.get();
	}

	// Open up the file and start parsing!
	struct stat file_stats = {0};
	stat( szFilename, &file_stats );
	unsigned int nSize = static_cast<unsigned int>(file_stats.st_size);

	// I realize this is ugly, but it is necessary for proper
	// clean up after breaking part way
	String strBuffer;
	strBuffer.resize( nSize + 1 );
	char* pBuffer = (char*)strBuffer.c_str();
	memset( pBuffer, 0, nSize + 1 );

	FILE* f = fopen( szFilename, "rb" );
	if( f ) {
		fread( pBuffer, nSize, 1, f );
		fclose( f );
	} else {
		GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Failed to load scene file \'%s\'", szFilename );
		return false;
	}

	std::istringstream		in( pBuffer );
	unsigned int			linenum = 0;

	// Command parser for parsing commands embedded in the scene
	AsciiCommandParser* parser = new AsciiCommandParser();
	GlobalLog()->PrintNew( parser, __FILE__, __LINE__, "command parser" );

	{
		char				line[MAX_CHARS_PER_LINE] = {0};		// <sigh>....

		// Verify version number
		in.getline( line, MAX_CHARS_PER_LINE );
		linenum++;

		// First check the first few characters to see if it contains our marker
		static const char* id = "RISE ASCII SCENE";
		if( strncmp( line, id, strlen(id) ) ) {
			GlobalLog()->Print( eLog_Error, "AsciiSceneParser: Scene does not contain RISE ASCII SCENE marker" );
			return false;
		}

		// Next find the scene version number
		const char* num = &line[strlen(id)];

		int version = atoi( num );

		if( version != CURRENT_SCENE_VERSION ) {
			GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Scene version problem, scene is version \'%d\', we require \'%d\'", version, CURRENT_SCENE_VERSION );
			return false;
		}
	}

	//
	// Parse the rest of the scene, basically read each line and see if
	//  we have a chunk, a comment or a command to pass to the command
	//  parser
	//

	std::stack<LOOP> loops;

	bool bInCommentBlock = false;
	for(;;) {
		char				line[MAX_CHARS_PER_LINE] = {0};		// <sigh>....
		in.getline( line, MAX_CHARS_PER_LINE );

		linenum++;

		if( in.fail() || in.eof() ) {
			break;
		}

		// Tokenize the string to get rid of comments etc
		String			tokens[1024];
		unsigned int numTokens = AsciiCommandParser::TokenizeString( line, tokens, 1024 );

		if( bInCommentBlock ) {
			if( tokens[0].size() >= 2 && tokens[0][0] == '*' && tokens[0][1] == '/' ) {
				bInCommentBlock = false;
			}
			continue;
		}

		if( numTokens == 0 ) {
			// Empty
			continue;
		}

		if( tokens[0][0] == '#' ) {
			// Comment
			continue;
		}

		if( tokens[0].size() >= 2 && tokens[0][0] == '/' && tokens[0][1] == '*' ) {
			// Comment block
			bInCommentBlock = true;
			continue;
		}

		if( tokens[0][0] == '>' ) {
			// Command
			if( !parser->ParseCommand( &tokens[1], numTokens-1, pJob ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Failed to parse line \'%s\' (%u)", line, linenum );
				return false;
			}
			continue;
		}

		// Check for a macro definition
		if( tokens[0][0] == '!' || tokens[0] == "DEFINE" || tokens[0] == "define" ) {
			// We have a macro
			if( numTokens < 3 ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Not enough parameters for macro definition line (%u)", linenum );
				return false;
			}

			if( !substitute_macros_in_tokens( tokens, numTokens ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error while performing macro subsitution on line %u", linenum );
				return false;
			}

			if( !evaluate_expressions_in_tokens( tokens, numTokens ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error while performing math expression evaluation %u", linenum );
				return false;
			}

			if( !add_macro( tokens[1], tokens[2] ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error adding new macro (%u)", linenum );
				return false;
			}
			continue;
		}

		// Check for macro removal
		if( tokens[0][0] == '~' || tokens[0] == "undef" || tokens[0] == "UNDEF" ) {
			if( numTokens < 2 ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Not enough parameters for macro removal line (%u)", linenum );
				return false;
			}

			if( !remove_macro( tokens[1] ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Couldn't find the macro to remove (%u)", linenum );
				return false;
			}
			continue;
		}

		// Check for loops
		if( tokens[0] == "FOR" ) {
			// loops require the following format
			// FOR <variable name> <start value> <end value> <increment size>
			if( numTokens < 5 ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Not enough paramters for loop line (%u)", linenum );
				return false;
			}

			// First check to see if the variable name is already in the macro map
			if( macros.find( tokens[1] ) != macros.end() ) {
				// Already there
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Variable \'%s\' already exists line (%u)", tokens[1].c_str(), linenum );
				return false;
			}

			if( !substitute_macros_in_tokens( tokens, numTokens ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error while performing macro subsitution on line %u", linenum );
				return false;
			}

			if( !evaluate_expressions_in_tokens( tokens, numTokens ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error while performing math expression evaluation %u", linenum );
				return false;
			}

			LOOP l;
            l.position = in.tellg();
			l.var = tokens[1];
			l.curvalue = atof( tokens[2].c_str() );
			l.endvalue = atof( tokens[3].c_str() );
			l.increment = atof( tokens[4].c_str() );
			l.linecount = linenum;

			macros[l.var] = l.curvalue;

			loops.push( l );
			continue;
		}

		// Check for loop end
		if( tokens[0] == "ENDFOR" ) {
            // We are at the end of the current loop
			if( loops.size() == 0 ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: LOOPEND found with no current loop, line (%u)", linenum );
			}

			LOOP& l = loops.top();
			l.curvalue += l.increment;

			MacroTable::iterator it = macros.find( l.var );

			if( l.curvalue > l.endvalue ) {
				// This loop is done, remove it from the queue and continue
				loops.pop();
				if( it == macros.end() ) {
					GlobalLog()->PrintEasyError( "AsciiSceneParser:: Fatal error in trying to remove loop variable" );
					return false;
				}
				macros.erase( it );
				continue;
			}

			// Otherwise, update the value in the macro list and continue
			if( it == macros.end() ) {
				GlobalLog()->PrintEasyError( "AsciiSceneParser:: Fatal error in trying to update loop variable" );
				return false;
			}

			it->second = l.curvalue;

			// Set the file back to the line this loop begins at and continue
			in.seekg( l.position );
			linenum = l.linecount;
			continue;
		}

		// Otherwise must be a chunk
		// Read the chunk type
		std::map<std::string,IAsciiChunkParser*>::iterator it = chunks.find( std::string(tokens[0].c_str()) );

		if( it == chunks.end() ) {
			GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Failed to find chunk \'%s\' on line %u", tokens[0].c_str(), linenum );
			return false;
		}

		const IAsciiChunkParser* pChunkParser = it->second;

		// Parse the '{'
		{
			in.getline( line, MAX_CHARS_PER_LINE );
			linenum++;
			if( in.fail() ) {
				GlobalLog()->PrintEasyError( "AsciiSceneParser::ParseScene:: Failed reading looking for '{' for chunk" );
				break;
			}

			String			toks[8];
			unsigned int numTokens = AsciiCommandParser::TokenizeString( line, toks, 8 );

			if( numTokens < 1 ) {
				return false;
			}

			if( toks[0][0] != '{' ) {
				GlobalLog()->PrintEasyError( "AsciiSceneParser::ParseScene:: Cannot find '{' for chunk" );
				return false;
			}

			// Keep reading the parameters for the chunk until we encounter the closing '}'
			IAsciiChunkParser::ParamsList chunkparams;
			for(;;) {
				in.getline( line, MAX_CHARS_PER_LINE );

				linenum++;
				if( in.fail() ) {
					GlobalLog()->PrintEasyError( "AsciiSceneParser::ParseScene:: Failed reading while reading chunk" );
					break;
				}

				// Don't bother reading comments or commands
				String			tokens[1024];
				unsigned int numTokens = AsciiCommandParser::TokenizeString( line, tokens, 1024 );

				if( numTokens < 1 ) {
					continue;
				}

				if( tokens[0][0] == '#' ) {
					continue;
				}

				if( tokens[0][0] == '>' ) {
					// We could optionally just parse the command...
					continue;
				}

				if( tokens[0][0] == '}' ) {
					// End of chunk, so break out
					break;
				}

				if( !substitute_macros_in_tokens( tokens, numTokens ) ) {
					GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error while performing macro subsitution on line %u", linenum );
				}

				if( !evaluate_expressions_in_tokens( tokens, numTokens ) ) {
					GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error while performing math expression evaluation %u", linenum );
				}

				// Otherwise, assemble the tokens and add it to the chunk list
				String s;
				make_string_from_tokens( s, tokens, numTokens, " " );
				chunkparams.push_back( s );
			}

			// Finished reading a chunk so parse it
			if( !pChunkParser->ParseChunk( chunkparams, pJob ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Failed to load chunk \'%s\' on line %u", tokens[0].c_str(), linenum );
				return false;
			}
		}
	}

	safe_release( parser );
	GlobalLog()->PrintEx( eLog_Info, "AsciiSceneParser: Successfully loaded \'%s\'", szFilename );

	// parser_entries unique_ptrs destroy the parsers when they go out of scope
	return true;
}

//////////////////////////////////////////////////
// Implementation of the macro substituion code
//////////////////////////////////////////////////

char AsciiSceneParser::substitute_macro( String& token )
{
	// A macro can be any part of a token
	std::string str( token.c_str() );
	std::string::size_type x = str.find_first_of( "@!" );

	std::string processed;

	if( x != std::string::npos ) {
		char macro_char = str[x];		// remember this, depending on whether its an @ or % we do different operations

		// We have a macro!
		if( x > 0 ) {
			processed = str.substr( 0, x );
		}
		str = str.substr( x+1, str.size() );

		// Find the end of the macro
		x = str.find_first_not_of( "ABCDEFGHIJKLMNOPQRSTUVWXYZ_" );

		std::string macro;
		if( x == std::string::npos ) {
			macro = str;
		} else {
			macro = str.substr( 0, x );
		}

		MacroTable::const_iterator it = macros.find( macro.c_str() );

		if( it == macros.end() ) {
			return 2;	// Error
		}

		// Re-assemble the string
		static const int MAX_BUF_SIZE = 64;
		char buf[MAX_BUF_SIZE] = {0};
		if( macro_char == '@' ) {
			snprintf( buf, MAX_BUF_SIZE, "%.12f", it->second );
		} else {
			snprintf( buf, MAX_BUF_SIZE, "%.4d", (int)it->second );
		}
		processed.append( buf );

		if( x<str.size() ) {
			processed.append( str.substr( x, str.size() ) );
		}

		token = processed.c_str();

		return 1;	// Successfull subsitution
	}

	return 0;	// No substituion
}

bool AsciiSceneParser::substitute_macros_in_tokens( String* tokens, const unsigned int num_tokens )
{
	for( unsigned int i=0; i<num_tokens; i++ ) {
		for(;;) {
			char c = substitute_macro( tokens[i] );

			if( c==0 ) {
				break;
			}

			if( c==2 ) {
				return false;
			}
		}
	}

	return true;
}

bool AsciiSceneParser::add_macro( String& name, String& value  )
{
	// Add a new macro
	std::string str( name.c_str() );

	// Make sure only valid things are in the macro
	if( str.find_first_not_of( "ABCDEFGHIJKLMNOPQRSTUVWXYZ_" ) != std::string::npos ) {
		return false;
	}

	// Check to see if it already exists
	if( macros.find( name ) != macros.end() ) {
		return false;
	}

	macros[name] = atof(value.c_str() );
	return true;
}

bool AsciiSceneParser::remove_macro( String& name )
{
	MacroTable::iterator it = macros.find( name );
	if( it == macros.end() ) {
		return false;
	}

	macros.erase( it );
	return true;
}
