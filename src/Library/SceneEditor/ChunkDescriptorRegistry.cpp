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

#include <map>
#include <mutex>
#include <vector>

namespace RISE
{
	const ChunkDescriptor* DescriptorForKeyword( const String& keyword )
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

		auto it = map.find( keyword.c_str() );
		return it == map.end() ? 0 : it->second;
	}
}
