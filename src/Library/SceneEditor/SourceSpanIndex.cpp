//////////////////////////////////////////////////////////////////////
//
//  SourceSpanIndex.cpp - implementation of Phase 6.1's per-entity
//    source-file metadata registry.
//
//////////////////////////////////////////////////////////////////////

#include "SourceSpanIndex.h"
#include <algorithm>

namespace RISE
{
    SourceSpanIndex::SourceSpanIndex()
    {
    }

    SourceSpanIndex::~SourceSpanIndex()
    {
    }

    void SourceSpanIndex::Add(const std::string& name, SourceSpan&& span)
    {
        mEntries[name] = std::move(span);
    }

    const SourceSpan* SourceSpanIndex::Find(const std::string& name) const
    {
        auto it = mEntries.find(name);
        if( it == mEntries.end() ) return nullptr;
        return &it->second;
    }

    SourceSpan* SourceSpanIndex::FindMutable(const std::string& name)
    {
        auto it = mEntries.find(name);
        if( it == mEntries.end() ) return nullptr;
        return &it->second;
    }

    std::size_t SourceSpanIndex::Count() const
    {
        return mEntries.size();
    }

    void SourceSpanIndex::RecordCreationLocation(const std::string& name,
                                                 std::string filePath,
                                                 std::size_t chunkEndOffset)
    {
        CreationLocation loc;
        loc.filePath = std::move(filePath);
        loc.chunkEndOffset = chunkEndOffset;
        mCreationLocation[name] = std::move(loc);
    }

    std::size_t SourceSpanIndex::GetCreationOffsetEnd(const std::string& name) const
    {
        auto it = mCreationLocation.find(name);
        if( it == mCreationLocation.end() ) return kNoCreationOffset;
        return it->second.chunkEndOffset;
    }

    const std::string& SourceSpanIndex::GetCreationFilePath(const std::string& name) const
    {
        auto it = mCreationLocation.find(name);
        if( it == mCreationLocation.end() ) return mEmptyPath;
        return it->second.filePath;
    }

    bool SourceSpanIndex::HasCreationLocation(const std::string& name) const
    {
        return mCreationLocation.find(name) != mCreationLocation.end();
    }

    void SourceSpanIndex::Clear()
    {
        mEntries.clear();
        mCreationLocation.clear();
        mFileIdentity = FileIdentity{};
    }

    void SourceSpanIndex::SetFileIdentity( FileIdentity id )
    {
        mFileIdentity = std::move( id );
    }

    void SourceSpanIndex::ApplyOffsetDeltas( const std::vector<OffsetDelta>& deltasIn )
    {
        if( deltasIn.empty() ) return;

        // Sort once, then walk each offset using a short linear pass
        // (deltas count == number of EditScript ops in one save —
        // typically O(dirty objects), small).
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
            // Guard against unsigned wrap on the rare case where a
            // bookkeeping bug produces a negative net offset.
            const long long signedSum = static_cast<long long>( offset ) + cum;
            return signedSum < 0 ? 0 : static_cast<std::size_t>( signedSum );
        };

        // Walk every SourceSpan + its per-parameter spans.
        for( auto& kv : mEntries ) {
            SourceSpan& s = kv.second;
            s.chunkBeginOffset       = adjust( s.chunkBeginOffset );
            s.chunkEndOffset         = adjust( s.chunkEndOffset );
            s.bodyOpenBraceOffset    = adjust( s.bodyOpenBraceOffset );
            s.bodyCloseBraceOffset   = adjust( s.bodyCloseBraceOffset );
            s.bodyCloseBraceLineBegin = adjust( s.bodyCloseBraceLineBegin );
            for( auto& pkv : s.parameterSpans ) {
                ParameterSpan& p = pkv.second;
                p.lineBeginOffset = adjust( p.lineBeginOffset );
                p.lineEndOffset   = adjust( p.lineEndOffset );
                p.valueBegin      = adjust( p.valueBegin );
                p.valueEnd        = adjust( p.valueEnd );
                p.commentBegin    = adjust( p.commentBegin );
            }
        }

        // Walk every CreationLocation chunkEndOffset (used by save
        // engine's placement loop for FOR 2..N entities).
        for( auto& kv : mCreationLocation ) {
            kv.second.chunkEndOffset = adjust( kv.second.chunkEndOffset );
        }
    }

    void SourceSpanIndex::RemapFilePath( const std::string& oldPath,
                                        const std::string& newPath )
    {
        if( oldPath.empty() || oldPath == newPath ) return;
        for( auto& kv : mEntries ) {
            if( kv.second.filePath == oldPath ) {
                kv.second.filePath = newPath;
            }
        }
        for( auto& kv : mCreationLocation ) {
            if( kv.second.filePath == oldPath ) {
                kv.second.filePath = newPath;
            }
        }
        // The FileIdentity's filePath is intentionally NOT touched
        // here — the save engine updates it explicitly to the
        // newly-written target file's identity (path + fresh stat).
    }
}
