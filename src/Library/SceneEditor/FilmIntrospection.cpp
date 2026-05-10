//////////////////////////////////////////////////////////////////////
//
//  FilmIntrospection.cpp
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FilmIntrospection.h"
#include "ChunkDescriptorRegistry.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/ILog.h"
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace RISE;

namespace
{
	String FormatUInt( unsigned int v )
	{
		char buf[32];
		std::snprintf( buf, sizeof(buf), "%u", v );
		return String( buf );
	}

	String FormatDouble( Scalar v )
	{
		char buf[64];
		std::snprintf( buf, sizeof(buf), "%.6g", v );
		return String( buf );
	}

	bool ParseUInt( const String& s, unsigned int& out )
	{
		// Use strtoll so we can reject (a) anything past UINT_MAX —
		// otherwise inputs like "4294967297" silently wrap to 1 in the
		// final cast and pass Job::SetFilm's kMaxFilmWidth bound — and
		// (b) partial-token parses like "12.5" or "1e3" which sscanf
		// would happily accept as 12 / 1.  Same pattern the RISE-CLI
		// flag parser uses (src/RISE/commandconsole.cpp:229).
		const char* start = s.c_str();
		char* endp = nullptr;
		const long long tmp = std::strtoll( start, &endp, 10 );
		if( endp == start || *endp != '\0' ) return false;
		if( tmp < 1 || tmp > static_cast<long long>( UINT_MAX ) ) return false;
		out = static_cast<unsigned int>( tmp );
		return true;
	}

	bool ParseDouble( const String& s, Scalar& out )
	{
		// Reject partial-token parses (e.g. "1.5abc" → 1.5) and empty
		// input by checking the strtod endpoint, mirroring ParseUInt.
		// NaN / inf gating is the caller's responsibility (SetProperty
		// applies std::isfinite for pixelAR after parse) since some
		// future double-typed param might legitimately accept ±inf.
		const char* start = s.c_str();
		char* endp = nullptr;
		out = std::strtod( start, &endp );
		return endp != start && *endp == '\0';
	}
}

std::vector<CameraProperty> FilmIntrospection::Inspect( const IFilm& film )
{
	std::vector<CameraProperty> out;

	const ChunkDescriptor* desc = DescriptorForKeyword( String( "film" ) );
	if( !desc ) {
		// FilmAsciiChunkParser is registered in CreateAllChunkParsers;
		// a missing descriptor here means the parser-wiring is broken.
		GlobalLog()->PrintEx( eLog_Error,
			"FilmIntrospection::Inspect: `film` descriptor not registered" );
		return out;
	}

	for( const ParameterDescriptor& p : desc->parameters )
	{
		CameraProperty row;
		row.name        = String( p.name.c_str() );
		row.kind        = p.kind;
		row.description = String( p.description.c_str() );
		row.editable    = true;
		// Forward optional presentation hints from the descriptor.
		// Matches the Camera / Object / Rasterizer / Light introspection
		// pattern so a future preset (e.g. resolution quick-picks for
		// `width`) or unit label surfaces in the GUI panel automatically.
		row.presets     = p.presets;
		row.unitLabel   = String( p.unitLabel.c_str() );

		const std::string n = p.name;
		if( n == "width" ) {
			row.value = FormatUInt( film.GetWidth() );
		}
		else if( n == "height" ) {
			row.value = FormatUInt( film.GetHeight() );
		}
		else if( n == "pixelAR" ) {
			row.value = FormatDouble( film.GetPixelAR() );
		}
		else {
			// Unknown descriptor param — surface as a read-only row
			// with empty value rather than dropping it silently.  A
			// developer who adds a new param to FilmAsciiChunkParser
			// without updating this switch will see the row in the
			// panel and notice it's not getting a value.
			row.value    = String();
			row.editable = false;
		}
		out.push_back( row );
	}

	return out;
}

bool FilmIntrospection::SetProperty( IJobPriv& job,
                                     const String& name,
                                     const String& value )
{
	const IScene* scene = job.GetScene();
	if( !scene ) return false;
	const IFilm* film = scene->GetFilm();
	if( !film ) return false;

	// Read the current Film state so the missing fields stay put.
	unsigned int w = film->GetWidth();
	unsigned int h = film->GetHeight();
	double pAR     = film->GetPixelAR();

	const std::string n( name.c_str() );
	if( n == "width" ) {
		unsigned int v = 0;
		if( !ParseUInt( value, v ) ) return false;
		w = v;
	}
	else if( n == "height" ) {
		unsigned int v = 0;
		if( !ParseUInt( value, v ) ) return false;
		h = v;
	}
	else if( n == "pixelAR" ) {
		Scalar v = 0;
		if( !ParseDouble( value, v ) ) return false;
		// std::sscanf("%lf") parses "nan", "inf", "+inf", "-inf" — gate
		// against them explicitly.  `NaN <= 0.0` is false, so a plain
		// `v <= 0.0` check lets NaN slip past and poison every camera's
		// projection matrix on the resync.
		if( !std::isfinite( v ) || v <= 0.0 ) return false;
		pAR = v;
	}
	else {
		return false;
	}

	// Job::SetFilm walks the camera manager and resyncs every camera's
	// projection in lock-step, so cameras can never silently desync
	// from Film dims — see docs/SCENE_CONVENTIONS.md §8.5.
	return job.SetFilm( w, h, pAR );
}

String FilmIntrospection::GetPropertyValue( const IFilm& film, const String& name )
{
	const std::string n( name.c_str() );
	if( n == "width" )   return FormatUInt( film.GetWidth() );
	if( n == "height" )  return FormatUInt( film.GetHeight() );
	if( n == "pixelAR" ) return FormatDouble( film.GetPixelAR() );
	return String();
}
