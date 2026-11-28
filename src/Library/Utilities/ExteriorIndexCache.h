//////////////////////////////////////////////////////////////////////
//
//  ExteriorIndexCache.h - Bounded, lock-free-read cache of tables
//    precomputed for a specific exterior index of refraction.
//
//  Used by SSS models whose boundary extrapolation A = (1+Fdr)/(1-Fdr)
//  depends on the surrounding medium IOR (air=1 is handled directly by
//  the caller's constructor table, bypassing this cache).
//
//  CONTRACT:
//    - Keyed by exact exterior IOR (`double` equality).
//    - Lock-free reads: entries are immutable once published via a
//      release store to `count` and live until cache destruction.
//    - Builds are serialized by `buildMutex` only while `count < Capacity`.
//    - Once full (e.g., dispersive enclosures under HWSS/MLT), misses use
//      a lock-free nearest-key scan with a one-shot warning to avoid
//      mutex contention (documented in docs/DEBT_LEDGER.md).
//
//  Author: RISE debt-cleanup, slice `debt-dl291`
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef EXTERIOR_INDEX_CACHE_H
#define EXTERIOR_INDEX_CACHE_H

#include <atomic>
#include <cmath>
#include <mutex>
#include "Math3D/Math3D.h"
#include "../Interfaces/ILog.h"

namespace RISE
{
	template< class T, unsigned int Capacity >
	class ExteriorIndexCache
	{
		struct Entry
		{
			Scalar	exteriorIOR;
			T*		payload;
		};

		mutable std::atomic<Entry*>			slots[Capacity];
		mutable std::atomic<unsigned int>	count;
		mutable std::mutex					buildMutex;
		mutable std::atomic<bool>			warnedFull;

		//! Scans the first `n` published entries for an exact key.
		const T* Find( const Scalar exteriorIOR, const unsigned int n ) const
		{
			for( unsigned int i = 0; i < n; ++i ) {
				const Entry* e = slots[i].load( std::memory_order_acquire );
				if( e && e->exteriorIOR == exteriorIOR ) {
					return e->payload;
				}
			}
			return 0;
		}

		//! Nearest cached key; only called once all Capacity slots are
		//! published (immutable from then on), so it needs no lock.
		const T& Nearest( const Scalar exteriorIOR ) const
		{
			if( !warnedFull.exchange( true, std::memory_order_relaxed ) ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"ExteriorIndexCache:: more than %u distinct exterior indices (e.g. a dispersive enclosure); "
					"further exteriors reuse the nearest cached table (DL-291)", Capacity );
			}
			const Entry* best = slots[0].load( std::memory_order_acquire );
			for( unsigned int i = 1; i < Capacity; ++i ) {
				const Entry* e = slots[i].load( std::memory_order_acquire );
				if( std::fabs( e->exteriorIOR - exteriorIOR ) < std::fabs( best->exteriorIOR - exteriorIOR ) ) {
					best = e;
				}
			}
			return *best->payload;
		}

	public:
		ExteriorIndexCache() : count( 0 ), warnedFull( false )
		{
			for( unsigned int i = 0; i < Capacity; ++i ) {
				slots[i].store( 0, std::memory_order_relaxed );
			}
		}

		~ExteriorIndexCache()
		{
			for( unsigned int i = 0; i < Capacity; ++i ) {
				Entry* e = slots[i].load( std::memory_order_relaxed );
				if( e ) {
					delete e->payload;
					delete e;
				}
			}
		}

		//! Returns the table for `exteriorIOR`, building and taking
		//! ownership of the heap-allocated T* from `build(exteriorIOR)` on first use.
		template< class Builder >
		const T& Get( const Scalar exteriorIOR, const Builder& build ) const
		{
			const unsigned int published = count.load( std::memory_order_acquire );
			if( const T* hit = Find( exteriorIOR, published ) ) {
				return *hit;
			}
			if( published == Capacity ) {
				return Nearest( exteriorIOR );		// full: no lock, ever again
			}

			std::lock_guard<std::mutex> guard( buildMutex );
			const unsigned int n = count.load( std::memory_order_acquire );
			if( const T* hit = Find( exteriorIOR, n ) ) {
				return *hit;			// another thread built it while we waited
			}

			if( n < Capacity ) {
				Entry* e = new Entry;
				e->exteriorIOR = exteriorIOR;
				e->payload = build( exteriorIOR );
				slots[n].store( e, std::memory_order_release );
				count.store( n + 1, std::memory_order_release );
				return *e->payload;
			}

			// Filled while we waited for the lock.
			return Nearest( exteriorIOR );
		}

		//! Number of distinct exteriors built so far (diagnostics / tests).
		unsigned int Size() const { return count.load( std::memory_order_acquire ); }
	};
}

#endif
