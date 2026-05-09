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
		class FrameStore;	// forward decl — held as a counted reference

		class Rasterizer : public virtual IRasterizer, public virtual Reference
		{
		protected:
			typedef std::vector<IRasterizerOutput*>	RasterizerOutputListType;
			RasterizerOutputListType				outs;
			IProgressCallback*						pProgressFunc;

			//! L6a — Canonical FrameStore the rasterizer writes into
			//! (Phase 2 design, see docs/FRAMESTORE_DESIGN.md §6).
			//! L6a (this commit): held but unused — the helper still
			//! routes pixel writes through `mPersistentImage`.  L6b
			//! flips `PixelBasedRasterizerHelper` to write through
			//! `mFrameStore->AsBeautyRasterImage()` and bracket per-
			//! block writes with `BeginTile`/`EndTile`.  Counted
			//! reference: addref'd in the Rasterizer constructor when
			//! non-null, released in the destructor.  May be null
			//! (allows a transitional period where Job hasn't yet been
			//! migrated to allocate one — see L6a's verification
			//! commit).
			FrameStore*								mFrameStore;

#ifdef RISE_ENABLE_OIDN
			bool									bDenoisingEnabled;
			OidnQuality								mDenoisingQuality;
			OidnDevice								mDenoisingDevice;
			OidnPrefilter							mDenoisingPrefilter;

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

			//! Constructor.  `frameStore` may be null while L6a is
			//! mid-migration; non-null is the L6b+ target state.
			//! When non-null, this constructor addrefs it; the
			//! destructor releases.
			explicit Rasterizer( FrameStore* frameStore = nullptr );
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

			// L6a — IRasterizer override.  Returns the FrameStore
			// passed at construction time (may be null until Job
			// migrates to allocate one).
			// `virtual` is explicitly written here to match the
			// style of every other IRasterizer override in this
			// section (AddRasterizerOutput, SetProgressCallback,
			// etc. all spell out `virtual`).  `override` is
			// intentionally OMITTED because the surrounding
			// overrides aren't marked `override`; adding it here
			// trips `-Winconsistent-missing-override` against the
			// pre-existing methods.  See user memory:
			// `feedback_override_keyword_in_job.md`.
			virtual FrameStore* GetFrameStore() const
				{ return mFrameStore; }

#ifdef RISE_ENABLE_OIDN
			void SetDenoisingEnabled( bool enabled ) { bDenoisingEnabled = enabled; }
			void SetDenoisingQuality( OidnQuality quality ) { mDenoisingQuality = quality; }
			void SetDenoisingDevice( OidnDevice device ) { mDenoisingDevice = device; }
			void SetDenoisingPrefilter( OidnPrefilter prefilter ) { mDenoisingPrefilter = prefilter; }
#endif
		};
	}
}


#endif
