//////////////////////////////////////////////////////////////////////
//
//  MediaPathLocator.cpp - Implementation of the media path locator
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 22, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "MediaPathLocator.h"
#include "../Interfaces/ILog.h"
#include <sys/types.h>
#include <sys/stat.h>

using namespace RISE;

MediaPathLocator::MediaPathLocator()
{
}

MediaPathLocator::~MediaPathLocator()
{
}

bool MediaPathLocator::FileExists( const String& file )
{
	struct stat file_stats = {0};
	return !stat( file.c_str(), &file_stats );
}

bool MediaPathLocator::AddPath( const char* path )
{
	if( path ) {
		String p(path);

		PathListType::iterator it;
		for( it=paths.begin(); it!=paths.end(); it++ ) {
			const String& po = *it;

			if( p == po ) {
				GlobalLog()->PrintEx( eLog_Warning, "MediaPathLocator::AddPath '%s' already exists, ignoring request to add", path );
				return false;
			}
		}

		if( !(path[strlen(path)-1] == '/' || path[strlen(path)-1] == '\\') ) {
			p.concatenate( "/" );
		}
		paths.push_back( p );

		return true;
	}

	return false;
}

bool MediaPathLocator::RemovePath( const char* path )
{
	String po( path );

	PathListType::iterator it;
	for( it=paths.begin(); it!=paths.end(); it++ ) {
		const String& p = *it;

		if( po == p ) {
			paths.erase( it );
			return true;
		}
	}

	return false;
}

void MediaPathLocator::ClearAllPaths( )
{
	paths.clear();
}

String MediaPathLocator::Find( const String& file )
{
	if( FileExists( file ) ) {
		return file;
	}

	// Otherwise, we need to slap the file name in front of each of the paths in our list
	// and check if the file exists there
	PathListType::const_iterator it;
	for( it=paths.begin(); it!=paths.end(); it++ ) {
		const String& path = *it;

		String s( path );
		s.concatenate( file );

		if( FileExists( s ) ) {
			GlobalLog()->PrintEx( eLog_Info, "MediaPathLocator:: Remapped '%s' to '%s'", file.c_str(), s.c_str() );
			return s;
		}
	}

	GlobalLog()->PrintEx( eLog_Error, "MediaPathLocator:: Cannot find file '%s', giving up, errors may ensue!", file.c_str() );

	return file;
}

String MediaPathLocator::Find( const char* file )
{
	return Find( String( file ) );
}


namespace RISE
{
	MediaPathLocator& GlobalMediaPathLocator()
	{
		static MediaPathLocator global;
		return global;
	}
}


