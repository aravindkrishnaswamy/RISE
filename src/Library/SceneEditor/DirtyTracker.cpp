//////////////////////////////////////////////////////////////////////
//
//  DirtyTracker.cpp - implementation of the Phase 6.3 dirty-set.
//
//////////////////////////////////////////////////////////////////////

#include "DirtyTracker.h"
#include <algorithm>

namespace RISE
{
    void DirtyTracker::MarkDirty( const std::string& objectName )
    {
        if( objectName.empty() ) return;
        mNames.insert( objectName );
    }

    void DirtyTracker::Clear()
    {
        mNames.clear();
    }

    bool DirtyTracker::Contains( const std::string& objectName ) const
    {
        return mNames.find( objectName ) != mNames.end();
    }

    std::vector<std::string> DirtyTracker::Snapshot() const
    {
        std::vector<std::string> out;
        out.reserve( mNames.size() );
        for( const std::string& n : mNames ) {
            out.push_back( n );
        }
        std::sort( out.begin(), out.end() );
        return out;
    }
}
