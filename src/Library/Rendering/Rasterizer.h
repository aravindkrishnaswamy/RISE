//////////////////////////////////////////////////////////////////////
//
//  Rasterizer.h - Implementation help for rasterizers
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 29, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RASTERIZER_
#define RASTERIZER_

#include "../Utilities/Reference.h"
#include "../Utilities/OidnConfig.h"
#include "../Interfaces/IRasterizer.h"
#include <chrono>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class OIDNDenoiser;	// forward decl — full type only needed in Rasterizer.cpp

		class Rasterizer : public virtual IRasterizer, public virtual Reference
		{
		protected:
			typedef std::vector<IRasterizerOutput*>	RasterizerOutputListType;
			RasterizerOutputListType				outs;
			IProgressCallback*						pProgressFunc;

#ifdef RISE_ENABLE_OIDN
			bool									bDenoisingEnabled;
			OidnQuality								mDenoisingQuality;

			//! Wall-clock timestamp captured at the start of RasterizeScene
			//! by derived rasterizers via BeginRenderTimer().  Read by the
			//! denoise call site immediately before oidn::Filter::execute()
			//! to drive the OidnQuality::Auto heuristic.  See docs/OIDN.md
			//! (OIDN-P0-1) for the heuristic itself.
			mutable std::chrono::steady_clock::time_point mRenderStartTime;

			//! Per-rasterizer OIDN denoise context.  Owns the cached
			//! oidn::DeviceRef + FilterRef + buffer handles so cross-
			//! render reuse on the same rasterizer (especially the
			//! interactive viewport) skips the device/filter commit
			//! cost on cache hits.  Allocated eagerly in the
			//! constructor, freed in the destructor.  See docs/OIDN.md
			//! (OIDN-P0-2) for the cache key and rebuild semantics.
			//! `mutable` because the denoise call site is reached from
			//! const methods (RasterizeScene is `const`).
			mutable OIDNDenoiser*					mDenoiser;
#endif

			Rasterizer();
			virtual ~Rasterizer();

			// Figures out the number of threads to spawn based on the number of
			// processors in the system and the option settings
			int HowManyThreadsToSpawn() const;

#ifdef RISE_ENABLE_OIDN
			//! Stamp the render-start wall clock.  Called from each
			//! rasterizer's RasterizeScene entry point.  Cheap (one
			//! steady_clock::now); no-op when OIDN is disabled at compile
			//! time.
			void BeginRenderTimer() const {
				mRenderStartTime = std::chrono::steady_clock::now();
			}

			//! Seconds elapsed since BeginRenderTimer().  Used by the
			//! denoise call site to feed the auto heuristic.
			double GetRenderElapsedSeconds() const {
				const auto now = std::chrono::steady_clock::now();
				const std::chrono::duration<double> elapsed = now - mRenderStartTime;
				return elapsed.count();
			}
#endif

		public:
			virtual void AddRasterizerOutput( IRasterizerOutput* ro );
			virtual void FreeRasterizerOutputs( );
			virtual void EnumerateRasterizerOutputs( IEnumCallback<IRasterizerOutput>& pFunc ) const;
			virtual void SetProgressCallback( IProgressCallback* pFunc );

#ifdef RISE_ENABLE_OIDN
			void SetDenoisingEnabled( bool enabled ) { bDenoisingEnabled = enabled; }
			void SetDenoisingQuality( OidnQuality quality ) { mDenoisingQuality = quality; }
#endif
		};
	}
}


#endif
