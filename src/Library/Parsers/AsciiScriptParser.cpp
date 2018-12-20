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
#include "AsciiScriptParser.h"
#include "AsciiCommandParser.h"
#include "../Utilities/MediaPathLocator.h"

using namespace RISE;
using namespace RISE::Implementation;

#define MAX_CHARS_PER_LINE		8192
#define CURRENT_SCRIPT_VERSION	3

AsciiScriptParser::AsciiScriptParser( const char * szFilename_ )
{
	memset( szFilename, 0, 1024 );
	if( szFilename_ ) {
		strcpy( szFilename, GlobalMediaPathLocator().Find(szFilename_).c_str() );
	}
}

AsciiScriptParser::~AsciiScriptParser( )
{
}

bool AsciiScriptParser::ParseScript( IJob& pJob )
{
	// Open up the file and start parsing!
	std::ifstream			in( szFilename );

	if( in.fail() ) {
		GlobalLog()->PrintEx( eLog_Error, "AsciiScriptParser: Failed to load scene file \'%s\'", szFilename );
		return false;
	}

	char				line[MAX_CHARS_PER_LINE] = {0};		// <sigh>....
	AsciiCommandParser* parser = new AsciiCommandParser();
	GlobalLog()->PrintNew( parser, __FILE__, __LINE__, "command parser" );

	{
		// Verify version number
		in.getline( line, MAX_CHARS_PER_LINE );

		// First check the first few characters to see if it contains our marker
		static const char* id = "RISE ASCII SCRIPT";
		if( strncmp( line, id, strlen(id) ) ) {			
			GlobalLog()->Print( eLog_Error, "AsciiScriptParser: Scene does not contain RISE ASCII SCRIPT marker" );
			return false;
		}

		// Next find the scene version number
		const char* num = &line[strlen(id)];

		int version = atoi( num );

		if( version != CURRENT_SCRIPT_VERSION ) {
			GlobalLog()->PrintEx( eLog_Error, "AsciiScriptParser: Scene version problem, scene is version \'%d\', we require \'%d\'", version, CURRENT_SCRIPT_VERSION );
			return false;
		}
	}

	for(;;) {
		in.getline( line, MAX_CHARS_PER_LINE );
		if( in.fail() ) {
			break;
		}

		if( !parser->ParseCommand( line, pJob ) ) {
			GlobalLog()->PrintEx( eLog_Error, "AsciiScriptParser: Failed to parse line \'%s\'", line );
			return false;
		}
	}

	safe_release( parser );
	GlobalLog()->PrintEx( eLog_Info, "AsciiScriptParser: Successfully loaded \'%s\'", szFilename );

	return true;
}
