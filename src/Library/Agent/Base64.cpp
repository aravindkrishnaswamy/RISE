//////////////////////////////////////////////////////////////////////
//
//  Base64.cpp - the tiny RFC-4648 base64 codec (see Base64.h).
//
//////////////////////////////////////////////////////////////////////

#include "Base64.h"

namespace RISE
{
	namespace Agent
	{
		namespace
		{
			const char* const kAlphabet =
				"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

			//! Reverse-lookup for a base64 character; -1 for a non-alphabet
			//! byte (the caller separately allows padding / whitespace).
			int DecodeChar( unsigned char c )
			{
				if( c >= 'A' && c <= 'Z' ) return c - 'A';
				if( c >= 'a' && c <= 'z' ) return c - 'a' + 26;
				if( c >= '0' && c <= '9' ) return c - '0' + 52;
				if( c == '+' ) return 62;
				if( c == '/' ) return 63;
				return -1;
			}
		}

		std::string Base64Encode( const std::vector<unsigned char>& bytes )
		{
			std::string out;
			const std::size_t n = bytes.size();
			out.reserve( ( ( n + 2 ) / 3 ) * 4 );

			std::size_t i = 0;
			while( i + 3 <= n ) {
				const unsigned int triple =
					( static_cast<unsigned int>( bytes[i]   ) << 16 ) |
					( static_cast<unsigned int>( bytes[i+1] ) <<  8 ) |
					  static_cast<unsigned int>( bytes[i+2] );
				out += kAlphabet[ ( triple >> 18 ) & 0x3F ];
				out += kAlphabet[ ( triple >> 12 ) & 0x3F ];
				out += kAlphabet[ ( triple >>  6 ) & 0x3F ];
				out += kAlphabet[   triple         & 0x3F ];
				i += 3;
			}

			const std::size_t rem = n - i;
			if( rem == 1 ) {
				const unsigned int triple = static_cast<unsigned int>( bytes[i] ) << 16;
				out += kAlphabet[ ( triple >> 18 ) & 0x3F ];
				out += kAlphabet[ ( triple >> 12 ) & 0x3F ];
				out += '=';
				out += '=';
			} else if( rem == 2 ) {
				const unsigned int triple =
					( static_cast<unsigned int>( bytes[i]   ) << 16 ) |
					( static_cast<unsigned int>( bytes[i+1] ) <<  8 );
				out += kAlphabet[ ( triple >> 18 ) & 0x3F ];
				out += kAlphabet[ ( triple >> 12 ) & 0x3F ];
				out += kAlphabet[ ( triple >>  6 ) & 0x3F ];
				out += '=';
			}
			return out;
		}

		bool Base64Decode( const std::string& text, std::vector<unsigned char>& out )
		{
			out.clear();

			int  accumBits = 0;    // number of valid 6-bit groups buffered
			unsigned int accum = 0;
			bool sawPad = false;

			for( char ch : text ) {
				const unsigned char c = static_cast<unsigned char>( ch );
				if( c == ' ' || c == '\t' || c == '\n' || c == '\r' ) continue;
				if( c == '=' ) { sawPad = true; continue; }
				if( sawPad ) {
					// A non-pad, non-whitespace char after padding is invalid.
					out.clear();
					return false;
				}
				const int v = DecodeChar( c );
				if( v < 0 ) { out.clear(); return false; }
				accum = ( accum << 6 ) | static_cast<unsigned int>( v );
				accumBits += 6;
				if( accumBits >= 8 ) {
					accumBits -= 8;
					out.push_back( static_cast<unsigned char>( ( accum >> accumBits ) & 0xFF ) );
				}
			}
			return true;
		}
	}
}
