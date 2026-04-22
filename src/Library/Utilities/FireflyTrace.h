//////////////////////////////////////////////////////////////////////
//
//  FireflyTrace.h - Ephemeral per-pixel diagnostic logging.
//
//    Opt-in logging of a single pixel's paths for firefly diagnosis.
//    Activates only when env var RISE_FFTRACE_X and RISE_FFTRACE_Y
//    are set; writes to RISE_FFTRACE_FILE (or stderr if unset),
//    serialized by a single mutex so lines from many render threads
//    don't tear.
//
//    DO NOT leave trace calls in shipping code — they are hot-path
//    and meant only for bug investigation.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#ifndef FIREFLY_TRACE_H_
#define FIREFLY_TRACE_H_

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <mutex>

namespace RISE
{
	namespace FireflyTrace
	{
		struct State
		{
			int			targetX;
			int			targetY;
			FILE*		out;
			std::mutex	mu;
			bool		initialized;
			State() : targetX(-1), targetY(-1), out(0), initialized(false) {}
		};

		inline State& GetState()
		{
			static State s;
			return s;
		}

		inline void LazyInit()
		{
			State& s = GetState();
			if( s.initialized ) return;
			std::lock_guard<std::mutex> lk( s.mu );
			if( s.initialized ) return;
			const char* sx = std::getenv( "RISE_FFTRACE_X" );
			const char* sy = std::getenv( "RISE_FFTRACE_Y" );
			const char* sf = std::getenv( "RISE_FFTRACE_FILE" );
			if( sx && sy ) {
				s.targetX = std::atoi( sx );
				s.targetY = std::atoi( sy );
			}
			if( sf && *sf ) {
				s.out = std::fopen( sf, "w" );
			}
			if( !s.out ) {
				s.out = stderr;
			}
			s.initialized = true;
		}

		inline bool IsActive( int x, int y )
		{
			LazyInit();
			const State& s = GetState();
			return s.targetX == x && s.targetY == y;
		}

		// Thread-local flag the integrator sets true while walking the
		// traced pixel's path.  Nested utilities (ManifoldSolver,
		// LightSampler, BSDF, ...) can check this without needing
		// rast.x/y plumbed through their API.
		inline bool& PathActiveRef()
		{
			static thread_local bool active = false;
			return active;
		}

		inline bool IsPathActive()
		{
			return PathActiveRef();
		}

		struct PathScope
		{
			bool prev;
			PathScope( bool on ) : prev( PathActiveRef() ) { PathActiveRef() = on; }
			~PathScope() { PathActiveRef() = prev; }
		};

		inline void LogLine( const char* fmt, ... )
		{
			LazyInit();
			State& s = GetState();
			std::lock_guard<std::mutex> lk( s.mu );
			va_list ap;
			va_start( ap, fmt );
			std::vfprintf( s.out, fmt, ap );
			std::fputc( '\n', s.out );
			std::fflush( s.out );
			va_end( ap );
		}
	}
}

#define FF_TRACE_ACTIVE(x, y)    ::RISE::FireflyTrace::IsActive( (int)(x), (int)(y) )
#define FF_TRACE(...)            ::RISE::FireflyTrace::LogLine( __VA_ARGS__ )

#endif
