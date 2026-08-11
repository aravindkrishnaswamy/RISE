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
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace RISE
{
	class Job;
	namespace Implementation
	{
		class OIDNDenoiser;	// forward decl — full type only needed in Rasterizer.cpp
		class FrameStore;	// forward decl — held as a counted reference

		class Rasterizer : public virtual IRasterizer,
		                   public virtual IFireRasterizerState,
		                   public virtual Reference
		{
		protected:
			typedef std::vector<IRasterizerOutput*>	RasterizerOutputListType;
			class FireOutputTopologyLease
			{
			public:
				FireOutputTopologyLease(
					const Rasterizer& owner,
					const IScene& scene,
					FireRenderPreflightAuthorization authorization );
				~FireOutputTopologyLease();
				FireOutputTopologyLease( const FireOutputTopologyLease& ) = delete;
				FireOutputTopologyLease& operator=(
					const FireOutputTopologyLease& ) = delete;
			private:
				const Rasterizer* owner_;
			};
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
			void WithRetainedRasterizerOutputs( Callback callback ) const
			{
				RetainedRasterizerOutputSnapshot snapshot(outs,outsMutex);
				callback(snapshot.Outputs());
			}

			template< class Callback >
			void ForEachRasterizerOutput( Callback callback ) const
			{
				WithRetainedRasterizerOutputs(
					[&]( const RasterizerOutputListType& outputs ) {
						for( IRasterizerOutput* output : outputs ) {
							callback(output);
							ValidateFireOutputLeaseState();
						}
					});
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
			bool RestoreOutputFrameStoreBindings( FrameStore* frameStore );
			void RestoreFrameStoreAfterFailedTransaction( FrameStore* frameStore );

			//! Protects the live output list, FrameStore binding, and fire
			//! topology lease.  Every callback traversal retains a snapshot
			//! under this mutex and invokes arbitrary output code only after
			//! unlocking, so callback-side Add/Remove/Free is lifetime-safe.
			//! A fire render leases topology for its full entry; mutations
			//! during that interval fail closed.
			mutable std::mutex						outsMutex;
			std::atomic<uint64_t> mFireOutputTopologyGeneration { 0u };
			std::atomic<unsigned int> mFireOutputBindingInProgress { 0u };
			mutable unsigned int mFireOutputTopologyLeaseCount = 0u;
			mutable const IScene* mFireOutputLeasedScene = nullptr;
			mutable std::string mFireOutputLeasedSceneMediaBinding;
			mutable std::function<bool()> mFireOutputLeasedStateValidator;

			IProgressCallback*						pProgressFunc;

			//! Canonical pixel/output store.  Pixel rasterizers write the
			//! Beauty view directly and outputs observe the same counted
			//! binding.  It may be null only before Job installs a film-sized
			//! store or for a legacy implementation that declines the push.
			FrameStore*								mFrameStore;
			int									mForTestThreadCountOverride = 0;
			mutable std::mutex mFireRenderPreflightMutex;
			mutable FireRenderPreflightAuthorization mFireRenderPreflightAuthorization;
			mutable const IScene* mFireRenderPreflightScene;
			mutable const FrameStore* mFireRenderPreflightStore;
			mutable uint64_t mFireRenderPreflightGeneration;
			mutable uint64_t mFireRenderPreflightOutputTopologyGeneration;
			mutable std::string mFireRenderPreflightMetadataBinding;
			mutable std::string mFireRenderPreflightSceneMediaBinding;
			mutable std::function<bool()> mFireRenderStateValidator;

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

			//! When non-null, the constructor retains `frameStore`; the
			//! destructor releases it.  Job may install the canonical store
			//! later when film dimensions were unavailable at construction.
			explicit Rasterizer( FrameStore* frameStore = nullptr );
			virtual ~Rasterizer();

			bool RequireFireRenderPreflight(
				const IScene& scene,
				FireRenderPreflightAuthorization authorization ) const;
			void ValidateFireOutputLeaseState() const;
			void AuthorizeInternalFireReentry(
				const IScene& scene,
				FireRenderPreflightAuthorization authorization ) const;
			void AuthorizeInternalFireDelegate(
				IRasterizer& delegate,
				const IScene& scene,
				FireRenderPreflightAuthorization authorization ) const;
			void ClearInternalFireDelegateAuthorization(
				IRasterizer& delegate ) const;
			virtual bool AuthorizeFireDelegatePreflight(
				const IScene&,
				FireRenderPreflightAuthorization ) const { return true; }
			virtual void ClearFireDelegatePreflight() const {}
			virtual bool SupportsFireMediaTransport() const { return true; }

		private:
			friend class ::RISE::Job;
			void NotifyFrameStoreChanged( FrameStore* frameStore );
			void SetFireRenderStateValidator(
				const std::function<bool()>& validator ) const;
			void ReleaseFireOutputTopologyLease() const;
			void ClearFireRenderPreflightAuthorization() const;
			bool AuthorizeFireRenderPreflight(
				const IScene& scene,
				FireRenderPreflightAuthorization authorization ) const;

		public:
			bool LastRenderCompleted() const override { return true; }
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
			virtual void AddRasterizerOutput( IRasterizerOutput* ro ) override;
			//! Removes exactly one matching output, if present.  This is an
			//! implementation-level companion to the legacy all-or-nothing
			//! FreeRasterizerOutputs API, used by transactional callers that must
			//! roll back one attachment without disturbing outputs added later by
			//! another owner.
			virtual void RemoveRasterizerOutput( IRasterizerOutput* ro );
			virtual void FreeRasterizerOutputs( ) override;
			virtual void EnumerateRasterizerOutputs( IEnumCallback<IRasterizerOutput>& pFunc ) const override;
			std::vector<IRasterizerOutput*> RetainRasterizerOutputs() const;
			virtual void SetProgressCallback( IProgressCallback* pFunc ) override;
			// L6a — IRasterizer override.  Returns the FrameStore
			// passed at construction time (may be null until Job
			// migrates to allocate one).
			// `virtual` remains explicit to match the surrounding
			// IRasterizer methods; `override` pins the capability after
			// the fire-boundary review made this class multi-interface.
			virtual FrameStore* GetFrameStore() const override
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
			// The swap and all output notifications are transactional.
			// Reentrant or concurrent binding changes fail closed; fire
			// renders additionally hold a topology lease that rejects the
			// mutation before any binding is published.
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
			// Reentrant or concurrent binding announcements fail closed.
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
