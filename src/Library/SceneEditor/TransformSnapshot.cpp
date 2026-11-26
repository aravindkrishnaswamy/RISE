//////////////////////////////////////////////////////////////////////
//
//  TransformSnapshot.cpp - implementation of the Phase 6.1 helper.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TransformSnapshot.h"
#include <algorithm>

namespace RISE
{
    void TransformSnapshot::Add(const std::string& name, const Matrix4& m)
    {
        mEntries[name] = m;
    }

    const Matrix4* TransformSnapshot::Find(const std::string& name) const
    {
        auto it = mEntries.find(name);
        if( it == mEntries.end() ) return nullptr;
        return &it->second;
    }

    bool TransformSnapshot::Contains(const std::string& name) const
    {
        return mEntries.find(name) != mEntries.end();
    }

    std::size_t TransformSnapshot::Count() const
    {
        return mEntries.size();
    }

    void TransformSnapshot::Clear()
    {
        mEntries.clear();
    }

    std::vector<std::string> TransformSnapshot::Names() const
    {
        std::vector<std::string> out;
        out.reserve( mEntries.size() );
        for( const auto& kv : mEntries ) {
            out.push_back( kv.first );
        }
        std::sort( out.begin(), out.end() );
        return out;
    }
}
