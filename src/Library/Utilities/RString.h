//////////////////////////////////////////////////////////////////////
//
//  RString.h - My own simple string implementation based on 
//    std::vector
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 27, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_STRING_
#define RISE_STRING_

#include <vector>
#include <ctype.h>			// for toupper

namespace RISE
{
	class String : 
		public std::vector<char>
	{
	public:
		String()
		{
		}

		String( const String& copy ) : 
		std::vector<char>( copy )
		{
		}

		String( std::vector<char>::const_iterator start, std::vector<char>::const_iterator finish ) : 
		std::vector<char>( start, finish )
		{
			resize( size()+1, 0 );
		}

		String( const char * sz )
		{
			// From a standard C string
			if( sz ) {
				const size_t string_size = strlen( sz );
				resize( string_size+1, 0 );
				memcpy( (char*)(&(*(begin()))), sz, strlen( sz )+1 );
			}
		}

		virtual ~String()
		{
		}

		const char* c_str() const
		{
			return (const char*)(&(*(begin())));
		}

		String operator =( const char* sz )
		{
			// From a standard C string
			const size_t string_size = strlen( sz );
			resize( string_size+1, 0 );
			memcpy( (char*)(&(*(begin()))), sz, strlen( sz )+1 );
			return (*this);
		}

		bool operator==( const char* sz ) const
		{
			if( strlen( sz ) != strlen(c_str()) ) {
				return false;
			}
			return ( strcmp( (const char*)(&(*(begin()))), sz ) == 0 );
		}

		void concatenate( const char* s )
		{
			resize( size() + strlen(s) + 1, 0 );
			strcat( (char*)(&(*(begin()))), s );
		}

		void concatenate( const String& s )
		{
			resize( size() + s.size() + 1, 0 );
			strcat( (char*)(&(*(begin()))), s.c_str() );
		}

		int toInt( ) const
		{
			int ret;
			sscanf( c_str(), "%d", &ret );
			return ret;
		}

		unsigned int toUInt( ) const
		{
			unsigned int ret;
			sscanf( c_str(), "%u", &ret );
			return ret;
		}

		float toFloat() const
		{
			float ret;
			sscanf( c_str(), "%f", &ret );
			return ret;
		}

		double toDouble() const
		{
			double ret;
			sscanf( c_str(), "%lf", &ret );
			return ret;
		}

		bool toBoolean() const
		{
			// If the first character is a t, then its true, otherwise false
			return (toupper(*(begin())) == 'T');
		}

		char toChar() const
		{
			char ret;
			sscanf( c_str(), "%c", &ret );
			return ret;
		}

		unsigned char toUChar() const
		{
			unsigned char ret;
			sscanf( c_str(), "%c", &ret );
			return ret;
		}
	};
}

#endif

