//////////////////////////////////////////////////////////////////////
//
//  Options.cpp - Implements the Options class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 12, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Options.h"
#include "Parsers/AsciiCommandParser.h"
#include <fstream>

using namespace RISE;
using namespace RISE::Implementation;

Options::Options( 
	const char* filename 
	)
{
	if( !filename ) {
		return;
	}
	// Read the entire file line by line, and store
	// all the tokens
	std::ifstream			in( filename );

	if( in.fail() ) {
		GlobalLog()->PrintEx( eLog_Error, "Options::Options: Failed to load options file \'%s\'", filename );
		return;
	}

	char				line[2048] = {0};

	for(;;) {
		in.getline( line, 2048 );

		if( in.fail() ) {
			break;
		}

		// Tokenize the string to get rid of comments etc
		String			tokens[1024];
		unsigned int numTokens = Implementation::AsciiCommandParser::TokenizeString( line, tokens, 1024 );

		if( numTokens >= 2 ) {

			// Check the first character of the token to make sure its not a comment
			if( (tokens[0])[0] != '#' ) {

				// There actually are tokens!
				// Store them
				
				// First make sure the option isn't already in our map
				if( options.find( tokens[0] ) != options.end() ) {
					GlobalLog()->PrintEx( eLog_Warning, "Options::Options: This option exists more than once! \'%s\'", tokens[0].c_str() );
					continue;
				}

				// If tokens[1] beings with a " then we are dealing with a string
				// and we should concatenate all the tokens into a string
				if( tokens[1] == "str" ) {
					RISE::String val;
					for( unsigned int i=2; i<numTokens; i++ ) {
						val.concatenate( " " );
						val.concatenate( tokens[i] );
					}
					options[tokens[0]] = val;
				} else {
					options[tokens[0]] = tokens[1];
				}
			}
		}
	}
}

Options::~Options()
{
}

int Options::ReadInt( const char* name, int default_value )
{
	std::map<String,String>::iterator it = options.find( String(name) );

	if( it != options.end() ) {
		return atoi(it->second.c_str());
	}

	return default_value;
}

double Options::ReadDouble( const char* name, double default_value )
{
	std::map<String,String>::iterator it = options.find( String(name) );

	if( it != options.end() ) {
		return atof(it->second.c_str());
	}

	return default_value;
}

bool Options::ReadBool( const char* name, bool default_value )
{
	std::map<String,String>::iterator it = options.find( String(name) );

	if( it != options.end() ) {
		return (it->second=="TRUE" || it->second=="true");
	}

	return default_value;
}

String Options::ReadString( const char* name, String default_value )
{
	std::map<String,String>::iterator it = options.find( String(name) );

	if( it != options.end() ) {
		return it->second;
	}

	return default_value;
}

#include "Utilities/MediaPathLocator.h"
#include <sys/types.h>
#include <sys/stat.h>

namespace RISE
{
	IOptions& GlobalOptions()
	{
		static Options* pGlobal;
		static const char* szDefaultOptionsFile = "global.options";

		if( !pGlobal ) {
			// Initialize it
			
			struct stat file_stats = {0};

			// First try and get the global option file name from the environment
			const char* szoptionsfile = getenv( "RISE_OPTIONS_FILE" );

			if( !szoptionsfile ) {
				GlobalLog()->PrintEasyInfo( "Global Options file not specified with the 'RISE_OPTIONS_FILE' environment variable, looking for global.options file" );
			} else {
				// Check to see if it exists
				if( stat( szoptionsfile, &file_stats ) == -1 ) {
					// Doesn't exist, now lets try the options file 
					GlobalLog()->PrintEx( eLog_Warning, "The options file '%s' specified in RISE_OPTIONS_FILE doesn't exist, looking for global.options file", szoptionsfile );
				} else {
					pGlobal = new Options( szoptionsfile );
				}
			}

			if( !pGlobal ) {
				// Try loading through the media path locator
				RISE::String optionsfile = GlobalMediaPathLocator().Find(szDefaultOptionsFile);
				if( stat( optionsfile.c_str(), &file_stats ) == -1 ) {
					// Still doesn't exist, give up!
					pGlobal = new Options( 0 );
				} else {
					pGlobal = new Options( optionsfile.c_str() );
				}
			}
		}
		
		return *pGlobal;
	}
}

