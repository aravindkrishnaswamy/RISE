//////////////////////////////////////////////////////////////////////
//
//  AsciiScriptParser.cpp - Implementation of the really simple
//  AsciiScriptParser class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 22, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <memory>
#include <cstdlib>
#include <vector>
#include "AsciiScriptParser.h"
#include "AsciiCommandParser.h"
#include "../Utilities/MediaPathLocator.h"

using namespace RISE;
using namespace RISE::Implementation;

static constexpr int kCurrentScriptVersion = 3;

AsciiScriptParser::AsciiScriptParser( const char * szFilename_ )
{
	if( szFilename_ ) {
		filename = GlobalMediaPathLocator().Find( szFilename_ ).c_str();
	}
}

AsciiScriptParser::~AsciiScriptParser( )
{
}

bool AsciiScriptParser::ParseScript( IJob& pJob )
{
	// Open up the file and start parsing!
	std::ifstream in( filename );

	if( in.fail() ) {
		GlobalLog()->PrintEx( eLog_Error, "AsciiScriptParser: Failed to load script file '%s'", filename.c_str() );
		return false;
	}

	static thread_local std::vector<std::filesystem::path> activeScripts;
	std::error_code pathError;
	auto path = std::filesystem::canonical( filename, pathError );
	if( pathError ) path = filename;
	if( activeScripts.size() >= 64 ||
		std::find( activeScripts.begin(), activeScripts.end(), path ) != activeScripts.end() ) {
		GlobalLog()->PrintEx( eLog_Error, "AsciiScriptParser: Recursive or deeply nested script '%s'", filename.c_str() );
		return false;
	}
	activeScripts.push_back( std::move(path) );
	struct ActiveScript {
		std::vector<std::filesystem::path>& scripts;
		~ActiveScript() { scripts.pop_back(); }
	} active{ activeScripts };

	std::string line;

	{
		// Verify version number
		// First check the first few characters to see if it contains our marker
		static const char* id = "RISE ASCII SCRIPT";
		if( !std::getline( in, line ) || line.find( '\0' ) != std::string::npos ||
			line.compare( 0, std::strlen(id), id ) != 0 ) {
			GlobalLog()->Print( eLog_Error, "AsciiScriptParser: Script does not contain RISE ASCII SCRIPT marker" );
			return false;
		}

		// Next find the scene version number
		// Preserve the legacy signed decimal prefix, including header annotations.
		if( std::strtol( line.c_str() + std::strlen(id), nullptr, 10 ) != kCurrentScriptVersion ) {
			GlobalLog()->PrintEx( eLog_Error, "AsciiScriptParser: Expected script version %d", kCurrentScriptVersion );
			return false;
		}
	}

	const auto release = []( AsciiCommandParser* parser ) { parser->release(); };
	std::unique_ptr<AsciiCommandParser, decltype(release)> parser( new AsciiCommandParser(), release );
	GlobalLog()->PrintNew( parser.get(), __FILE__, __LINE__, "command parser" );
	while( std::getline( in, line ) ) {
		if( line.find( '\0' ) != std::string::npos || !parser->ParseCommand( line.c_str(), pJob ) ) {
			GlobalLog()->PrintEx( eLog_Error, "AsciiScriptParser: Failed to parse line \'%s\'", line.c_str() );
			return false;
		}
	}

	if( in.bad() ) {
		return false;
	}
	GlobalLog()->PrintEx( eLog_Info, "AsciiScriptParser: Successfully loaded '%s'", filename.c_str() );

	return true;
}
