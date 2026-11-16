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
#include <filesystem>
#include <vector>

namespace RISE
{
	//! Returns a `std::filesystem::path` usable with wide-character Win32 I/O
	//! (`std::ifstream(path)`, `std::ofstream(path)`, `std::filesystem::exists`,
	//! `create_directories`, `remove_all`) even when the path exceeds Win32
	//! `MAX_PATH` (260) on a Windows host without `LongPathsEnabled` or a
	//! `longPathAware` application manifest.  Win32 `\\?\` extended-length
	//! paths require an absolute, lexically normalized backslash-separated
	//! path (`\\?\C:\...` or `\\?\UNC\server\share\...`).  Pass
	//! `forceExtendedPrefix = true` when handing a short directory root to a
	//! recursive Win32 walker (`std::filesystem::remove_all`) whose child
	//! entries exceed `MAX_PATH`.  No-op on non-Win32.
	inline std::filesystem::path LongAwareFsPath(
		const std::filesystem::path& raw,
		const bool forceExtendedPrefix = false
		)
	{
#ifdef _WIN32
		if( raw.empty() ) {
			return raw;
		}
		const std::wstring& w = raw.native();
		auto stripPrefixedTrailingSlash = []( std::wstring& s ) {
			const bool isUnc = ( s.rfind( L"\\\\?\\UNC\\", 0 ) == 0 || s.rfind( L"\\\\.\\UNC\\", 0 ) == 0 );
			if( isUnc ) {
				// Preserve `\\?\UNC\server\share` (5 backslashes total); strip deeper trailing `\`.
				while( s.size() > 8 && s.back() == L'\\' ) {
					std::size_t slashes = 0;
					for( std::size_t i = 0; i + 1 < s.size(); ++i ) {
						if( s[i] == L'\\' ) ++slashes;
					}
					if( slashes < 5 ) break;
					s.pop_back();
				}
			} else {
				while( s.size() > 7 && s.back() == L'\\' && s[s.size() - 2] != L':' ) {
					s.pop_back();
				}
			}
		};
		if( w.rfind( L"\\\\?\\", 0 ) == 0 || w.rfind( L"\\\\.\\", 0 ) == 0 ||
			w.rfind( L"//?/", 0 ) == 0 || w.rfind( L"//./", 0 ) == 0 ) {
			std::wstring norm = w;
			for( wchar_t& ch : norm ) {
				if( ch == L'/' ) ch = L'\\';
			}
			if( norm.size() >= 8 &&
				( norm.rfind( L"\\\\?\\UNC\\", 0 ) == 0 || norm.rfind( L"\\\\.\\UNC\\", 0 ) == 0 ) ) {
				const std::wstring pfx = norm.substr( 0, 8 );
				std::wstring body = std::filesystem::path( L"\\\\" + norm.substr( 8 ) ).lexically_normal().make_preferred().native();
				if( body.rfind( L"\\\\", 0 ) == 0 ) {
					body.erase( 0, 2 );
				}
				norm = pfx + body;
			} else if( norm.size() >= 4 ) {
				const std::wstring pfx = norm.substr( 0, 4 );
				norm = pfx + std::filesystem::path( norm.substr( 4 ) ).lexically_normal().make_preferred().native();
			}
			stripPrefixedTrailingSlash( norm );
			return std::filesystem::path( norm );
		}
		// Only fast-return short paths BEFORE `absolute()` when `raw` is already
		// fully absolute (`is_absolute()` requires both root-name and root-dir
		// on Windows) and the caller did not request a recursive-walk prefix: a
		// relative path < 240 chars can exceed MAX_PATH once resolved against CWD.
		if( !forceExtendedPrefix && raw.is_absolute() && w.size() < 240 ) {
			return raw;
		}
		std::error_code ec;
		std::filesystem::path absPath = raw.is_absolute() ? raw : std::filesystem::absolute( raw, ec );
		if( ec ) {
			return raw;
		}
		std::wstring norm = absPath.lexically_normal().make_preferred().native();
		if( norm.rfind( L"\\\\?\\", 0 ) == 0 || norm.rfind( L"\\\\.\\", 0 ) == 0 ) {
			stripPrefixedTrailingSlash( norm );
			return std::filesystem::path( norm );
		}
		if( !forceExtendedPrefix && norm.size() < 240 ) {
			return raw;
		}
		// Strip any non-root trailing backslash before prepending `\\?\` so
		// literal Win32 directory APIs (`RemoveDirectoryW`, `GetFileAttributesExW`)
		// accept directory paths passed with a trailing separator (preserving `C:\`).
		while( norm.size() > 3 && norm.back() == L'\\' && norm[norm.size() - 2] != L':' ) {
			norm.pop_back();
		}
		if( norm.rfind( L"\\\\", 0 ) == 0 ) {
			return std::filesystem::path( L"\\\\?\\UNC\\" + norm.substr( 2 ) );
		}
		return std::filesystem::path( L"\\\\?\\" + norm );
#else
		(void)forceExtendedPrefix;
		return raw;
#endif
	}

	inline std::filesystem::path LongAwareFsPath( const char* raw, const bool forceExtendedPrefix = false )
	{
		return LongAwareFsPath( std::filesystem::path( raw ? raw : "" ), forceExtendedPrefix );
	}

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


