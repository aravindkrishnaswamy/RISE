//////////////////////////////////////////////////////////////////////
//
//  MediaPathLocator.cpp - Implementation of the media path locator
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 22, 2005
//  Tabs: 4
//  Comments: Resolves relative media/asset paths against an ordered list
//    of search roots (each normalized with a trailing separator).
//    Not internally mutex-protected; configure paths before concurrent renders.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "MediaPathLocator.h"
#include "../Interfaces/ILog.h"
#include <algorithm>
#include <filesystem>
#include <string_view>

using namespace RISE;

MediaPathLocator::MediaPathLocator()
{
}

MediaPathLocator::~MediaPathLocator()
{
}

namespace
{
	// Normalizes a search directory to always end with a path separator
	// so prefix concatenation and duplicate detection are consistent.
	std::string SearchRoot( const char* path )
	{
		std::string root = path ? path : "";
		if( !root.empty() && root.back() != '/' && root.back() != '\\' ) root += '/';
		return root;
	}
}

bool MediaPathLocator::FileExists( const String& file )
{
	std::error_code error;
	return std::filesystem::exists( LongAwareFsPath( file.c_str() ), error );
}

bool MediaPathLocator::AddPath( const char* path )
{
	const std::string root = SearchRoot( path );
	if( root.empty() ) return false;
	const auto existing = std::find_if( paths.begin(), paths.end(),
		[&root]( const String& item ) { return std::string_view(item.c_str()) == root; } );
	if( existing != paths.end() ) {
		GlobalLog()->PrintEx( eLog_Warning, "MediaPathLocator::AddPath '%s' already exists, ignoring request to add", path );
		return false;
	}
	paths.emplace_back( root.c_str() );
	return true;
}

bool MediaPathLocator::RemovePath( const char* path )
{
	const std::string root = SearchRoot( path );
	if( root.empty() ) return false;
	const auto entry = std::find_if( paths.begin(), paths.end(),
		[&root]( const String& item ) { return std::string_view(item.c_str()) == root; } );
	if( entry == paths.end() ) return false;
	paths.erase( entry );
	return true;
}

void MediaPathLocator::ClearAllPaths( )
{
	paths.clear();
}

// Checks `file` as-is first (CWD/absolute), then prefixes registered roots
// in order; logs an error and returns `file` unchanged if no match exists.
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

		const std::string candidate = std::string(path.c_str()) + file.c_str();
		String s( candidate.c_str() );

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


