//////////////////////////////////////////////////////////////////////
//
//  ChunkDescriptorRegistry.cpp - Implementation.  Lazily builds a
//    keyword → ChunkDescriptor* map by walking the parser entries
//    returned by `CreateAllChunkParsers()`.  The IAsciiChunkParser
//    instances themselves stay alive in a function-static vector so
//    the const-references we cache remain valid for the lifetime of
//    the program.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ChunkDescriptorRegistry.h"
#include "../Parsers/ChunkParserRegistry.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace RISE
{
	namespace
	{
		//! The one lazily-built, cached registry both `DescriptorForKeyword`
		//! and `AllKeywordsForCategory` read -- factored out so S21's
		//! category-filtered enumeration does not stand up a second
		//! `CreateAllChunkParsers()` pass (and a second copy of the
		//! keep-the-parsers-alive vector) beside the original.
		const std::map<std::string, const ChunkDescriptor*>& Registry()
		{
			static std::once_flag once;
			static std::vector<ChunkParserEntry> entries;
			static std::map<std::string, const ChunkDescriptor*> map;

			std::call_once( once, []{
				entries = CreateAllChunkParsers();
				for( const ChunkParserEntry& e : entries ) {
					if( e.parser ) {
						map[ e.keyword ] = &( e.parser->Describe() );
					}
				}
			});

			return map;
		}
	}

	const ChunkDescriptor* DescriptorForKeyword( const String& keyword )
	{
		const std::map<std::string, const ChunkDescriptor*>& map = Registry();
		auto it = map.find( keyword.c_str() );
		return it == map.end() ? 0 : it->second;
	}

	std::vector<String> AllKeywordsForCategory( ChunkCategory category )
	{
		std::vector<String> out;
		for( const auto& kv : Registry() ) {
			if( kv.second && kv.second->category == category ) {
				out.push_back( String( kv.first.c_str() ) );
			}
		}
		std::sort( out.begin(), out.end(), []( const String& a, const String& b ) {
			return std::strcmp( a.c_str(), b.c_str() ) < 0;
		});
		return out;
	}
}
