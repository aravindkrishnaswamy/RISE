//////////////////////////////////////////////////////////////////////
//
//  SceneGrammar.cpp - Implementation.  Walks the parser registry on
//    first access, collects each parser's ChunkDescriptor, and
//    builds fast lookup tables.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-24
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SceneGrammar.h"

namespace RISE
{
	namespace SceneEditorSuggestions
	{
		SceneGrammar::SceneGrammar()
			: mParsers( CreateAllChunkParsers() )
		{
			mKeywords.reserve( mParsers.size() );
			mDescriptors.reserve( mParsers.size() );

			for( std::vector<ChunkParserEntry>::const_iterator it = mParsers.begin(); it != mParsers.end(); ++it ) {
				const ChunkDescriptor& desc = it->parser->Describe();
				// Always register the entry's keyword (how AsciiSceneParser
				// actually dispatches) so aliases like mis_pathtracing_shaderop
				// — which share a parser class with pathtracing_shaderop and
				// therefore expose the same Describe().keyword — still
				// resolve at suggestion time.
				mKeywords.push_back( it->keyword );
				mDescriptors.push_back( &desc );
				mByKeyword[it->keyword] = &desc;
			}
		}

		const SceneGrammar& SceneGrammar::Instance()
		{
			static const SceneGrammar s;
			return s;
		}

		const ChunkDescriptor* SceneGrammar::FindChunk( const std::string& keyword ) const
		{
			std::map<std::string, const ChunkDescriptor*>::const_iterator it = mByKeyword.find( keyword );
			if( it == mByKeyword.end() ) {
				return 0;
			}
			return it->second;
		}

		std::vector<const ChunkDescriptor*> SceneGrammar::ChunksInCategory( ChunkCategory c ) const
		{
			std::vector<const ChunkDescriptor*> out;
			for( std::vector<const ChunkDescriptor*>::const_iterator it = mDescriptors.begin(); it != mDescriptors.end(); ++it ) {
				if( (*it)->category == c ) {
					out.push_back( *it );
				}
			}
			return out;
		}

		const char* SceneGrammar::CategoryDisplayName( ChunkCategory c ) const
		{
			switch( c ) {
			case ChunkCategory::Painter:          return "Painters";
			case ChunkCategory::Function:         return "Functions";
			case ChunkCategory::Material:         return "Materials";
			case ChunkCategory::Camera:           return "Cameras";
			case ChunkCategory::Geometry:         return "Geometry";
			case ChunkCategory::Modifier:         return "Modifiers";
			case ChunkCategory::Medium:           return "Media";
			case ChunkCategory::Object:           return "Objects";
			case ChunkCategory::ShaderOp:         return "Shader Ops";
			case ChunkCategory::Shader:           return "Shaders";
			case ChunkCategory::Rasterizer:       return "Rasterizers";
			case ChunkCategory::RasterizerOutput: return "Rasterizer Output";
			case ChunkCategory::Light:            return "Lights";
			case ChunkCategory::PhotonMap:        return "Photon Maps";
			case ChunkCategory::PhotonGather:     return "Photon Gather";
			case ChunkCategory::IrradianceCache:  return "Irradiance Cache";
			case ChunkCategory::Animation:        return "Animation";
			}
			return "";
		}
	}
}
