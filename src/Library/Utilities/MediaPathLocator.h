//////////////////////////////////////////////////////////////////////
//
//  MediaPathLocator.h - Defines a class which given a path/filename
//    is able to then provide a fully qualified path and filename
//    where that file can be found, given a set of paths to look in
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 22, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MEDIA_PATH_LOCATOR_H
#define MEDIA_PATH_LOCATOR_H

#include "RString.h"
#include <vector>

namespace RISE
{
	class MediaPathLocator
	{
	protected:
		typedef std::vector<String> PathListType;
		PathListType paths;

		bool FileExists( const String& file );

	public:
		MediaPathLocator();
		virtual ~MediaPathLocator();

		/// Adds a new path to the path list
		bool AddPath( const char* path );

		/// Removes a path from the path list
		bool RemovePath( const char* path );

		/// Clears the entire path list
		void ClearAllPaths( );

		/// Finds the given path/filename and returns a string that contains
		/// the fully qualified and verified file
		/// \return If the path/filename cannot be found and verified, returns
		/// the original path/filename
		String Find( const char* file );
		String Find( const String& file );
	};

	MediaPathLocator& GlobalMediaPathLocator();
}

#endif


