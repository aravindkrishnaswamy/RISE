//////////////////////////////////////////////////////////////////////
//
//  DirtyTracker.cpp - implementation of the Phase 6.3 / Phase B
//    dirty-set.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
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
        // Transient channels only — `mSessionCreated` persists, a
        // byte-splice-era rule (the deleted save engine re-emitted
        // created-entity chunks into its managed block on every
        // subsequent same-session save); kept so the channel still
        // means "created at any point this session".
        mNames.clear();
        mEntityDirty.clear();
        mCreatedPending.clear();
        mCstHeadDirty = false;
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

    void DirtyTracker::MarkEntityDirty( EntityCategory category,
                                        const std::string& name )
    {
        if( name.empty() ) return;
        mEntityDirty.insert( std::make_pair( category, name ) );
    }

    std::vector<DirtyEntity> DirtyTracker::EntitySnapshot() const
    {
        // mEntityDirty is a std::set — already in (category,name)
        // sorted order; copy straight out.
        return std::vector<DirtyEntity>( mEntityDirty.begin(),
                                         mEntityDirty.end() );
    }

    void DirtyTracker::MarkEntityCreated( EntityCategory category,
                                          const std::string& name )
    {
        if( name.empty() ) return;
        const DirtyEntity e = std::make_pair( category, name );
        mCreatedPending.insert( e );
        mSessionCreated.insert( e );
    }

    std::vector<DirtyEntity> DirtyTracker::SessionCreatedSnapshot() const
    {
        return std::vector<DirtyEntity>( mSessionCreated.begin(),
                                         mSessionCreated.end() );
    }
}
