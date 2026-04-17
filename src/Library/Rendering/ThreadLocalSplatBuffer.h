//////////////////////////////////////////////////////////////////////
//
//  ThreadLocalSplatBuffer.h - Per-worker sparse splat accumulator.
//
//    In BDPT/VCM/MLT's t==1 light-to-camera splat path, the hot
//    loop calls SplatFilm::Splat() once per accepted connection.
//    That hit a per-row RMutex (pthread_mutex) in the hot path,
//    serializing threads whenever any two splat to the same row.
//    On fast-inner-loop scenes this mutex ping-pong drops parallel
//    efficiency from 7–8× down to 3–4× on 10 cores.
//
//    This buffer sits in `thread_local` storage owned by the worker
//    and collects (x, y, color) records with zero synchronization.
//    At end-of-tile the worker calls FlushInto(film), which sorts
//    the records by row and acquires each row's mutex exactly once
//    to batch-commit its splats — contention drops from O(splats)
//    to O(unique rows touched).
//
//    Memory cost is bounded: for a 32×32 tile with up to ~50 splats
//    per pixel (VCM worst case), worst-case buffer is ~50K records
//    × 40 bytes = ~2 MB per thread.  Typical use is far smaller
//    (BDPT: 2–5 splats/pixel).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 16, 2026
//  Tabs: 4
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_THREAD_LOCAL_SPLAT_BUFFER_
#define RISE_THREAD_LOCAL_SPLAT_BUFFER_

#include "SplatFilm.h"
#include <cstdint>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class ThreadLocalSplatBuffer
		{
		public:
			// Reuse SplatFilm's record type so FlushInto can hand the
			// raw array straight to BatchCommit without casting.
			typedef SplatFilm::BatchRecord Record;

			ThreadLocalSplatBuffer() :
				pBoundFilm( 0 ), width( 0 ), height( 0 ) {}

			/// Bind to a SplatFilm.  If a previous binding exists,
			/// its pending records are flushed before rebinding.
			/// Called automatically by Splat() when the worker's
			/// buffer sees a new film pointer.
			void Bind( SplatFilm* film, unsigned int w, unsigned int h )
			{
				if( pBoundFilm && pBoundFilm != film ) {
					FlushInto( *pBoundFilm );
				}
				pBoundFilm = film;
				width      = w;
				height     = h;
				if( records.capacity() < 4096 ) {
					records.reserve( 4096 );
				}
			}

			/// Append a splat record.  No synchronization — the buffer
			/// is owned by a single worker thread.  If the record
			/// count exceeds `flushThreshold`, auto-flushes into the
			/// bound film so buffer memory stays bounded.
			void Splat( unsigned int x, unsigned int y, const RISEPel& c )
			{
				if( x >= width || y >= height || !pBoundFilm ) {
					return;
				}
				Record r;
				r.pixelIndex = y * width + x;
				r.color      = c;
				records.push_back( r );
				if( records.size() >= 65536 ) {
					FlushInto( *pBoundFilm );
				}
			}

			/// Batch-commit all buffered records into the shared
			/// SplatFilm.  Sorts by pixelIndex so rows group together,
			/// then acquires each unique row-mutex exactly once.
			/// After this call the buffer is empty.
			void FlushInto( SplatFilm& film );

			/// Flush into the currently-bound film (if any).  Called
			/// at end-of-tile to ensure no splats are orphaned when
			/// the render completes.
			void FlushBound()
			{
				if( pBoundFilm ) {
					FlushInto( *pBoundFilm );
				}
			}

			/// Flush AND drop the binding.  Use when the worker is
			/// done with its current render so the next render's
			/// (possibly different) SplatFilm does not dereference
			/// a stale pointer on first Splat().  Required at the
			/// end of every worker task (see RasterizeDispatchers.h
			/// and the custom MLT task bodies).
			void FlushAndUnbind()
			{
				FlushBound();
				pBoundFilm = 0;
				width      = 0;
				height     = 0;
			}

			SplatFilm* GetBoundFilm() const { return pBoundFilm; }

			/// Drop all pending records without committing.  Used at
			/// end-of-pass if the worker is torn down mid-flight.
			void Clear()
			{
				records.clear();
			}

			std::size_t Size() const { return records.size(); }

		private:
			SplatFilm*			pBoundFilm;
			unsigned int			width;
			unsigned int			height;
			std::vector<Record>	records;
		};

		//! Return the current thread's buffer.  Lazy-initialises to
		//! the given dimensions on first call per thread.  Call
		//! Init() explicitly on subsequent renders if the image size
		//! may differ.
		ThreadLocalSplatBuffer& GetThreadLocalSplatBuffer();

		//! Flush the calling thread's splat buffer into whatever
		//! film it's currently bound to, if any.  Free function so
		//! worker threads can flush without knowing the film they're
		//! attached to.  Called at end-of-tile and end-of-worker
		//! to keep splats from being orphaned.
		void FlushCallingThreadSplatBuffer();
	}
}

#endif
