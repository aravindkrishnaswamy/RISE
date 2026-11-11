//////////////////////////////////////////////////////////////////////
//
//  CliOptions.h - Post-load CLI override parser and applier shared by
//    commandconsole.cpp and tests/CliOverridesTest.cpp.
//
//  Supports:
//    --width <N>        positive integer film width in pixels
//    --height <N>       positive integer film height in pixels
//    --pixel-ar <X>     positive finite float pixel aspect ratio
//    --denoise          force OIDN post-process denoising ON
//    --no-denoise       force OIDN post-process denoising OFF
//    --camera <name>    switch active scene camera by name (also --camera=<name>)
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_CLI_OPTIONS_H_
#define RISE_CLI_OPTIONS_H_

#include <cstdlib>
#include <cstring>
#include <ostream>
#include <string>
#include <vector>

#include "Interfaces/ICameraManager.h"
#include "Interfaces/IFilm.h"
#include "Interfaces/IJobPriv.h"
#include "Interfaces/IRasterizer.h"
#include "Interfaces/IScene.h"
#include "Utilities/FiniteMath.h"
#include "Utilities/Log/Log.h"

namespace RISE
{
	namespace Cli
	{
		struct PostLoadCliOverrides
		{
			int         filmWidth       = -1;
			int         filmHeight      = -1;
			double      filmPixelAR     = -1.0;
			int         denoiseMode     = -1;   //!< -1 = scene default, 0 = --no-denoise, 1 = --denoise
			bool        hasCamera       = false;
			std::string cameraName;

			bool HasFilmOverride() const
			{
				return ( filmWidth > 0 ) || ( filmHeight > 0 ) || ( filmPixelAR > 0.0 );
			}

			bool HasAnyOverride() const
			{
				return HasFilmOverride() || ( denoiseMode >= 0 ) || hasCamera;
			}
		};

		enum class FlagMatch
		{
			NotMatched,
			MatchedOk,
			MatchedError
		};

		//! Attempts to consume one post-load CLI override flag at `argv[ai]`
		//! (advancing `ai` when a space-separated value token is consumed).
		inline FlagMatch TryConsumePostLoadCliFlag(
			int argc,
			char** argv,
			int& ai,
			PostLoadCliOverrides& dst,
			std::ostream& err )
		{
			const char* a = argv[ai];
			if( !a ) return FlagMatch::NotMatched;

			auto consumePositiveInt = [&]( const char* name, int& out ) -> FlagMatch {
				if( ai + 1 >= argc ) {
					err << "ERROR: " << name << " requires a positive integer; got end-of-args.\n";
					return FlagMatch::MatchedError;
				}
				char* endp = nullptr;
				const long v = std::strtol( argv[ai + 1], &endp, 10 );
				if( endp == argv[ai + 1] || *endp != '\0' || v <= 0 ) {
					err << "ERROR: " << name << " requires a positive integer; got `" << argv[ai + 1] << "`.\n";
					return FlagMatch::MatchedError;
				}
				out = static_cast<int>( v );
				++ai;
				return FlagMatch::MatchedOk;
			};

			auto consumePositiveDouble = [&]( const char* name, double& out ) -> FlagMatch {
				if( ai + 1 >= argc ) {
					err << "ERROR: " << name << " requires a positive float; got end-of-args.\n";
					return FlagMatch::MatchedError;
				}
				char* endp = nullptr;
				const double v = std::strtod( argv[ai + 1], &endp );
				if( endp == argv[ai + 1] || *endp != '\0' || !RISE::IsFiniteDouble( v ) || v <= 0.0 ) {
					err << "ERROR: " << name << " requires a positive float; got `" << argv[ai + 1] << "`.\n";
					return FlagMatch::MatchedError;
				}
				out = v;
				++ai;
				return FlagMatch::MatchedOk;
			};

			if( std::strcmp( a, "--width" ) == 0 ) {
				return consumePositiveInt( "--width", dst.filmWidth );
			}
			if( std::strcmp( a, "--height" ) == 0 ) {
				return consumePositiveInt( "--height", dst.filmHeight );
			}
			if( std::strcmp( a, "--pixel-ar" ) == 0 ) {
				return consumePositiveDouble( "--pixel-ar", dst.filmPixelAR );
			}
			if( std::strcmp( a, "--denoise" ) == 0 ) {
				if( dst.denoiseMode == 0 ) {
					err << "ERROR: --denoise and --no-denoise are mutually exclusive; pick one.\n";
					return FlagMatch::MatchedError;
				}
				dst.denoiseMode = 1;
				return FlagMatch::MatchedOk;
			}
			if( std::strncmp( a, "--denoise=", 10 ) == 0 ) {
				err << "ERROR: --denoise takes no `=value` argument; use `--denoise` or `--no-denoise`.\n";
				return FlagMatch::MatchedError;
			}
			if( std::strcmp( a, "--no-denoise" ) == 0 ) {
				if( dst.denoiseMode == 1 ) {
					err << "ERROR: --denoise and --no-denoise are mutually exclusive; pick one.\n";
					return FlagMatch::MatchedError;
				}
				dst.denoiseMode = 0;
				return FlagMatch::MatchedOk;
			}
			if( std::strncmp( a, "--no-denoise=", 13 ) == 0 ) {
				err << "ERROR: --no-denoise takes no `=value` argument; use `--denoise` or `--no-denoise`.\n";
				return FlagMatch::MatchedError;
			}
			if( std::strcmp( a, "--camera" ) == 0 ) {
				if( ai + 1 >= argc || !argv[ai + 1] || argv[ai + 1][0] == '\0' || argv[ai + 1][0] == '-' ) {
					err << "ERROR: --camera requires a non-empty camera name; got "
					    << ( ai + 1 >= argc ? "end-of-args" : argv[ai + 1] ) << ".\n";
					return FlagMatch::MatchedError;
				}
				dst.hasCamera = true;
				dst.cameraName = argv[++ai];
				return FlagMatch::MatchedOk;
			}
			if( std::strncmp( a, "--camera=", 9 ) == 0 ) {
				const char* val = a + 9;
				if( !val[0] ) {
					err << "ERROR: --camera= requires a non-empty camera name after '='.\n";
					return FlagMatch::MatchedError;
				}
				dst.hasCamera = true;
				dst.cameraName = val;
				return FlagMatch::MatchedOk;
			}

			return FlagMatch::NotMatched;
		}

		//! Applies parsed post-load CLI overrides (`--camera`, `--width`/`--height`/`--pixel-ar`,
		//! `--denoise`/`--no-denoise`) to `pJob` after scene loading.  Shared between
		//! Windows and POSIX branches of commandconsole.cpp.
		inline bool ApplyPostLoadCliOverrides(
			IJobPriv* pJob,
			const PostLoadCliOverrides& ov,
			std::ostream& err )
		{
			if( !ov.HasAnyOverride() ) return true;
			if( !pJob || !pJob->GetScene() ) {
				err << "ERROR: cannot apply CLI overrides without a loaded scene.\n";
				return false;
			}

			bool ok = true;

			if( ov.hasCamera ) {
				if( !pJob->SetActiveCamera( ov.cameraName.c_str() ) ) {
					struct NameCollector : public IEnumCallback<const char*>
					{
						std::vector<std::string> names;
						bool operator()( const char* const& nm )
						{
							if( nm && *nm ) names.push_back( nm );
							return true;
						}
					} collector;
					if( pJob->GetCameras() ) {
						pJob->GetCameras()->EnumerateItemNames( collector );
					}
					err << "ERROR: --camera `" << ov.cameraName << "` was not found in scene";
					if( collector.names.empty() ) {
						err << " (scene defines no named cameras).\n";
					} else {
						err << " (available: ";
						for( std::size_t i = 0; i < collector.names.size(); ++i ) {
							if( i > 0 ) err << ", ";
							err << collector.names[i];
						}
						err << ").\n";
					}
					GlobalLog()->PrintEx( eLog_Error,
						"CLI --camera override failed: camera `%s` not found.", ov.cameraName.c_str() );
					ok = false;
				}
			}

			if( ov.HasFilmOverride() ) {
				const IFilm* curFilm = pJob->GetScene()->GetFilm();
				const unsigned int w   = ( ov.filmWidth   > 0 ) ? static_cast<unsigned int>( ov.filmWidth )  : ( curFilm ? curFilm->GetWidth()   : 960u );
				const unsigned int h   = ( ov.filmHeight  > 0 ) ? static_cast<unsigned int>( ov.filmHeight ) : ( curFilm ? curFilm->GetHeight()  : 540u );
				const double       pAR = ( ov.filmPixelAR > 0.0 ) ? ov.filmPixelAR                           : ( curFilm ? curFilm->GetPixelAR() : 1.0 );
				if( !pJob->SetFilm( w, h, pAR ) ) {
					err << "ERROR: CLI Film override failed (invalid width=" << w
					    << ", height=" << h << ", pixelAR=" << pAR << ").\n";
					GlobalLog()->PrintEasyError(
						"CLI Film override failed (invalid width/height/pixelAR)." );
					ok = false;
				}
			}

			if( ov.denoiseMode >= 0 ) {
				IRasterizer* pRas = pJob->GetRasterizer();
				if( !pRas ) {
					err << "ERROR: " << ( ov.denoiseMode ? "--denoise" : "--no-denoise" )
					    << " cannot be applied because the scene defines no rasterizer.\n";
					GlobalLog()->PrintEasyError(
						"CLI denoise override failed: scene defines no rasterizer." );
					ok = false;
				} else if( !pRas->SetDenoisingOverride( ov.denoiseMode != 0 ) ) {
					err << "ERROR: " << ( ov.denoiseMode ? "--denoise" : "--no-denoise" )
					    << " is not supported by the active rasterizer or build configuration.\n";
					GlobalLog()->PrintEasyError(
						"CLI denoise override failed: OIDN denoising unsupported by active rasterizer/build." );
					ok = false;
				}
			}

			return ok;
		}
	}
}

#endif
