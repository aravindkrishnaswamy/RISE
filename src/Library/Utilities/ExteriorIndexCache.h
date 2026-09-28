//////////////////////////////////////////////////////////////////////
//
//  ExteriorIndexCache.h - A small, lock-free-read cache of tables that
//    were precomputed for one EXTERIOR index of refraction.
//
//  WHY (DL-291).  Some SSS models bake a boundary condition into a
//  table at construction: the Donner-Jensen multipole's extrapolation
//  term A = (1+Fdr)/(1-Fdr) is a function of the layer index RELATIVE
//  to the medium surrounding the body, and the profile is a
//  Sum-of-Gaussians fit (or a tabulated inverse Hankel transform) of
//  the resulting reflectance.  The constructor builds the table for
//  air; a body seen through another medium needs the same table built
//  for that exterior.  Rebuilding per hit is prohibitive (milliseconds
//  per build), and the set of exteriors a scene presents is small, so
//  each distinct exterior is built once, on first use, and kept.
//
//  CONTRACT.
//    - The key is the exterior index EXACTLY (bitwise double equality):
//      two keys that differ in the last bit are two entries.  Callers
//      handle air (exterior == 1) themselves, from their constructor
//      table, so the in-air path never touches this cache.
//    - Readers never lock: an entry is fully built before its slot is
//      published (release store of the count), and entries are never
//      mutated or removed until the cache is destroyed.
//    - Misses are serialized by one mutex (the build runs under it) --
//      but only while the cache still has room.  Once full, a miss is
//      served by a LOCK-FREE nearest-key scan: under HWSS / MLT spectral
//      rendering a dispersive enclosure presents a new exterior on
//      almost every hit, and taking the mutex there serialized the
//      render (external review: skin in BK7, PT spectral hwss, 2.4x
//      wall clock, 17-19 s of system time).
//    - Capacity is bounded.  A scene that presents more than `Capacity`
//      distinct exteriors (e.g. a DISPERSIVE enclosure, whose index
//      changes with every hero wavelength) is served the cached entry
//      with the NEAREST key once the cache is full, with a one-shot
//      warning.  That is an approximation, bounded by the spacing of
//      the keys already cached, and it depends on which exteriors
//      arrived first; it is documented in docs/DL49_SSS_EXTERIOR_INDEX.md.
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

		//! Returns the table for `exteriorIOR`, building it with
		//! `build( exteriorIOR )` (which must return a heap-allocated T*,
		//! owned by the cache from then on) on the first request.
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
