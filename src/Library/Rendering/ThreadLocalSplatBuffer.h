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
//    On normal task/pass completion, FlushInto(film) sorts
//    the records by row and acquires each row's mutex exactly once
//    to batch-commit its splats — contention drops from O(splats)
//    to O(unique rows touched).
//
//    Auto-flush begins at 65,536 buffered records. Vector capacity is
//    retained between tasks and is implementation-dependent; this is
//    a record-count threshold, not a fixed allocation byte budget.
//    Exceptional task exit discards pending records and the binding
//    without accessing a possibly destroyed film.
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
			/// count reaches 65,536, auto-flushes into the bound film.
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
			/// After a successful call the buffer is empty.
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
				try {
					FlushBound();
				} catch( ... ) {
					DiscardAndUnbind();
					throw;
				}
				DiscardAndUnbind();
			}

			/// Failure cleanup: discard pending records AND forget the film.
			/// Never flush, allocate, lock, or dereference the bound film.
			/// Retains vector capacity for reuse by this worker.
			void DiscardAndUnbind() noexcept
			{
				records.clear();
				pBoundFilm = nullptr;
				width = height = 0;
			}

			SplatFilm* GetBoundFilm() const { return pBoundFilm; }

			/// Drop only pending records, retaining the current binding.
			/// Failed task cleanup must use DiscardAndUnbind() instead.
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

		//! Return the current thread's buffer. Bind() supplies its current
		//! film and dimensions; storage capacity persists across tasks.
		ThreadLocalSplatBuffer& GetThreadLocalSplatBuffer();

		//! Task/pass lifetime guard. Normal paths keep their explicit flush;
		//! this guard discards and unbinds on every exit, including flush failure.
		//! Cost is one TLS lookup at construction and record cleanup at task end;
		//! Splat() and its batching threshold are unchanged.
		class ThreadLocalSplatBufferTaskScope
		{
		public:
			ThreadLocalSplatBufferTaskScope();
			~ThreadLocalSplatBufferTaskScope() noexcept;
			ThreadLocalSplatBufferTaskScope(const ThreadLocalSplatBufferTaskScope&) = delete;
			ThreadLocalSplatBufferTaskScope& operator=(const ThreadLocalSplatBufferTaskScope&) = delete;
		private:
			ThreadLocalSplatBuffer& buffer_;
		};

		//! Flush the calling thread's splat buffer into whatever
		//! film it's currently bound to, if any.  Free function so
		//! worker threads can flush without knowing the film they're
		//! attached to.  Called at end-of-tile and end-of-worker
		//! to keep splats from being orphaned.
		void FlushCallingThreadSplatBuffer();
	}
}

#endif
