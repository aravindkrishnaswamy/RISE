//////////////////////////////////////////////////////////////////////
//
//  AsciiCommandParser.cpp - Implementation of the ascii
//    command parser
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 19, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include <string>
#include <cerrno>
#include <cmath>
#include <limits>
#include <cctype>
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include "AsciiCommandParser.h"
#include "StdOutProgress.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/RTime.h"
#include "../Utilities/MediaPathLocator.h"
#include "../Utilities/RandomNumbers.h"

static const unsigned int RISE_MAX_TOKENS_PER_LINE = 1024;
static const char RISE_COMMENT = '#';

using namespace RISE;
using namespace RISE::Implementation;

AsciiCommandParser::AsciiCommandParser() = default;
AsciiCommandParser::~AsciiCommandParser() = default;

namespace
{
	template<class Consumer>
	unsigned int ReadCommandTokens( std::string_view line, unsigned int limit, Consumer consume )
	{
		unsigned int count = 0;
		while( count < limit ) {
			const auto first = line.find_first_not_of( " \t\r" );
			if( first == std::string_view::npos ) break;
			line.remove_prefix( first );
			if( line.front() == '"' ) {
				const auto close = line.find( '"', 1 );
				if( close != std::string_view::npos ) {
					consume( line.substr( 1, close - 1 ) );
					++count;
					line.remove_prefix( close + 1 );
					continue;
				}
			}
			const auto end = line.find_first_of( " \t\r" );
			consume( line.substr( 0, end ) );
			++count;
			if( end == std::string_view::npos ) break;
			line.remove_prefix( end + 1 );
		}
		return count;
	}

	// Preserve decimal prefixes and explicit signs accepted by the old console.
	bool ReadUnsigned( const std::string& token, unsigned int& result )
	{
		const char* begin = token.c_str();
		while( std::isspace( static_cast<unsigned char>(*begin) ) ) ++begin;
		const bool negative = *begin == '-';
		if( negative || *begin == '+' ) ++begin;
		if( std::isdigit( static_cast<unsigned char>(*begin) ) ) {
			char* end = nullptr;
			errno = 0;
			const auto value = std::strtoull( begin, &end, 10 );
			if( errno != ERANGE && value <= std::numeric_limits<unsigned int>::max() ) {
				result = static_cast<unsigned int>(value);
				if( negative ) result = 0U - result;
				return true;
			}
		}
		GlobalLog()->PrintEx( eLog_Error, "Expected an unsigned number, got '%s'", token.c_str() );
		return false;
	}

	bool ReadDouble( const std::string& token, double& result )
	{
		char* end = nullptr;
		const double value = std::strtod( token.c_str(), &end );
		if( end != token.c_str() && std::isfinite(value) ) {
			result = value;
			return true;
		}
		GlobalLog()->PrintEx( eLog_Error, "Expected a finite number, got '%s'", token.c_str() );
		return false;
	}

	bool ReadBoolean( const std::string& token )
	{
		return std::toupper( static_cast<unsigned char>(token.c_str()[0]) ) == 'T';
	}

	bool DispatchCommand( const std::string* tokens, unsigned int count, IJob& job, AsciiCommandParser* parser )
	{
		if( count == 0 ) return false;
		using Command = bool (*)(const std::string*, unsigned int, IJob&, AsciiCommandParser*);
		static constexpr std::pair<std::string_view, Command> commands[] = {
			{"set", AsciiCommandParser::ParseSet},
			{"remove", AsciiCommandParser::ParseRemove},
			{"modify", AsciiCommandParser::ParseModify},
			{"clearall", AsciiCommandParser::ParseClearAll},
			{"predict", AsciiCommandParser::ParsePredictRasterizationTime},
			{"render", AsciiCommandParser::ParseRasterize},
			{"renderanimation", AsciiCommandParser::ParseRasterizeAnimation},
			{"load", AsciiCommandParser::ParseLoad},
			{"photonmap", AsciiCommandParser::ParsePhotonMap},
			{"run", AsciiCommandParser::ParseRun},
			{"quit", AsciiCommandParser::ParseQuit},
			{"mediapath", AsciiCommandParser::ParseMediaPath},
			{"echo", AsciiCommandParser::ParseEcho}
		};
		for( const auto& command : commands ) {
			if( tokens[0] == command.first ) return command.second( tokens + 1, count - 1, job, parser );
		}
		return false;
	}
}

unsigned int AsciiCommandParser::TokenizeString( const char* line, String* tokens, unsigned int maxTokens )
{
	if( !line || !tokens ) return 0;
	return ReadCommandTokens( line, maxTokens, [&]( std::string_view token ) {
		*tokens++ = std::string(token).c_str();
	} );
}

std::vector<std::string> AsciiCommandParser::Tokenize( std::string_view line )
{
	std::vector<std::string> tokens;
	ReadCommandTokens( line, RISE_MAX_TOKENS_PER_LINE,
		[&tokens]( std::string_view token ) { tokens.emplace_back( token ); } );
	return tokens;
}

bool AsciiCommandParser::ParseCommand( const char* line, IJob& job )
{
	if( !line ) return false;
	if( !*line ) return true;
	const auto tokens = Tokenize( line );
	if( !tokens.empty() && tokens[0].c_str()[0] == RISE_COMMENT ) return true;
	return DispatchCommand( tokens.data(), static_cast<unsigned int>(tokens.size()), job, this );
}

bool AsciiCommandParser::ParseCommand( String* tokens, unsigned int count, IJob& job )
{
	if( !tokens || count == 0 ) return false;
	std::vector<std::string> owned;
	owned.reserve( count );
	for( unsigned int i = 0; i < count; ++i ) owned.emplace_back( tokens[i].c_str() );
	return DispatchCommand( owned.data(), count, job, this );
}

bool AsciiCommandParser::ParseClearAll( const std::string*, unsigned int, IJob& pJob, AsciiCommandParser* )
{
	return pJob.ClearAll();
}

bool AsciiCommandParser::ParseLoad( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* )
{
	// Loads an ascii scene file
	if( num_tokens < 1 ) {
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseLoad: No file specified to load" );
		return false;
	}

	// Slice 6c-3c: the interactive console `load <file>` command routes through the CST-only Auto loader
	// (the legacy streaming Job::LoadAsciiScene was deleted).  Native-v7 scenes load via the CST path; an
	// un-migrated scene hard-fails with an actionable diagnostic pointing at the offline migrator.
	return pJob.LoadAsciiSceneAuto( tokens[0].c_str() );
}

bool AsciiCommandParser::ParseRun( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise )
{
	// Loads an ascii script file
	if( num_tokens < 1 ) {
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseLoad: No file specified to load" );
		return false;
	}

	return pJob.RunAsciiScript( tokens[0].c_str() );
}

bool AsciiCommandParser::ParseQuit( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise )
{
	GlobalLog()->Print( eLog_Warning, "Bye!" );
	exit(1);
	return true;
}

bool ParseSetAccelerator( const std::string* tokens, unsigned int num_tokens, IJob& pJob )
{
	// Check for the correct number of parameters
	if( num_tokens < 3 )
	{
		GlobalLog()->PrintEx( eLog_Error, "AsciiCommandParser::ParseSetAccelerator: Wrong number of parameters: %d", num_tokens );
		return false;
	}

	bool bOctree = false;
	bool bBSP = false;
	unsigned int nMaxObj;
	unsigned int nMaxRecur;

	char c = std::toupper(static_cast<unsigned char>(tokens[0].c_str()[0]));
	if( c == 'B' ) {
		bBSP = true;
	} else if( c == 'O' ) {
		bOctree = true;
	}

	if( !ReadUnsigned( tokens[1], nMaxObj ) || !ReadUnsigned( tokens[2], nMaxRecur ) ) return false;

	return pJob.SetPrimaryAcceleration( bBSP, bOctree, nMaxObj, nMaxRecur );
}

bool AsciiCommandParser::ParseSet( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser*)
{
	// We are to add some element into the engine
	if( num_tokens < 1 )
	{
		// But no element was specified!
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseSet: No element to add" );
		return false;
	}

	// Otherwise just do strcmps to find out what we add
	if( tokens[0] == "accelerator" ) {
		return ParseSetAccelerator( &tokens[1], num_tokens-1, pJob );
	}

	if( tokens[0] == "light_rr_threshold" ) {
		if( num_tokens < 2 ) {
			GlobalLog()->Print( eLog_Error, "AsciiCommandParser::ParseSet: light_rr_threshold requires a value" );
			return false;
		}
		double threshold;
		return ReadDouble( tokens[1], threshold ) && pJob.SetLightSampleRRThreshold( threshold );
	}

	if( tokens[0] == "global_medium" ) {
		if( num_tokens < 2 ) {
			GlobalLog()->Print( eLog_Error, "AsciiCommandParser::ParseSet: global_medium requires a medium name" );
			return false;
		}
		return pJob.SetGlobalMedium( tokens[1].c_str() );
	}

	GlobalLog()->PrintEx( eLog_Error, "AsciiCommandParser::ParseSet: Unknown element type: %s", tokens[0].c_str() );
	return false;
}

bool ParseModifyObject_UV_Spherical( const std::string* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 2 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseModifyObject_UV_Spherical: not enough parameters" );
		return false;
	}

	double radius;
	if( !ReadDouble( tokens[1], radius ) ) return false;

	return pJob.SetObjectUVToSpherical( tokens[0].c_str(), radius );
}

bool ParseModifyObject_UV_Box( const std::string* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 4 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseModifyObject_UV_Box: not enough parameters" );
		return false;
	}

	double width, height, depth;
	if( !ReadDouble( tokens[1], width ) || !ReadDouble( tokens[2], height ) ||
		!ReadDouble( tokens[3], depth ) ) return false;

	return pJob.SetObjectUVToBox( tokens[0].c_str(), width, height, depth );
}

bool ParseModifyObject_UV_Cylindrical( const std::string* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 4 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseModifyObject_UV_Cylindrical: not enough parameters" );
		return false;
	}

	const char axis = tokens[2].c_str()[0];
	if( tokens[2].size() != 1 || (axis != 'x' && axis != 'y' && axis != 'z') ) return false;
	double radius, size;
	if( !ReadDouble( tokens[1], radius ) || !ReadDouble( tokens[3], size ) ) return false;

	return pJob.SetObjectUVToCylindrical( tokens[0].c_str(), radius, axis, size );
}

bool ParseModifyObject_UV( const std::string* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseModifyObject_UV: not enough parameters" );
		return false;
	}

	if( tokens[0] == "spherical" )
	{
		return ParseModifyObject_UV_Spherical( &tokens[1], num_tokens-1, pJob );
	}
	else if( tokens[0] == "box" )
	{
		return ParseModifyObject_UV_Box( &tokens[1], num_tokens-1, pJob );
	}
	else if( tokens[0] == "cylindrical" )
	{
		return ParseModifyObject_UV_Cylindrical( &tokens[1], num_tokens-1, pJob );
	}
	else
	{
		GlobalLog()->PrintEx( eLog_Warning, "AsciiCommandParser::ParseModifyObject_UV: Unknown mapping type: %s", tokens[0].c_str() );
		return false;
	}
}

bool ParseModifyObject( const std::string* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseModifyObject: no object specifed or no type to modify" );
		return false;
	}

	// Find out what about the object we are changing.
	//
	// Two grammars coexist here.  The legacy `error` / `uv` forms are
	// TYPE-FIRST (`modify object error <name> <value>`), so they match on
	// tokens[0].  The newer `material` / `shader` forms are NAME-FIRST
	// (`modify object <name> material <materialName>`), so they match the
	// keyword on tokens[1].  Checking tokens[0] first preserves the legacy
	// dispatch; the name-first forms are tried next.
	if( tokens[0] == "error" )
	{
		if( num_tokens < 3 )
		{
			GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseModifyObject: no error level specifed!" );
			return false;
		}

		double err;
		if( !ReadDouble( tokens[2], err ) ) return false;

		return pJob.SetObjectIntersectionError( tokens[1].c_str(), err );
	}
	else if( tokens[0] == "uv" )
	{
		// The user is specifying an over-riding UV generator
		return ParseModifyObject_UV( &tokens[1], num_tokens-1, pJob );
	}

	// Name-first forms: `modify object <name> material|shader <name2>`.
	if( num_tokens >= 3 )
	{
		if( tokens[1] == "material" )
		{
			return pJob.SetObjectMaterial( tokens[0].c_str(), tokens[2].c_str() );
		}
		else if( tokens[1] == "shader" )
		{
			return pJob.SetObjectShader( tokens[0].c_str(), tokens[2].c_str() );
		}
	}

	GlobalLog()->PrintEx( eLog_Error, "AsciiCommandParser::ParseModifyObject: Unknown modification target for object: %s", tokens[0].c_str() );
	return false;
}

//! `> modify material <name> scale <value>` — rescales a luminaire
//! material's emission.
bool ParseModifyMaterial( const std::string* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 3 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseModifyMaterial: usage: modify material <name> scale <value>" );
		return false;
	}

	if( tokens[1] == "scale" )
	{
		double scale;
		if( !ReadDouble( tokens[2], scale ) ) return false;
		return pJob.SetMaterialEmissionScale( tokens[0].c_str(), scale );
	}

	GlobalLog()->PrintEx( eLog_Error, "AsciiCommandParser::ParseModifyMaterial: Unknown material modification target: %s", tokens[1].c_str() );
	return false;
}

//! `> modify rasterizer radiance_scale <value>` — overrides the active
//! rasterizer's environment radiance scale.
bool ParseModifyRasterizer( const std::string* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 2 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseModifyRasterizer: usage: modify rasterizer radiance_scale <value>" );
		return false;
	}

	if( tokens[0] == "radiance_scale" )
	{
		double scale;
		if( !ReadDouble( tokens[1], scale ) ) return false;
		return pJob.SetActiveRasterizerRadianceScale( scale );
	}

	GlobalLog()->PrintEx( eLog_Error, "AsciiCommandParser::ParseModifyRasterizer: Unknown rasterizer modification target: %s", tokens[0].c_str() );
	return false;
}

//! `> modify animation <name>` — selects the active named animation
//! (the one rendered by `renderanimation` / scrubbed in the GUI).
bool ParseModifyAnimation( const std::string* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseModifyAnimation: usage: modify animation <name>" );
		return false;
	}

	return pJob.SetActiveAnimation( tokens[0].c_str() );
}

bool AsciiCommandParser::ParseModify( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* )
{
	// We are to modify some existing item
	if( num_tokens < 1 )
	{
		// But no element type was specified!
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseModify: No element type to modify" );
		return false;
	}

	// Otherwise just do strcmps to find out what to modify
	if( tokens[0] == "object" ) {
		return ParseModifyObject( &tokens[1], num_tokens-1, pJob );
	}
	else if( tokens[0] == "material" ) {
		return ParseModifyMaterial( &tokens[1], num_tokens-1, pJob );
	}
	else if( tokens[0] == "rasterizer" ) {
		return ParseModifyRasterizer( &tokens[1], num_tokens-1, pJob );
	}
	else if( tokens[0] == "animation" ) {
		return ParseModifyAnimation( &tokens[1], num_tokens-1, pJob );
	}

	GlobalLog()->PrintEx( eLog_Error, "AsciiCommandParser::ParseModify: Unknown element type: %s", tokens[0].c_str() );
	return false;
}




bool AsciiCommandParser::ParseRemovePainter( const std::string* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseRemovePainter: No painter to remove" );
		return false;
	}

	return pJob.RemovePainter( tokens[0].c_str() );
}

bool AsciiCommandParser::ParseRemoveMaterial( const std::string* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseRemoveMaterial: No Material to remove" );
		return false;
	}

	return pJob.RemoveMaterial( tokens[0].c_str() );
}

bool AsciiCommandParser::ParseRemoveGeometry( const std::string* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseRemoveGeometry: No geometry to remove" );
		return false;
	}

	return pJob.RemoveGeometry( tokens[0].c_str() );
}

bool AsciiCommandParser::ParseRemoveObject( const std::string* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseRemoveObject: No object to remove" );
		return false;
	}

	return pJob.RemoveObject( tokens[0].c_str() );
}

bool AsciiCommandParser::ParseRemoveLight( const std::string* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseRemoveLight: No light to remove" );
		return false;
	}

	return pJob.RemoveLight( tokens[0].c_str() );
}

bool AsciiCommandParser::ParseRemoveModifier( const std::string* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseRemoveModifier: No modifier to remove" );
		return false;
	}

	return pJob.RemoveModifier( tokens[0].c_str() );
}

bool AsciiCommandParser::ParseRemoveRasterizerOutputs( const std::string* tokens, unsigned int num_tokens, IJob& pJob )
{
	return pJob.RemoveRasterizerOutputs();
}

bool AsciiCommandParser::ParseRemove( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* )
{
	// We are to add some element into the engine
	if( num_tokens < 1 )
	{
		// But no object was specified!
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseRemove: No element to remove" );
		return false;
	}

	// Otherwise just do strcmps to find out what we add
	if( tokens[0] == "painter" )
	{
		return ParseRemovePainter( &tokens[1], num_tokens-1, pJob );
	}
	else if( tokens[0] == "material" )
	{
		return ParseRemoveMaterial( &tokens[1], num_tokens-1, pJob );
	}
	else if( tokens[0] == "geometry" )
	{
		return ParseRemoveGeometry( &tokens[1], num_tokens-1, pJob );
	}
	else if( tokens[0] == "object" )
	{
		return ParseRemoveObject( &tokens[1], num_tokens-1, pJob );
	}
	else if( tokens[0] == "light" )
	{
		return ParseRemoveLight( &tokens[1], num_tokens-1, pJob );
	}
	else if( tokens[0] == "modifier" )
	{
		return ParseRemoveModifier( &tokens[1], num_tokens-1, pJob );
	} 
	else if( tokens[0] == "rasterizeroutputs" )
	{
		return ParseRemoveRasterizerOutputs( &tokens[1], num_tokens-1, pJob );
	} 
	else if( tokens[0] == "all" )
	{
		return pJob.ClearAll();
	}
	
	GlobalLog()->PrintEx( eLog_Error, "AsciiCommandParser::ParseRemove: Unknown element type: %s", tokens[0].c_str() );
	return false;
}

static void LogRasterizationTime( const char* label, unsigned int milliseconds )
{
	using namespace std::chrono;
	auto remaining = std::chrono::milliseconds( milliseconds );
	std::string message = label;
	const auto append = [&]( auto unit, const char* name ) {
		const auto count = duration_cast<decltype(unit)>(remaining);
		remaining -= count;
		if( count.count() == 0 ) return;
		message += std::to_string( count.count() );
		message += name;
	};
	using Days = duration<int, std::ratio<86400>>;
	append( Days(1), " days " );
	append( hours(1), " hours " );
	append( minutes(1), " minutes " );
	append( seconds(1), " seconds " );
	append( std::chrono::milliseconds(1), " ms" );
	message += '\n';
	GlobalLog()->Print( eLog_Benign, message.c_str() );
}

bool AsciiCommandParser::ParsePredictRasterizationTime( const std::string*, unsigned int, IJob& job, AsciiCommandParser* )
{
	unsigned int predicted = 0;
	if( !job.PredictRasterizationTime( 4096, &predicted, nullptr ) ) return false;
	LogRasterizationTime( "Predicted Rasterization Time: ", predicted );
	return true;
}

bool AsciiCommandParser::ParseRasterize( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* )
{
	if( num_tokens != 0 && num_tokens < 4 ) {
		GlobalLog()->PrintEasyError( "Usage: render [left right top bottom]" );
		return false;
	}
	unsigned int left = 0, right = 0, top = 0, bottom = 0;
	if( num_tokens >= 4 && (!ReadUnsigned( tokens[0], left ) || !ReadUnsigned( tokens[1], right ) ||
		!ReadUnsigned( tokens[2], top ) || !ReadUnsigned( tokens[3], bottom )) ) return false;

	std::chrono::steady_clock::time_point tStart;
	std::chrono::steady_clock::time_point tEnd;
	bool bRet = true;
	
	StdOutProgress	progress( "Rasterizing Scene: " );	
	pJob.SetProgress( &progress );
	if( num_tokens >= 4 )
	{


		tStart = std::chrono::steady_clock::now();
		bRet = pJob.RasterizeRegion(left, top, right, bottom);
		tEnd = std::chrono::steady_clock::now();

	} else {

		tStart = std::chrono::steady_clock::now();
		bRet = pJob.Rasterize();
		tEnd = std::chrono::steady_clock::now();
	}

	pJob.SetProgress( 0 );

	LogRasterizationTime( "Total Rasterization Time: ", ElapsedMilliseconds( tStart, tEnd ) );

	return bRet;
}

bool AsciiCommandParser::ParseRasterizeAnimation( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* )
{
	double time_start=0, time_end=1.0;
	unsigned int frames=30;

	if( num_tokens >= 3 ) {
		if( !ReadDouble( tokens[0], time_start ) || !ReadDouble( tokens[1], time_end ) ||
			!ReadUnsigned( tokens[2], frames ) ) return false;
	}

	const bool do_fields = num_tokens >= 4 && ReadBoolean( tokens[3] );
	const bool invert_fields = num_tokens >= 5 && ReadBoolean( tokens[4] );
	unsigned int specificFrame = 0;
	if( num_tokens >= 1 && num_tokens < 3 && !ReadUnsigned( tokens[0], specificFrame ) ) return false;

	bool bRet = true;

	StdOutProgress	progress( "Rasterizing Scene: " );	
	pJob.SetProgress( &progress );
	
	const auto tStart = std::chrono::steady_clock::now();
	if( num_tokens >= 3 ) {
		bRet = pJob.RasterizeAnimation(time_start, time_end, frames, do_fields, invert_fields );
	} else {
		if( num_tokens >= 1 ) {
			bRet = pJob.RasterizeAnimationUsingOptions( specificFrame );
		} else {
			bRet = pJob.RasterizeAnimationUsingOptions( );
		}
	}
	const auto tEnd = std::chrono::steady_clock::now();

	pJob.SetProgress( 0 );

	LogRasterizationTime( "Total Rasterization Time: ", ElapsedMilliseconds( tStart, tEnd ) );

	return bRet;
}

bool AsciiCommandParser::ParsePhotonMap( const std::string* tokens, unsigned int count, IJob& job, AsciiCommandParser* )
{
	if( count < 3 || tokens[2].empty() ) {
		GlobalLog()->PrintEasyError( "Usage: photonmap <type> <save|load> <file>" );
		return false;
	}
	using Action = bool (IJob::*)(const char*);
	struct Map {
		std::string_view name;
		Action save;
		Action load;
	};
	static constexpr Map maps[] = {
		{"caustic", &IJob::SaveCausticPelPhotonmap, &IJob::LoadCausticPelPhotonmap},
		{"global", &IJob::SaveGlobalPelPhotonmap, &IJob::LoadGlobalPelPhotonmap},
		{"translucent", &IJob::SaveTranslucentPelPhotonmap, &IJob::LoadTranslucentPelPhotonmap},
		{"spectral_caustic", &IJob::SaveCausticSpectralPhotonmap, &IJob::LoadCausticSpectralPhotonmap},
		{"shadow", &IJob::SaveShadowPhotonmap, &IJob::LoadShadowPhotonmap}
	};
	for( const auto& map : maps ) {
		if( tokens[0] != map.name ) continue;
		if( tokens[1] == "save" ) return (job.*map.save)( tokens[2].c_str() );
		if( tokens[1] == "load" ) return (job.*map.load)( tokens[2].c_str() );
		GlobalLog()->PrintEx( eLog_Error, "Unknown photon map operation: %s", tokens[1].c_str() );
		return false;
	}
	GlobalLog()->PrintEx( eLog_Error, "Unknown photon map type: %s", tokens[0].c_str() );
	return false;
}

static std::string JoinTokens( const std::string* tokens, unsigned int num_tokens )
{
	std::string ret;
	for( unsigned int i=0; i<num_tokens; i++ ) {
		if( i ) ret += ' ';
		ret += tokens[i];
	}

	return ret;
}

bool AsciiCommandParser::ParseMediaPath( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise )
{
	if( num_tokens < 1 ) {
		GlobalLog()->PrintEasyError( "AsciiCommandParser::ParseMediaPath: You need to do something with the media path (add|remove|clearall)" );
		return false;
	}

	if( tokens[0] == "add" )
	{
		if( num_tokens < 2 ) {
			GlobalLog()->PrintEasyError( "AsciiCommandParser::ParseMediaPath: You need to specify a path to add" );
			return false;
		}

		return GlobalMediaPathLocator().AddPath( JoinTokens(&tokens[1],num_tokens-1).c_str() );
	}
	else if( tokens[0] == "remove" )
	{
		if( num_tokens < 2 ) {
			GlobalLog()->PrintEasyError( "AsciiCommandParser::ParseMediaPath: You need to specify a path to remove" );
			return false;
		}

		return GlobalMediaPathLocator().RemovePath( JoinTokens(&tokens[1],num_tokens-1).c_str() );
	}
	else if( tokens[0] == "clearall" )
	{
		GlobalMediaPathLocator().ClearAllPaths();
		return true;
	}
	else
	{
		GlobalLog()->PrintEasyError( "AsciiCommandParser::ParseMediaPath: expected add, remove, or clearall" );
	}

	return false;
}

bool AsciiCommandParser::ParseEcho( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise )
{
	if( num_tokens < 2 ) {
		GlobalLog()->PrintEasyError( "Usage: ParseEcho (info|warning|error) message" );
		return false;
	}

	const std::string message = JoinTokens( &tokens[1], num_tokens - 1 ) + ' ';
	if( tokens[0] == "info" ) {
		GlobalLog()->Print( eLog_Info, message.c_str() );
	} else if( tokens[0] == "warning" ) {
		GlobalLog()->Print( eLog_Warning, message.c_str() );
	} else if( tokens[0] == "event" ) {
		GlobalLog()->Print( eLog_Event, message.c_str() );
	} else if( tokens[0] == "error" ) {
		GlobalLog()->Print( eLog_Error, message.c_str() );
	} else {
		GlobalLog()->PrintEx( eLog_Error, "AsciiCommandParser::ParseEcho:: Unknown type '%s'", tokens[0].c_str() );
		return false;
	}

	return true;
}
