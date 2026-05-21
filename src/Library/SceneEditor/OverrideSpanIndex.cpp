//////////////////////////////////////////////////////////////////////
//
//  OverrideSpanIndex.cpp - implementation of the Phase 6.2 catalog.
//
//////////////////////////////////////////////////////////////////////

#include "OverrideSpanIndex.h"
#include <algorithm>

namespace RISE
{
    OverrideSpanIndex::OverrideSpanIndex()
    {
    }

    OverrideSpanIndex::~OverrideSpanIndex()
    {
    }

    void OverrideSpanIndex::Add( OverrideRecord&& rec )
    {
        const std::string name = rec.targetName;
        const std::size_t idx = mEntries.size();
        mEntries.push_back( std::move(rec) );
        mByName.insert( std::make_pair(name, idx) );
    }

    const OverrideRecord* OverrideSpanIndex::FindManaged( const std::string& name ) const
    {
        const OverrideRecord* last = nullptr;
        auto range = mByName.equal_range( name );
        for( auto it = range.first; it != range.second; ++it ) {
            const OverrideRecord& r = mEntries[it->second];
            if( r.managed ) {
                // Return the last managed record in scene-file order.
                // mByName entries are inserted in scene-file order (Add
                // pushes to mEntries and inserts into mByName atomically),
                // but the multimap doesn't guarantee insertion-order
                // iteration.  Track the highest-index match by walking
                // and remembering the last we hit.
                if( !last || it->second > static_cast<std::size_t>(last - &mEntries[0]) ) {
                    last = &r;
                }
            }
        }
        return last;
    }

    bool OverrideSpanIndex::HasUnmanagedFor( const std::string& name ) const
    {
        auto range = mByName.equal_range( name );
        for( auto it = range.first; it != range.second; ++it ) {
            if( !mEntries[it->second].managed ) {
                return true;
            }
        }
        return false;
    }

    std::vector<const OverrideRecord*> OverrideSpanIndex::FindAll( const std::string& name ) const
    {
        std::vector<const OverrideRecord*> out;
        auto range = mByName.equal_range( name );
        for( auto it = range.first; it != range.second; ++it ) {
            out.push_back( &mEntries[it->second] );
        }
        // mByName multimap iteration order is implementation-defined;
        // sort by underlying index to recover scene-file order.
        std::sort( out.begin(), out.end(),
            []( const OverrideRecord* a, const OverrideRecord* b ) {
                return a < b;  // mEntries is contiguous → pointer-order == insertion-order
            } );
        return out;
    }

    std::size_t OverrideSpanIndex::Count() const
    {
        return mEntries.size();
    }

    void OverrideSpanIndex::Clear()
    {
        mEntries.clear();
        mByName.clear();
    }

    void OverrideSpanIndex::ApplyOffsetDeltas( const std::vector<OffsetDelta>& deltasIn )
    {
        if( deltasIn.empty() ) return;

        // Sort ascending by threshold so a single linear walk per
        // offset accumulates the right cumulative delta.  Mirrors
        // SourceSpanIndex::ApplyOffsetDeltas exactly so an offset
        // shared between the two indices ends up at the same
        // post-edit byte position.
        std::vector<OffsetDelta> deltas = deltasIn;
        std::sort( deltas.begin(), deltas.end(),
            []( const OffsetDelta& a, const OffsetDelta& b ) {
                return a.threshold < b.threshold;
            } );

        auto adjust = [&]( std::size_t offset ) -> std::size_t {
            long long cum = 0;
            for( const OffsetDelta& d : deltas ) {
                if( d.threshold <= offset ) {
                    cum += d.delta;
                } else {
                    break;  // sorted ascending
                }
            }
            const long long signedSum = static_cast<long long>( offset ) + cum;
            return signedSum < 0 ? 0 : static_cast<std::size_t>( signedSum );
        };

        for( OverrideRecord& r : mEntries ) {
            r.chunkBeginOffset = adjust( r.chunkBeginOffset );
            r.chunkEndOffset   = adjust( r.chunkEndOffset );
        }
    }

    void OverrideSpanIndex::RemoveAllManaged()
    {
        // Compact in place: copy unmanaged records to a fresh vector,
        // then rebuild mByName.  We can't just `erase_if` because the
        // multimap is index-based; the indices need to be renumbered
        // after the compaction.
        std::vector<OverrideRecord> kept;
        kept.reserve( mEntries.size() );
        for( OverrideRecord& r : mEntries ) {
            if( !r.managed ) {
                kept.push_back( std::move(r) );
            }
        }
        mEntries.swap( kept );

        mByName.clear();
        for( std::size_t i = 0; i < mEntries.size(); ++i ) {
            mByName.insert( std::make_pair(mEntries[i].targetName, i) );
        }
    }
}
