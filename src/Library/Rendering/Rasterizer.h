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
#include <mutex>
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
			class RetainedRasterizerOutputSnapshot
			{
			public:
				RetainedRasterizerOutputSnapshot(
					const RasterizerOutputListType& source,
					std::mutex& sourceMutex
					)
				{
					std::lock_guard<std::mutex> lock(sourceMutex);
					try {
						for( IRasterizerOutput* output : source ) {
							output->addref();
							try {
								mOutputs.push_back(output);
							}
							catch( ... ) {
								output->release();
								throw;
							}
						}
					}
					catch( ... ) {
						Release();
						throw;
					}
				}

				~RetainedRasterizerOutputSnapshot()
				{
					Release();
				}

				const RasterizerOutputListType& Outputs() const
				{
					return mOutputs;
				}

			private:
				void Release()
				{
					for( IRasterizerOutput* output : mOutputs ) output->release();
					mOutputs.clear();
				}

				RasterizerOutputListType mOutputs;
			};

			template< class Callback >
			void ForEachRasterizerOutput( Callback callback ) const
			{
				RetainedRasterizerOutputSnapshot snapshot(outs,outsMutex);
				for( IRasterizerOutput* output : snapshot.Outputs() ) callback(output);
			}

			RasterizerOutputListType				outs;

			//! Registers one output under outsMutex and reports whether this call
			//! inserted it (false means the dedup path).  AutoRasterizer uses the
			//! result to roll back only its own wrapper insertion if delegate
			//! registration throws.  A newly inserted output immediately receives
			//! the current FrameStore before this returns.
			bool RegisterRasterizerOutput( IRasterizerOutput* ro );
			bool UnregisterRasterizerOutput( IRasterizerOutput* ro );
			bool ReleaseRasterizerOutputs();

			//! L8 review round 5 — protects `outs` against concurrent
			//! mutation from non-render threads.
			//!
			//! Background: the public mutators (`AddRasterizerOutput`,
			//! `FreeRasterizerOutputs`) used to be unlocked.  In the
			//! macOS GUI, Swift's `attachViewportFrameStoreToOpaqueRasterizer`
			//! re-runs whenever the SwiftUI view body recomputes (per
			//! display refresh, ~60 Hz), each call ending in
			//! `Rasterizer::AddRasterizerOutput(vfs)` from the UI
			//! thread.  Meanwhile, render workers iterate `outs`
			//! unlocked (~20 sites across PT/BDPT/VCM/MLT subclasses)
			//! firing OutputImage / OutputIntermediateImage.
			//! Concurrent `push_back` during iteration is a `vector`
			//! data race that can produce iterator-invalidation hangs
			//! or crashes (the user-visible "hung after several
			//! load→render cycles" symptom).
			//!
			//! AddRasterizerOutput + FreeRasterizerOutputs +
			//! EnumerateRasterizerOutputs + ReannounceFrameStore take
			//! this lock.  Iteration sites in subclasses remain
			//! unlocked under the contract: callers must not invoke
			//! `AddRasterizerOutput` / `FreeRasterizerOutputs` while
			//! a render is in flight on the same rasterizer.  Bridge
			//! enforces this at `[self rasterize]` entry
			//! (FreeRasterizerOutputs + Attach happen before
			//! `_job->Rasterize()` returns to the caller); but the
			//! Swift-side display-refresh path violated it before
			//! L8 round 5.  Companion fixes: dedup AddRasterizerOutput
			//! (so the Swift path becomes a no-op when already
			//! attached) and gate the bridge's `attachViewport...`
			//! re-entries.
			mutable std::mutex						outsMutex;

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
			int									mForTestThreadCountOverride = 0;

			//! Auxiliary-surface selection is also consumed by agent
			//! perception AOVs, so it must survive in builds without OIDN.
			OidnPrefilter							mDenoisingPrefilter;

#ifdef RISE_ENABLE_OIDN
			bool									bDenoisingEnabled;
			OidnQuality								mDenoisingQuality;
			OidnDevice								mDenoisingDevice;

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
			//! Test seam for covering the single-thread dispatcher branch.
			//! Zero restores topology-derived production behavior.
			void ForTest_SetThreadCountOverride( const int count ) {
				mForTestThreadCountOverride = count;
			}
			virtual void AddRasterizerOutput( IRasterizerOutput* ro );
			//! Removes exactly one matching output, if present.  This is an
			//! implementation-level companion to the legacy all-or-nothing
			//! FreeRasterizerOutputs API, used by transactional callers that must
			//! roll back one attachment without disturbing outputs added later by
			//! another owner.
			virtual void RemoveRasterizerOutput( IRasterizerOutput* ro );
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

			// L6b — Late-binding FrameStore setter.  Used by `Job` to
			// push the canonical FrameStore into the rasterizer AFTER
			// scene load completes (most scene files declare the
			// rasterizer chunk BEFORE the camera chunk, so at
			// rasterizer-construction time the active camera dims
			// aren't yet known and the factory was passed nullptr).
			//
			// Releases any previous FrameStore and addrefs the new
			// one (matching the lifecycle Rasterizer::Rasterizer
			// established).  Passing nullptr clears the FrameStore
			// (rasterizer falls back to its internal IRasterImage
			// path until L6c).
			//
			// Threading: caller must establish the same "rasterizer
			// is parked, no render in flight" precondition the rest
			// of `Job`'s mutable-state mutations honor (see Job.h
			// CONCURRENCY CONTRACT).  L6c will introduce a
			// chain-mutex so reader threads (UI viewports, encoders)
			// can read FrameStore concurrently with this swap.
			virtual void SetFrameStore( FrameStore* frameStore );

			// L6e-3 — Re-fire `OnRasterizerFrameStoreChanged(mFrameStore)`
			// on every attached `IRasterizerOutput` WITHOUT swapping
			// `mFrameStore`.  Use case: callers that explicitly need to
			// rebroadcast the current binding without going through the
			// `SetFrameStore(nullptr) → SetFrameStore(fs)` toggle
			// (which would tear down + rebuild observer state on
			// already-bound consumers — see L6e-3 review P0).
			//
			// Idempotent: calling on a rasterizer with null mFrameStore
			// just dispatches `OnRasterizerFrameStoreChanged(nullptr)`,
			// which most outputs treat as a no-op.
			//
			// Threading: same as `SetFrameStore` — caller must run on
			// the same thread that drives the rasterizer (no
			// concurrent SetFrameStore in-flight).
			void ReannounceFrameStore();

			// L6e-1.1 — Capability hook: does this rasterizer accept
			// the canonical Job-allocated FrameStore push, or does it
			// run on its own internal RISERasterImage path?
			//
			// Default true: PT/BDPT/VCM/interactive rasterizers write
			// through the FrameStore beauty view, and MLT copies each
			// resolved round into the canonical store before flushing.
			// A future rasterizer that retains an internal-only image
			// path must override this to false until it provides the
			// same completed-frame synchronization.
			//
			// Pre-fix this was a string-match on registry name in
			// `Job::PushJobFrameStoreToRasterizers`; brittle to
			// rename + scattered the policy away from the rasterizer
			// that owns the constraint.  See L6e-1.1 review #2 P0.
			virtual bool AcceptsFrameStorePush() const { return true; }

#ifdef RISE_ENABLE_OIDN
			void SetDenoisingEnabled( bool enabled ) { bDenoisingEnabled = enabled; }
			bool GetDenoisingEnabled() const { return bDenoisingEnabled; }
			void SetDenoisingQuality( OidnQuality quality ) { mDenoisingQuality = quality; }
			void SetDenoisingDevice( OidnDevice device ) { mDenoisingDevice = device; }
#endif
			//! Retained without OIDN because agent albedo/normal capture uses
			//! the same fast-versus-accurate surface semantics.
			void SetDenoisingPrefilter( OidnPrefilter prefilter ) { mDenoisingPrefilter = prefilter; }
			OidnPrefilter GetDenoisingPrefilter() const { return mDenoisingPrefilter; }
		};
	}
}


#endif
