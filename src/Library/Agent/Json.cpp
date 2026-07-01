//////////////////////////////////////////////////////////////////////
//
//  Json.cpp - the tiny hand-rolled JSON codec (see Json.h).
//
//  This is a message codec for the JSON-RPC set, not a general JSON
//  library.  The parser is a single-pass recursive-descent scanner over
//  a std::string; the serializer is a straight recursive walk.  Both are
//  total (never throw): the parser reports malformation via a bool +
//  message so the RPC layer can map it to a -32700 parse error.
//
//////////////////////////////////////////////////////////////////////

#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>

namespace RISE
{
	namespace Agent
	{
		namespace
		{
			//! A cap on nesting depth so a pathological line (e.g. thousands
			//! of `[`) fails as a parse error rather than overflowing the
			//! C++ stack.  The RPC message set nests only a handful deep.
			const int kMaxDepth = 256;

			const std::string kEmptyString;

			//! A single-pass recursive-descent JSON parser.  Holds the input
			//! + a cursor; every parse routine advances the cursor and
			//! returns false (setting mError) on malformation.
			class Parser
			{
			public:
				Parser( const std::string& s ) : mS( s ), mPos( 0 ), mDepth( 0 ) {}

				bool Parse( JsonValue& out )
				{
					SkipWs();
					if( !ParseValue( out ) ) return false;
					SkipWs();
					if( mPos != mS.size() ) {
						return Fail( "trailing characters after JSON value" );
					}
					return true;
				}

				const std::string& Error() const { return mError; }

			private:
				bool Fail( const char* msg )
				{
					if( mError.empty() ) mError = msg;
					return false;
				}

				void SkipWs()
				{
					while( mPos < mS.size() ) {
						const char c = mS[mPos];
						if( c == ' ' || c == '\t' || c == '\n' || c == '\r' ) ++mPos;
						else break;
					}
				}

				bool ParseValue( JsonValue& out )
				{
					if( mPos >= mS.size() ) return Fail( "unexpected end of input" );
					const char c = mS[mPos];
					switch( c ) {
						case '{': return ParseObject( out );
						case '[': return ParseArray( out );
						case '"': {
							std::string str;
							if( !ParseString( str ) ) return false;
							out = JsonValue::MakeString( str );
							return true;
						}
						case 't': case 'f': return ParseBool( out );
						case 'n': return ParseNull( out );
						default:
							if( c == '-' || ( c >= '0' && c <= '9' ) ) return ParseNumber( out );
							return Fail( "unexpected character in JSON value" );
					}
				}

				bool ParseObject( JsonValue& out )
				{
					if( ++mDepth > kMaxDepth ) return Fail( "JSON nesting too deep" );
					out = JsonValue::MakeObject();
					++mPos;   // consume '{'
					SkipWs();
					if( mPos < mS.size() && mS[mPos] == '}' ) { ++mPos; --mDepth; return true; }
					while( true ) {
						SkipWs();
						if( mPos >= mS.size() || mS[mPos] != '"' )
							return Fail( "expected string key in object" );
						std::string key;
						if( !ParseString( key ) ) return false;
						SkipWs();
						if( mPos >= mS.size() || mS[mPos] != ':' )
							return Fail( "expected ':' after object key" );
						++mPos;   // consume ':'
						SkipWs();
						JsonValue val;
						if( !ParseValue( val ) ) return false;
						out.set( key, val );
						SkipWs();
						if( mPos >= mS.size() )
							return Fail( "unterminated object" );
						if( mS[mPos] == ',' ) { ++mPos; continue; }
						if( mS[mPos] == '}' ) { ++mPos; --mDepth; return true; }
						return Fail( "expected ',' or '}' in object" );
					}
				}

				bool ParseArray( JsonValue& out )
				{
					if( ++mDepth > kMaxDepth ) return Fail( "JSON nesting too deep" );
					out = JsonValue::MakeArray();
					++mPos;   // consume '['
					SkipWs();
					if( mPos < mS.size() && mS[mPos] == ']' ) { ++mPos; --mDepth; return true; }
					while( true ) {
						SkipWs();
						JsonValue val;
						if( !ParseValue( val ) ) return false;
						out.push_back( val );
						SkipWs();
						if( mPos >= mS.size() )
							return Fail( "unterminated array" );
						if( mS[mPos] == ',' ) { ++mPos; continue; }
						if( mS[mPos] == ']' ) { ++mPos; --mDepth; return true; }
						return Fail( "expected ',' or ']' in array" );
					}
				}

				bool ParseBool( JsonValue& out )
				{
					if( mS.compare( mPos, 4, "true" ) == 0 ) { mPos += 4; out = JsonValue::MakeBool( true );  return true; }
					if( mS.compare( mPos, 5, "false" ) == 0 ) { mPos += 5; out = JsonValue::MakeBool( false ); return true; }
					return Fail( "invalid literal (expected true/false)" );
				}

				bool ParseNull( JsonValue& out )
				{
					if( mS.compare( mPos, 4, "null" ) == 0 ) { mPos += 4; out = JsonValue::MakeNull(); return true; }
					return Fail( "invalid literal (expected null)" );
				}

				bool ParseNumber( JsonValue& out )
				{
					// Scan the JSON number grammar (int frac? exp?) then hand
					// the whole span to strtod for a lossless parse.
					const std::size_t start = mPos;
					if( mPos < mS.size() && mS[mPos] == '-' ) ++mPos;
					while( mPos < mS.size() && mS[mPos] >= '0' && mS[mPos] <= '9' ) ++mPos;
					if( mPos < mS.size() && mS[mPos] == '.' ) {
						++mPos;
						while( mPos < mS.size() && mS[mPos] >= '0' && mS[mPos] <= '9' ) ++mPos;
					}
					if( mPos < mS.size() && ( mS[mPos] == 'e' || mS[mPos] == 'E' ) ) {
						++mPos;
						if( mPos < mS.size() && ( mS[mPos] == '+' || mS[mPos] == '-' ) ) ++mPos;
						while( mPos < mS.size() && mS[mPos] >= '0' && mS[mPos] <= '9' ) ++mPos;
					}
					if( mPos == start ) return Fail( "invalid number" );
					const std::string tok = mS.substr( start, mPos - start );
					char* end = nullptr;
					const double d = std::strtod( tok.c_str(), &end );
					if( end == tok.c_str() || *end != '\0' ) return Fail( "invalid number" );
					out = JsonValue::MakeNumber( d );
					return true;
				}

				//! Append the UTF-8 encoding of a Unicode code point to `out`.
				static void AppendUtf8( std::string& out, unsigned int cp )
				{
					if( cp <= 0x7F ) {
						out += static_cast<char>( cp );
					} else if( cp <= 0x7FF ) {
						out += static_cast<char>( 0xC0 | ( cp >> 6 ) );
						out += static_cast<char>( 0x80 | ( cp & 0x3F ) );
					} else if( cp <= 0xFFFF ) {
						out += static_cast<char>( 0xE0 | ( cp >> 12 ) );
						out += static_cast<char>( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
						out += static_cast<char>( 0x80 | ( cp & 0x3F ) );
					} else {
						out += static_cast<char>( 0xF0 | ( cp >> 18 ) );
						out += static_cast<char>( 0x80 | ( ( cp >> 12 ) & 0x3F ) );
						out += static_cast<char>( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
						out += static_cast<char>( 0x80 | ( cp & 0x3F ) );
					}
				}

				//! Read four hex digits at the cursor into `out16`.  Advances
				//! the cursor past them on success.
				bool ParseHex4( unsigned int& out16 )
				{
					if( mPos + 4 > mS.size() ) return Fail( "truncated \\u escape" );
					unsigned int v = 0;
					for( int i = 0; i < 4; ++i ) {
						const char c = mS[mPos + i];
						v <<= 4;
						if( c >= '0' && c <= '9' )      v |= static_cast<unsigned int>( c - '0' );
						else if( c >= 'a' && c <= 'f' ) v |= static_cast<unsigned int>( c - 'a' + 10 );
						else if( c >= 'A' && c <= 'F' ) v |= static_cast<unsigned int>( c - 'A' + 10 );
						else return Fail( "invalid hex digit in \\u escape" );
					}
					mPos += 4;
					out16 = v;
					return true;
				}

				bool ParseString( std::string& out )
				{
					out.clear();
					++mPos;   // consume opening '"'
					while( mPos < mS.size() ) {
						const unsigned char c = static_cast<unsigned char>( mS[mPos] );
						if( c == '"' ) { ++mPos; return true; }
						if( c == '\\' ) {
							++mPos;
							if( mPos >= mS.size() ) return Fail( "unterminated string escape" );
							const char e = mS[mPos];
							switch( e ) {
								case '"':  out += '"';  ++mPos; break;
								case '\\': out += '\\'; ++mPos; break;
								case '/':  out += '/';  ++mPos; break;
								case 'b':  out += '\b'; ++mPos; break;
								case 'f':  out += '\f'; ++mPos; break;
								case 'n':  out += '\n'; ++mPos; break;
								case 'r':  out += '\r'; ++mPos; break;
								case 't':  out += '\t'; ++mPos; break;
								case 'u': {
									++mPos;   // consume 'u'
									unsigned int cp = 0;
									if( !ParseHex4( cp ) ) return false;
									// Handle a UTF-16 surrogate pair.
									if( cp >= 0xD800 && cp <= 0xDBFF ) {
										if( mPos + 2 <= mS.size() && mS[mPos] == '\\' && mS[mPos+1] == 'u' ) {
											mPos += 2;
											unsigned int lo = 0;
											if( !ParseHex4( lo ) ) return false;
											if( lo >= 0xDC00 && lo <= 0xDFFF ) {
												cp = 0x10000 + ( ( cp - 0xD800 ) << 10 ) + ( lo - 0xDC00 );
											} else {
												// Unpaired -> emit both as-is (lenient).
												AppendUtf8( out, cp );
												cp = lo;
											}
										}
										// else: unpaired high surrogate, emit as-is.
									}
									AppendUtf8( out, cp );
									break;
								}
								default:
									return Fail( "invalid escape character" );
							}
						} else if( c < 0x20 ) {
							return Fail( "unescaped control character in string" );
						} else {
							out += static_cast<char>( c );
							++mPos;
						}
					}
					return Fail( "unterminated string" );
				}

				const std::string& mS;
				std::size_t         mPos;
				int                 mDepth;
				std::string         mError;
			};

			//! Serialize a number: %.17g, but collapse an exact integer to its
			//! integer form (no trailing ".0") so ids and dims read cleanly.
			void SerializeNumber( std::string& out, double d )
			{
				if( std::isnan( d ) || std::isinf( d ) ) {
					// JSON has no NaN/Inf; emit 0 (the RPC set never carries
					// these -- a defensive, standards-legal fallback).
					out += '0';
					return;
				}
				// Integral and representable as a 64-bit int -> emit as int.
				if( d == std::floor( d ) && std::fabs( d ) < 9.0e15 ) {
					char buf[32];
					std::snprintf( buf, sizeof( buf ), "%lld", static_cast<long long>( d ) );
					out += buf;
					return;
				}
				char buf[64];
				std::snprintf( buf, sizeof( buf ), "%.17g", d );
				out += buf;
			}

			void SerializeValue( std::string& out, const JsonValue& v )
			{
				switch( v.type() ) {
					case JsonValue::Type::Null:   out += "null"; break;
					case JsonValue::Type::Bool:   out += ( v.asBool() ? "true" : "false" ); break;
					case JsonValue::Type::Number: SerializeNumber( out, v.asNumber() ); break;
					case JsonValue::Type::String: JsonAppendEscapedString( out, v.asString() ); break;
					case JsonValue::Type::Array: {
						out += '[';
						for( std::size_t i = 0; i < v.size(); ++i ) {
							if( i ) out += ',';
							SerializeValue( out, v.at( i ) );
						}
						out += ']';
						break;
					}
					case JsonValue::Type::Object: {
						out += '{';
						const auto& mem = v.members();
						for( std::size_t i = 0; i < mem.size(); ++i ) {
							if( i ) out += ',';
							JsonAppendEscapedString( out, mem[i].first );
							out += ':';
							SerializeValue( out, mem[i].second );
						}
						out += '}';
						break;
					}
				}
			}
		}

		const std::string& JsonValue::asString() const
		{
			return mType == Type::String ? mString : kEmptyString;
		}

		const JsonValue& JsonValue::at( std::size_t i ) const
		{
			static const JsonValue kNull;
			if( i >= mArray.size() ) return kNull;
			return mArray[i];
		}

		const JsonValue* JsonValue::find( const std::string& key ) const
		{
			// Last-set wins: scan from the end.
			for( std::size_t i = mObject.size(); i > 0; --i ) {
				if( mObject[i-1].first == key ) return &mObject[i-1].second;
			}
			return nullptr;
		}

		const JsonValue& JsonValue::get( const std::string& key ) const
		{
			static const JsonValue kNull;
			const JsonValue* p = find( key );
			return p ? *p : kNull;
		}

		void JsonAppendEscapedString( std::string& out, const std::string& s )
		{
			out += '"';
			for( char c : s ) {
				const unsigned char uc = static_cast<unsigned char>( c );
				switch( c ) {
					case '"':  out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\b': out += "\\b";  break;
					case '\f': out += "\\f";  break;
					case '\n': out += "\\n";  break;
					case '\r': out += "\\r";  break;
					case '\t': out += "\\t";  break;
					default:
						if( uc < 0x20 ) {
							static const char* const kHex = "0123456789abcdef";
							out += "\\u00";
							out += kHex[ ( uc >> 4 ) & 0xF ];
							out += kHex[ uc & 0xF ];
						} else {
							out += c;
						}
						break;
				}
			}
			out += '"';
		}

		bool JsonParse( const std::string& text, JsonValue& out, std::string& outError )
		{
			out = JsonValue::MakeNull();
			outError.clear();
			Parser p( text );
			if( !p.Parse( out ) ) {
				outError = p.Error().empty() ? std::string( "malformed JSON" ) : p.Error();
				out = JsonValue::MakeNull();
				return false;
			}
			return true;
		}

		std::string JsonSerialize( const JsonValue& v )
		{
			std::string out;
			SerializeValue( out, v );
			return out;
		}
	}
}
