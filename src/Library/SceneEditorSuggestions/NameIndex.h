//////////////////////////////////////////////////////////////////////
//
//  NameIndex.h - Scans a scene buffer to collect every `name`
//    parameter inside every `{...}` block, paired with the
//    containing chunk's keyword and category.  Used for
//    Reference-kind value completion (e.g. `material <cursor>`
//    inside a standard_object block → names of defined materials).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-24
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SCENE_EDITOR_SUGGESTIONS_NAMEINDEX_
#define SCENE_EDITOR_SUGGESTIONS_NAMEINDEX_

#include <string>
#include <vector>

#include "../Parsers/ChunkDescriptor.h"

namespace RISE
{
	namespace SceneEditorSuggestions
	{
		struct DefinedName
		{
			std::string   name;
			std::string   chunkKeyword;
			ChunkCategory category = ChunkCategory::Painter;
		};

		class NameIndex
		{
		public:
			explicit NameIndex( const std::string& bufferText );

			const std::vector<DefinedName>& AllNames() const { return mNames; }

			// All names whose containing chunk has a category in the given set.
			std::vector<const DefinedName*> FilterByCategories( const std::vector<ChunkCategory>& cats ) const;

		private:
			std::vector<DefinedName> mNames;
		};
	}
}

#endif
