//////////////////////////////////////////////////////////////////////
//
//  ViewportFrameStore.cpp - Implementation.
//
//  See header for the architecture.  This file implements the lazy
//  chain-allocation pattern (mirroring FileRasterizerOutput's
//  EnsureChain), the BridgeObserver helper that fans FrameStore
//  events out to the user-supplied callbacks, and the rasterizer
//  Attach/Detach methods.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ViewportFrameStore.h"
#include "FrameStore.h"
#include "FrameSink.h"
#include "FileEncoderObserver.h"
#include "../Interfaces/IRasterizer.h"
#include "../Interfaces/IRenderObserver.h"
#include "../Interfaces/IRasterImage.h"
#include "../Interfaces/IFrameEncoder.h"
#include "../Interfaces/ILog.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <mutex>
#include <optional>
#include <shared_mutex>

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	namespace Implementation
	{
		namespace
		{
			template <typename T>
			class RetainedReference
			{
			public:
				explicit RetainedReference( T* ptr ) : ptr_(ptr) {}
				~RetainedReference() { if( ptr_ ) ptr_->release(); }
				RetainedReference( const RetainedReference& ) = delete;
				RetainedReference& operator=( const RetainedReference& ) = delete;
				T* get() const { return ptr_; }
				T* operator->() const { return ptr_; }
				T& operator*() const { return *ptr_; }
				explicit operator bool() const { return ptr_ != nullptr; }

			private:
				T* ptr_;
			};

			bool EncoderAcceptsPath( const IFrameEncoder& encoder,
				const std::string& path )
			{
				const std::string::size_type separator = path.find_last_of("/\\");
				const std::string::size_type dot = path.find_last_of('.');
				if( dot == std::string::npos || dot+1u == path.size() ||
					(separator != std::string::npos && dot < separator) ) return false;
				std::string extension = path.substr(dot+1u);
				std::transform(extension.begin(),extension.end(),extension.begin(),
					[]( const unsigned char c ) {
						return static_cast<char>(std::tolower(c));
					});
				for( std::string candidate : encoder.Extensions() ) {
					std::transform(candidate.begin(),candidate.end(),candidate.begin(),
						[]( const unsigned char c ) {
							return static_cast<char>(std::tolower(c));
						});
					if( candidate == extension ) return true;
				}
				return false;
			}
		}

		// ─────────────────────────────────────────────────────────────
		// BridgeObserver — internal IRenderObserver registered on the
		// FrameStore.  Fans tile / frame / pre-denoise / denoise
		// callbacks out to the user-supplied std::function callbacks
		// stored on ViewportFrameStore.
		//
		// Lives in the .cpp because it's strictly an implementation
		// detail; nothing outside this TU references it.  Lifetime
		// bound to ViewportFrameStore via the same lazy-allocate /
		// teardown pattern as the FrameStore itself.
		// ─────────────────────────────────────────────────────────────
		class ViewportFrameStore::BridgeObserver : public IRenderObserver
		{
		public:
			explicit BridgeObserver( ViewportFrameStore& parent )
				: parent_( parent ), active_( false )
			{
			}
			void Activate() { active_.store(true,std::memory_order_release); }

			void OnTileComplete( const Rect& roi, uint64_t generation ) override
			{
				if( !active_.load(std::memory_order_acquire) ) return;
				if ( parent_.tileCb_ ) parent_.tileCb_( roi, generation );
			}

			void OnFrameComplete( unsigned int frame, uint64_t generation ) override
			{
				if( !active_.load(std::memory_order_acquire) ) return;
				if ( parent_.frameCb_ ) parent_.frameCb_( frame, generation );
			}

			void OnPreDenoiseComplete( unsigned int frame, uint64_t generation ) override
			{
				if( !active_.load(std::memory_order_acquire) ) return;
				if ( parent_.preDenoiseCb_ ) parent_.preDenoiseCb_( frame, generation );
			}

			void OnDenoiseComplete( unsigned int frame, uint64_t generation ) override
			{
				if( !active_.load(std::memory_order_acquire) ) return;
				if ( parent_.denoiseCb_ ) parent_.denoiseCb_( frame, generation );
			}

		private:
			ViewportFrameStore& parent_;
			std::atomic<bool> active_;
		};

		// ─────────────────────────────────────────────────────────────
		// Construction / destruction
		// ─────────────────────────────────────────────────────────────

		ViewportFrameStore::ViewportFrameStore()
			: cameraExposureEV_( Scalar( 0 ) )
		{
			dormant_.reserve(kMaxDormantChains);
		}

		ViewportFrameStore::~ViewportFrameStore()
		{
			// Same Output*-vs-dtor contract as FileRasterizerOutput:
			// the platform code is responsible for ensuring no
			// rasterizer thread is mid-OutputImage when this
			// destructs (typically by Detach()-ing first AND
			// joining any rasterizer threads).
			//
			// Route through BindFrameStore(nullptr) rather than holding
			// chainMutex_ around RemoveObserver.  The bind transaction
			// leaves the old chain published while it quiesces observers,
			// then swaps the chain and observer registrations at commit.
			// See BindFrameStore comment for the full rationale.
			//
			// Snapshot semantics for in-flight readers: same as
			// pre-fix.  Reader threads that already captured an
			// addref'd `framestore_` snapshot continue to hold it
			// alive past the commit; their work completes against the
			// captured pointer; they release.  Cleanup then drops the
			// VFS reference; the last retained reader
			// snapshot destroys the FrameStore when its work completes.
			BindFrameStore( nullptr );
		}

		// L8 review round 3 — the previous `TeardownChain()` method
		// was removed to eliminate a deadlock hazard.  It called
		// `RemoveObserver` while assuming the caller held
		// `chainMutex_` unique_lock; under that assumption, an
		// in-flight observer dispatch on a worker thread that
		// re-enters `chainMutex_` (via `RenderToBuffer`) would
		// deadlock against `RemoveObserver`'s wait protocol.
		//
		// Replacement: `BindFrameStore(nullptr)` prepares observer
		// removal without the chain lock, quiesces new callback claims,
		// then commits the observer mutation and pointer swap atomically
		// under deterministic observer-store locks plus chainMutex_.
		// The dtor uses it; `EnsureChain` still uses
		// `ParkActiveAsDormant_locked` for dim changes (which
		// doesn't call `RemoveObserver`).  No other in-tree caller
		// existed.

		// Caller MUST hold chainMutex_ as unique_lock when calling this.
		// Moves the current active triple onto the front of dormant_
		// (MRU position) and clears the active pointers.  If the
		// dormant cache is at capacity, returns the LRU entry via
		// `outEvicted` for the caller to tear down OUTSIDE the lock
		// (its `RemoveObserver` waits for in-flight observer dispatches
		// to drain — those dispatches re-enter `chainMutex_` via
		// `RenderToBuffer`, so calling RemoveObserver while holding
		// the lock deadlocks).  Caller is responsible for invoking
		// `TeardownDormant_unlocked` on the returned entry once the
		// lock is dropped.  See L8 review round 4.
		void ViewportFrameStore::ParkActiveAsDormant_locked( DormantChain& outEvicted )
		{
			outEvicted = DormantChain();
			if ( !framestore_ ) {
				return;  // nothing to park
			}

			// Evict LRU first if we'd otherwise exceed the cap.  The
			// cap is `kMaxDormantChains` for parked entries
			// (active is in addition to that).  Move-out the LRU into
			// `outEvicted`; teardown is the caller's job (post-fix).
			if ( dormant_.size() >= kMaxDormantChains ) {
				outEvicted = dormant_.back();
				dormant_.pop_back();
			}

			DormantChain entry;
			entry.fs   = framestore_;
			entry.sink = framesink_;
			entry.obs  = observer_;
			entry.w    = static_cast<unsigned int>( framestore_->Width() );
			entry.h    = static_cast<unsigned int>( framestore_->Height() );
			dormant_.insert( dormant_.begin(), entry );

			framestore_ = nullptr;
			framesink_  = nullptr;
			observer_   = nullptr;
		}

		// Tear down a dormant entry returned from
		// `ParkActiveAsDormant_locked` evictions.  Caller MUST NOT
		// hold `chainMutex_` — `RemoveObserver` waits for in-flight
		// observer dispatches whose callbacks re-enter chainMutex_
		// via `RenderToBuffer`.  See L8 review round 4 for the
		// dispatch-mutex inversion that motivated this split.
		// No-op on a default-constructed (empty) DormantChain.
		void ViewportFrameStore::TeardownDormant_unlocked( DormantChain& d )
		{
			if ( d.fs && d.obs ) {
				d.fs->RemoveObserver( d.obs );
			}
			delete d.obs;
			safe_release( d.sink );
			safe_release( d.fs );
			d = DormantChain();
		}

		// ─────────────────────────────────────────────────────────────
		// Callback setters
		// ─────────────────────────────────────────────────────────────

		void ViewportFrameStore::SetTileCompleteCallback( TileCompleteCallback cb )
		{
			tileCb_ = std::move( cb );
		}
		void ViewportFrameStore::SetFrameCompleteCallback( FrameCompleteCallback cb )
		{
			frameCb_ = std::move( cb );
		}
		void ViewportFrameStore::SetPreDenoiseCompleteCallback( FrameCompleteCallback cb )
		{
			preDenoiseCb_ = std::move( cb );
		}
		void ViewportFrameStore::SetDenoiseCompleteCallback( FrameCompleteCallback cb )
		{
			denoiseCb_ = std::move( cb );
		}

		// ─────────────────────────────────────────────────────────────
		// Rasterizer attachment
		// ─────────────────────────────────────────────────────────────

		void ViewportFrameStore::Attach( IRasterizer* rasterizer )
		{
			if ( !rasterizer ) return;
			// Rasterizer registration publishes the current FrameStore through
			// OnRasterizerFrameStoreChanged before it returns. Pulling a second raw
			// pointer here would race a concurrent SetFrameStore replacement.
			rasterizer->AddRasterizerOutput( this );
		}

		void ViewportFrameStore::Detach( IRasterizer* rasterizer )
		{
			if ( !rasterizer ) return;
			// IRasterizer doesn't have a per-output remove method
			// (FreeRasterizerOutputs is all-or-nothing).  For the
			// L4 use case (single GUI sink per rasterizer), the
			// platform code typically owns the rasterizer
			// lifetime: when the rasterizer is being torn down
			// (scene reload, rasterizer swap), the platform
			// either calls FreeRasterizerOutputs or lets the
			// rasterizer destructor release us.
			//
			// We provide Detach as a stub for symmetry with
			// Attach; if a future need arises for partial
			// detachment, IRasterizer can grow a
			// RemoveRasterizerOutput method.  Today this is a
			// no-op — the rasterizer's outputs list will release
			// us when it's freed or cleared.
			(void)rasterizer;
		}

		// ─────────────────────────────────────────────────────────────
		// L6e-2a — External FrameStore binding
		// ─────────────────────────────────────────────────────────────

		void ViewportFrameStore::BindFrameStore( FrameStore* external )
		{
			uint64_t bindRevision = 0u;
			{
				std::lock_guard<std::mutex> lock(bindRequestMutex_);
				if( bindDrainActive_ ) {
					throw std::runtime_error(
						"ViewportFrameStore bind transaction already active");
				}
				bindDrainActive_ = true;
				bindRevision = bindRevision_.fetch_add(
					1u,std::memory_order_acq_rel)+1u;
			}

			struct BindActivity
			{
				std::mutex& mutex;
				bool& draining;
				std::atomic<unsigned int>& active;
				BindActivity( std::mutex& requestMutex, bool& drainActive,
					std::atomic<unsigned int>& count )
					: mutex(requestMutex), draining(drainActive), active(count)
					{ active.fetch_add(1u,std::memory_order_acq_rel); }
				~BindActivity()
				{
					{
						std::lock_guard<std::mutex> lock(mutex);
						draining = false;
					}
					active.fetch_sub(1u,std::memory_order_acq_rel);
				}
			} bindActivity(bindRequestMutex_,bindDrainActive_,bindTransactionsInFlight_);
			if( bindPhaseOneTestHook_ ) bindPhaseOneTestHook_(bindRevision);
			ApplyBindFrameStore(external,bindRevision);
		}

		void ViewportFrameStore::ApplyBindFrameStore(
			FrameStore* external, const uint64_t requestRevision )
		{
			std::vector<DormantChain> oldDormant;
			oldDormant.reserve(kMaxDormantChains);
			FrameStore*     oldExternal = nullptr;
			FrameStore*     oldFs       = nullptr;
			FrameSink*      oldSink     = nullptr;
			BridgeObserver* oldObs      = nullptr;
			{
				std::unique_lock<std::shared_mutex> lock( chainMutex_ );
				if( bindRevision_.load(std::memory_order_acquire) != requestRevision ) {
					return;
				}

				// Idempotent — re-binding the same pointer is a no-op,
				// avoids tearing down + re-registering an observer that
				// would point at the same store.
				if ( external && external == externalFrameStore_ ) {
					return;
				}
				if( !external && !externalFrameStore_ && !framestore_ && !framesink_ &&
					!observer_ && dormant_.empty() ) {
					return;
				}
				// Snapshot every potentially-allocating old-chain value before a
				// candidate observer is created or any observer is quiesced.
				oldExternal = externalFrameStore_;
				oldFs       = framestore_;
				oldSink     = framesink_;
				oldObs      = observer_;
				oldDormant = dormant_;
			}

			struct BindCandidate
			{
				FrameStore* store = nullptr;
				BridgeObserver* observer = nullptr;
				~BindCandidate()
				{
					delete observer;
					safe_release(store);
				}
				void Commit()
				{
					store = nullptr;
					observer = nullptr;
				}
			} candidate;
			if( external ) {
				external->addref();
				candidate.store = external;
				if( chainConstructionTestHook_ ) {
					chainConstructionTestHook_("bind_after_retain");
				}
				candidate.observer = new BridgeObserver(*this);
				if( chainConstructionTestHook_ ) {
					chainConstructionTestHook_("bind_after_observer_allocation");
				}
				candidate.store->SetCameraExposureEV(
					static_cast<double>(cameraExposureEV_));
			}

			std::optional<FrameStore::ObserverMutationToken> candidateRegistration;
			if( candidate.store ) {
				candidateRegistration.emplace(
					candidate.store->PrepareObserverRegistration());
			}
			std::optional<FrameStore::ObserverMutationToken> oldObserverRemoval;
			std::array<std::optional<FrameStore::ObserverMutationToken>,
				kMaxDormantChains> dormantRemovals;
			for( size_t i=0; i<oldDormant.size(); ++i ) {
				DormantChain& d = oldDormant[i];
				if( d.fs && d.obs ) {
					dormantRemovals[i].emplace(
						d.fs->PrepareObserverRemoval(d.obs));
				}
			}
			if( oldFs && oldObs ) {
				oldObserverRemoval.emplace(oldFs->PrepareObserverRemoval(oldObs));
			}
			if( chainConstructionTestHook_ ) {
				chainConstructionTestHook_("bind_after_old_observer_quiesced");
			}

			FrameStore* committedStore = candidate.store;
			std::vector<FrameStore::ObserverMutationToken*> mutationTokens;
			if( candidateRegistration ) {
				mutationTokens.push_back(&*candidateRegistration);
			}
			if( oldObserverRemoval ) {
				mutationTokens.push_back(&*oldObserverRemoval);
			}
			for( size_t i=0; i<oldDormant.size(); ++i ) {
				if( dormantRemovals[i] && dormantRemovals[i]->IsPrepared() ) {
					mutationTokens.push_back(&*dormantRemovals[i]);
				}
			}
			FrameStore::LockPreparedObserverMutations(mutationTokens);
			{
				std::unique_lock<std::shared_mutex> lock(chainMutex_);
				externalFrameStore_ = candidate.store;
				framestore_ = candidate.store;
				framesink_ = nullptr;
				observer_ = candidate.observer;
				dormant_.clear();
				if( candidate.observer ) candidate.observer->Activate();
				if( candidateRegistration ) {
					bool replacementCommitted = false;
					if( oldObserverRemoval && candidate.store == oldFs ) {
						candidate.store->CommitPreparedObserverReplacement(
							*candidateRegistration,*oldObserverRemoval,
							candidate.observer);
						replacementCommitted = true;
					} else {
						for( size_t i=0; i<oldDormant.size(); ++i ) {
							if( dormantRemovals[i] &&
								candidate.store == oldDormant[i].fs ) {
								candidate.store->CommitPreparedObserverReplacement(
									*candidateRegistration,*dormantRemovals[i],
									candidate.observer);
								replacementCommitted = true;
								break;
							}
						}
					}
					if( !replacementCommitted ) {
						candidate.store->CommitPreparedObserverRegistration(
							*candidateRegistration,candidate.observer);
					}
				}
				if( oldObserverRemoval && oldObserverRemoval->IsPrepared() ) {
					oldFs->CommitPreparedObserverRemoval(*oldObserverRemoval);
				}
				candidate.Commit();
			}
			for( size_t i=0; i<oldDormant.size(); ++i ) {
				if( dormantRemovals[i] && dormantRemovals[i]->IsPrepared() ) {
					oldDormant[i].fs->CommitPreparedObserverRemoval(
						*dormantRemovals[i]);
				}
			}

			delete oldObs;
			safe_release( oldSink );
			if ( oldExternal ) {
				// External-bound: framestore_ was an alias for
				// externalFrameStore_; the addref lived on
				// externalFrameStore_.  Release through it.
				safe_release( oldExternal );
			} else {
				// Internal mode: framestore_ owned its addref.
				safe_release( oldFs );
			}
			for ( auto& d : oldDormant ) {
				delete d.obs;
				safe_release( d.sink );
				safe_release( d.fs );
			}

			if ( committedStore ) {
				// L8 round-18d — eLog_Info (was eLog_Event).  With the
				// interactive preview-scale path firing this message on
				// every dim transition (4 -> 8 -> 4 -> 2 -> 1 across a
				// single drag), the in-app log window was being spammed
				// with one line per pass.  Demote to Info — the
				// console / file logger still records it for debugging
				// (eLog_All includes Info), but the user-facing window
				// (eLog_Console = Serious | Event) hides it.
				GlobalLog()->PrintEx( eLog_Info,
					"ViewportFrameStore::BindFrameStore: bound to "
					"external FrameStore %ux%u",
					static_cast<unsigned int>( committedStore->Width() ),
					static_cast<unsigned int>( committedStore->Height() ) );
			} else if( bindRevision_.load(std::memory_order_acquire) == requestRevision ) {
				GlobalLog()->PrintEx( eLog_Info,
					"ViewportFrameStore::BindFrameStore: unbound — "
					"reverted to internal-managed mode" );
			}
		}

		bool ViewportFrameStore::IsExternallyBound() const
		{
			std::shared_lock<std::shared_mutex> lock( chainMutex_ );
			return externalFrameStore_ != nullptr;
		}

		void ViewportFrameStore::ForTest_SetBindPhaseOneHook(
			std::function<void(uint64_t)> hook )
		{
			bindPhaseOneTestHook_ = std::move(hook);
		}

		void ViewportFrameStore::ForTest_SetChainConstructionHook(
			std::function<void(const char*)> hook )
		{
			chainConstructionTestHook_ = std::move(hook);
		}

		// L6e-2b — Notification override.  `Rasterizer::SetFrameStore`
		// dispatches this to every attached IRasterizerOutput (the VFS
		// is one such output; see `Attach`).  Forward to
		// `BindFrameStore` — same lifecycle as a manual bind.
		void ViewportFrameStore::OnRasterizerFrameStoreChanged(
			FrameStore* framestore )
		{
			BindFrameStore( framestore );
		}

		// ─────────────────────────────────────────────────────────────
		// State queries
		// ─────────────────────────────────────────────────────────────

		// Capture a stable, addref'd snapshot of framestore_ under
		// the shared chain-mutex.  Returns nullptr if the chain
		// hasn't been allocated yet.  Caller must release() the
		// returned pointer when done with it.  This is the
		// foundation of the L4 round-2 P1-2 fix: readers operate
		// against a private addref so a concurrent EnsureChain
		// reallocation can't invalidate them mid-work.
		static FrameStore* SnapshotFrameStore(
			std::shared_mutex& mtx,
			FrameStore* const& ptr )
		{
			std::shared_lock<std::shared_mutex> lock( mtx );
			if ( !ptr ) return nullptr;
			ptr->addref();
			return ptr;
		}

		// L6e-2a — same pattern as SnapshotFrameStore but for
		// `framesink_`.  Used by the legacy-mode fallback in the
		// IRasterizerOutput passthrough methods (OutputImage etc.)
		// so a concurrent `BindFrameStore` swap on another thread
		// can't tear down `framesink_` mid-call.  Caller releases.
		static FrameSink* SnapshotFrameSink(
			std::shared_mutex& mtx,
			FrameSink* const& ptr )
		{
			std::shared_lock<std::shared_mutex> lock( mtx );
			if ( !ptr ) return nullptr;
			ptr->addref();
			return ptr;
		}

		uint64_t ViewportFrameStore::Generation() const
		{
			RetainedReference<FrameStore> snap(
				SnapshotFrameStore( chainMutex_, framestore_ ) );
			if ( !snap ) return 0;
			const uint64_t gen = snap->Generation();
			return gen;
		}

		void ViewportFrameStore::GetDimensions(
			unsigned int& outW, unsigned int& outH ) const
		{
			// Same snapshot-and-addref pattern as Generation() and
			// RenderToBuffer — guards against a concurrent
			// EnsureChain reallocation freeing the FrameStore between
			// the chain-pointer read and the Width()/Height() deref.
			// See L4 round-4 P2-D adversarial review.
			outW = 0;
			outH = 0;
			RetainedReference<FrameStore> snap(
				SnapshotFrameStore( chainMutex_, framestore_ ) );
			if ( !snap ) return;
			outW = static_cast<unsigned int>( snap->Width() );
			outH = static_cast<unsigned int>( snap->Height() );
		}

		// ─────────────────────────────────────────────────────────────
		// Display refresh
		// ─────────────────────────────────────────────────────────────

		void ViewportFrameStore::RenderToBuffer(
			void*                dst,
			size_t               dstStride,
			const Rect&          roi,
			FrameStoreOutput::TargetFormat fmt,
			const FrameStoreOutput::ViewTransform& xform,
			bool                 nonBlocking ) const
		{
			RetainedReference<FrameStore> snap(
				SnapshotFrameStore( chainMutex_, framestore_ ) );
			if ( !snap ) return;
			snap->Render( dst, dstStride, roi, fmt, xform, nonBlocking );
		}

		// ─────────────────────────────────────────────────────────────
		// Save As
		// ─────────────────────────────────────────────────────────────

		bool ViewportFrameStore::SaveAs(
			const std::string& path,
			IFrameEncoder*     encoder,
			const EncodeOpts&  opts ) const
		{
			if ( !encoder ) return false;
			if( !EncoderAcceptsPath(*encoder,path) ) {
				GlobalLog()->PrintEx( eLog_Error,
					"ViewportFrameStore::SaveAs: unavailable encoder for authored path '%s'",
					path.c_str() );
				return false;
			}
			RetainedReference<FrameStore> snap(
				SnapshotFrameStore( chainMutex_, framestore_ ) );
			if ( !snap ) return false;

			EncodeOpts transactionOpts = opts;
			bool artifactMetadataLeased = false;
			if( !snap->AcquireExternalArtifactMetadataSnapshot(
				transactionOpts.metadataSnapshot,artifactMetadataLeased) ) {
				GlobalLog()->PrintEx( eLog_Error,
					"ViewportFrameStore::SaveAs: output_provenance_unavailable for '%s': "
					"fire render is not finalized",path.c_str() );
				return false;
			}
			struct FireMetadataLease
			{
				FrameStore* store;
				bool active;
				~FireMetadataLease()
				{
					if( active ) store->ReleaseExternalArtifactMetadataLease();
				}
			};
			transactionOpts.useMetadataSnapshot = true;
			transactionOpts.frame = transactionOpts.metadataSnapshot.frame;
			transactionOpts.denoisedDerivative =
				transactionOpts.metadataSnapshot.denoisedContent;
			std::string error;
			bool success = false;
			{
				FireMetadataLease fireMetadataLease { snap.get(),artifactMetadataLeased };
				success = EncodeFrameStoreFileTransaction(
					*snap,*encoder,transactionOpts,path,error );
			}
			if( !success ) {
				GlobalLog()->PrintEx( eLog_Error,
					"ViewportFrameStore::SaveAs: output_provenance_unavailable for '%s': %s",
					path.c_str(),error.c_str() );
				return false;
			}
			GlobalLog()->PrintEx( eLog_Event,
				"ViewportFrameStore::SaveAs: written to '%s'", path.c_str() );
			return true;
		}

		bool ViewportFrameStore::SaveTo(
			IWriteBuffer&     dst,
			IFrameEncoder*    encoder,
			const EncodeOpts& opts ) const
		{
			if ( !encoder ) return false;
			RetainedReference<FrameStore> snap(
				SnapshotFrameStore( chainMutex_, framestore_ ) );
			if ( !snap ) return false;
			FrameStoreOutput::Metadata metadataSnapshot;
			bool artifactMetadataLeased = false;
			if( !snap->AcquireExternalArtifactMetadataSnapshot(
				metadataSnapshot,artifactMetadataLeased) ) {
				GlobalLog()->PrintEasyError(
					"ViewportFrameStore::SaveTo: output metadata is unavailable" );
				return false;
			}
			struct ArtifactMetadataLease
			{
				FrameStore* store;
				bool active;
				~ArtifactMetadataLease()
				{
					if( active ) store->ReleaseExternalArtifactMetadataLease();
				}
			} metadataLease { snap.get(),artifactMetadataLeased };
			if( !metadataSnapshot.renderFidelityStatus.empty() ) {
				GlobalLog()->PrintEasyError(
					"ViewportFrameStore::SaveTo: fire output requires a provenance-capable sink" );
				return false;
			}
			encoder->Encode( *snap, dst, opts );
			return true;
		}

		// ─────────────────────────────────────────────────────────────
		// IRasterizerOutput passthrough
		// ─────────────────────────────────────────────────────────────

		void ViewportFrameStore::OutputIntermediateImage(
			const IRasterImage& pImage,
			const Rect*         pRegion )
		{
			// L6e-2a — When externally bound, the rasterizer's
			// per-tile `BeginTile/EndTile` bracketing (post-L6e-1)
			// has ALREADY fired `OnTileComplete` on the same
			// FrameStore we observe.  Re-running
			// `CopyTileFromRasterImage` here would (a) re-copy data
			// that's already in the canonical store, and (b) double-
			// fire the observer chain → repaints + UI work for no
			// pixel change.  Short-circuit.
			{
				std::shared_lock<std::shared_mutex> lock( chainMutex_ );
				if ( externalFrameStore_ ) return;
			}

			EnsureChain( pImage.GetWidth(), pImage.GetHeight() );

			// Unlike FrameSink::OutputIntermediateImage (which
			// no-ops to match the legacy file-output behaviour),
			// the GUI viewport WANTS per-tile progressive updates.
			// We bypass framesink_ for intermediates and copy the
			// updated region's pixels directly into the FrameStore
			// via per-tile BeginTile/EndTile pairs.  EndTile fires
			// FrameStore observers' OnTileComplete (which fans out
			// to the user's TileCompleteCallback through
			// BridgeObserver) — exactly what platform repaint
			// loops listen for.  No MarkFrameComplete: this is
			// progressive, not final.  See L4 round-2 review P1-1.
			RetainedReference<FrameStore> snap(
				SnapshotFrameStore( chainMutex_, framestore_ ) );
			if ( !snap ) return;

			const unsigned int srcW = pImage.GetWidth();
			const unsigned int srcH = pImage.GetHeight();

			// Determine the affected pixel range.  RISE's `Rect`
			// uses INCLUSIVE bounds — top/left/bottom/right are
			// pixel indices, the rect covers
			// [top, bottom] × [left, right] (closed interval).
			// See PixelBasedRasterizerHelper::BoundsFromRect at
			// PixelBasedRasterizerHelper.h:193-214 and the
			// `<= rect.bottom/right` loops in
			// PixelBasedRasterizerHelper.cpp.  When pRegion is
			// nullptr the rasterizer means "the whole image"
			// (per IRasterizerOutput.h:33-36).
			//
			// We convert to exclusive bounds (rExc{Right,Bottom}
			// = pRegion->{right,bottom} + 1, clamped to image
			// size) before the tile-coverage math below — that
			// math (`(rExcRight + te - 1) / te`) is the standard
			// half-open ceiling division for a [0, end) range.
			// Without the conversion, single-pixel regions sitting
			// exactly on a tile boundary (e.g. Rect(32, 32, 32, 32)
			// at te=32) compute tx1 == tx0 and fire NO tile
			// callbacks.  See L4 round-3 P2.
			const unsigned int rTop      = pRegion ? pRegion->top  : 0;
			const unsigned int rLeft     = pRegion ? pRegion->left : 0;
			const unsigned int rExcBottom = pRegion
				? std::min<unsigned int>( pRegion->bottom + 1u, srcH )
				: srcH;
			const unsigned int rExcRight  = pRegion
				? std::min<unsigned int>( pRegion->right  + 1u, srcW )
				: srcW;

			// Iterate FrameStore tiles overlapping the region;
			// each CopyTileFromRasterImage call brackets the
			// per-tile write in BeginTile/EndTile, which fires
			// OnTileComplete for that tile.  The rasterizer's
			// just-rendered region may not align with the
			// FrameStore's tile grid; we copy the WHOLE FrameStore
			// tile for any tile that overlaps, which incidentally
			// re-reads pixels outside the rendered region (those
			// pixels are still present in the rasterizer's image
			// from previous-state, so the copy is correct).
			const size_t te  = snap->TileEdge();
			const size_t tcX = snap->TileCountX();
			const size_t tcY = snap->TileCountY();
			const size_t tx0 = static_cast<size_t>( rLeft ) / te;
			const size_t ty0 = static_cast<size_t>( rTop  ) / te;
			const size_t tx1 = std::min<size_t>( tcX,
				( static_cast<size_t>( rExcRight  ) + te - 1 ) / te );
			const size_t ty1 = std::min<size_t>( tcY,
				( static_cast<size_t>( rExcBottom ) + te - 1 ) / te );

			for ( size_t ty = ty0; ty < ty1; ++ty ) {
				for ( size_t tx = tx0; tx < tx1; ++tx ) {
					const unsigned int dstX0 = static_cast<unsigned int>( tx * te );
					const unsigned int dstY0 = static_cast<unsigned int>( ty * te );
					const unsigned int dstX1 = static_cast<unsigned int>(
						std::min( ( tx + 1 ) * te,
						          static_cast<size_t>( snap->Width() ) ) );
					const unsigned int dstY1 = static_cast<unsigned int>(
						std::min( ( ty + 1 ) * te,
						          static_cast<size_t>( snap->Height() ) ) );
					const Rect srcRect( dstY0, dstX0, dstY1, dstX1 );
					snap->CopyTileFromRasterImage( tx, ty, pImage, srcRect );
				}
			}

		}

		void ViewportFrameStore::OutputImage(
			const IRasterImage& pImage,
			const Rect*         pRegion,
			const unsigned int  frame )
		{
			// L6e-2a / L6f — externally bound: complete no-op.  The
			// rasterizer's per-tile `BeginTile/EndTile` (post-L6e-1)
			// already drove `OnTileComplete`; the rasterizer's
			// post-flush `MarkFrameComplete` (post-L6f) on the
			// canonical store now drives `OnFrameComplete` —
			// observers fan out from there.  Pre-L6f this branch
			// also fired `MarkFrameComplete` itself; that's now the
			// rasterizer's job, and firing here would DOUBLE-FIRE
			// observers on the same store.
			{
				std::shared_lock<std::shared_mutex> lock( chainMutex_ );
				if ( externalFrameStore_ ) return;
			}

			EnsureChain( pImage.GetWidth(), pImage.GetHeight() );
			// L6e-2a — snapshot framesink_ under chainMutex_ so a
			// concurrent BindFrameStore swap can't tear it down
			// mid-call.  Pre-fix: raw deref with no chain-lock —
			// safe under the single-rasterizer-thread contract that
			// drives Output*Image, but BindFrameStore can be called
			// from any thread (typically a UI thread post-L6e-2c),
			// breaking the contract's assumption.  See L6e-2a
			// adversarial review P1.
			RetainedReference<FrameSink> sinkSnap(
				SnapshotFrameSink( chainMutex_, framesink_ ) );
			if ( sinkSnap ) {
				sinkSnap->OutputImage( pImage, pRegion, frame );
			}
		}

		void ViewportFrameStore::OutputPreDenoisedImage(
			const IRasterImage& pImage,
			const Rect*         pRegion,
			const unsigned int  frame )
		{
			// L6e-2a / L6f — externally bound: complete no-op.
			// Rasterizer drives `MarkPreDenoiseComplete` post-flush.
			{
				std::shared_lock<std::shared_mutex> lock( chainMutex_ );
				if ( externalFrameStore_ ) return;
			}

			EnsureChain( pImage.GetWidth(), pImage.GetHeight() );
			RetainedReference<FrameSink> sinkSnap(
				SnapshotFrameSink( chainMutex_, framesink_ ) );
			if ( sinkSnap ) {
				sinkSnap->OutputPreDenoisedImage( pImage, pRegion, frame );
			}
		}

		void ViewportFrameStore::OutputDenoisedImage(
			const IRasterImage& pImage,
			const Rect*         pRegion,
			const unsigned int  frame )
		{
			// L6e-2a / L6f — externally bound: complete no-op.
			// Rasterizer drives `MarkDenoiseComplete` post-flush.
			{
				std::shared_lock<std::shared_mutex> lock( chainMutex_ );
				if ( externalFrameStore_ ) return;
			}

			EnsureChain( pImage.GetWidth(), pImage.GetHeight() );
			RetainedReference<FrameSink> sinkSnap(
				SnapshotFrameSink( chainMutex_, framesink_ ) );
			if ( sinkSnap ) {
				sinkSnap->OutputDenoisedImage( pImage, pRegion, frame );
			}
		}

		void ViewportFrameStore::SetCameraExposureCompensationEV( Scalar ev )
		{
			FrameStore* retained = nullptr;
			{
				std::unique_lock<std::shared_mutex> lock( chainMutex_ );
				cameraExposureEV_ = ev;
				retained = framestore_;
				if( retained ) retained->addref();
			}
			RetainedReference<FrameStore> snap(retained);
			if ( snap ) {
				snap->SetCameraExposureEV(static_cast<double>(ev));
			}
		}

		// ─────────────────────────────────────────────────────────────
		// EnsureChain — lazy alloc / resolution-change reallocate
		// ─────────────────────────────────────────────────────────────

		void ViewportFrameStore::EnsureChain( unsigned int width, unsigned int height )
		{
			// Precondition: this method is called from the
			// rasterizer worker thread that drives Output*Image,
			// and only one such thread is active at a time (the
			// "single active rasterizer" contract from §7.5 of
			// docs/FRAMESTORE_DESIGN.md).  TeardownChain below
			// calls FrameStore::RemoveObserver, which waits for
			// in-flight observer dispatches.  Under the single-
			// rasterizer-thread contract no other dispatch can be
			// in flight on this VFS, so the wait returns
			// immediately.  If the contract is ever relaxed
			// (e.g. two rasterizers concurrently driving the same
			// VFS), this reasoning breaks; see L4 adversarial
			// review MED-3.
			//
			// L6e-2a — When externally bound (`BindFrameStore`),
			// internal allocation is skipped entirely — `framestore_`
			// already points at the rasterizer's `mFrameStore`, which
			// is sized + managed by the Job layer.  Dimension
			// mismatches in the externally-bound case are a Job-layer
			// concern (resolution change re-binds via L6e-2b's
			// rebinding hook) — the VFS just observes.
			{
				std::shared_lock<std::shared_mutex> lock( chainMutex_ );
				if ( bindTransactionsInFlight_.load(std::memory_order_acquire) != 0u ||
					externalFrameStore_ ) {
					return;
				}
			}

			// Fast path: dims match — read framestore_ pointer
			// under shared_lock (other readers may be in
			// RenderToBuffer / SaveAs / Generation concurrently).
			{
				std::shared_lock<std::shared_mutex> lock( chainMutex_ );
				if( bindTransactionsInFlight_.load(std::memory_order_acquire) != 0u ) {
					return;
				}
				if ( framestore_ &&
				     framestore_->Width()  == width &&
				     framestore_->Height() == height )
				{
					return;
				}
			}

			// Slow path: dim mismatch.  Take unique_lock to swap the
			// chain pointers.  Reader threads holding addref'd
			// snapshots of the OLD framestore_ are unaffected —
			// their reference keeps the old store alive until they
			// release.  Reader threads NOT yet inside a snapshot
			// (about to take the shared_lock) wait until our swap
			// completes; they then see the NEW framestore_.
			//
			// L8 review round 4 — the LRU eviction's `RemoveObserver`
			// must run OUTSIDE chainMutex_ to avoid the dispatch-mutex
			// inversion documented in `BindFrameStore`.  We snapshot
			// the eviction candidate here (under the lock) and tear it
			// down post-lock.  Same dormant cache semantics; fewer
			// deadlock paths.
			DormantChain evicted;
			DormantChain replacement;
			{
				std::unique_lock<std::shared_mutex> lock( chainMutex_ );
				if( bindTransactionsInFlight_.load(std::memory_order_acquire) != 0u ||
					externalFrameStore_ ) {
					return;
				}

				// Re-check under unique_lock (a concurrent rasterizer
				// thread shouldn't be possible per the contract above,
				// but defensive against future contract relaxations).
				if ( framestore_ &&
				     framestore_->Width()  == width &&
				     framestore_->Height() == height )
				{
					return;
				}

				// Select or fully construct the replacement before parking the
				// active chain.  Every throwing allocation and observer-registration
				// step therefore has the strong guarantee: the currently published
				// FrameStore remains intact.
				for ( auto it = dormant_.begin(); it != dormant_.end(); ++it ) {
					if ( it->w == width && it->h == height ) {
						replacement = *it;
						dormant_.erase( it );
						break;
					}
				}

				if ( !replacement.fs ) {
					GlobalLog()->PrintEx( eLog_Info,
						"ViewportFrameStore:: allocating new FrameStore "
						"chain for %ux%u (dormant cache size %zu).",
						width, height, dormant_.size() );
					try {
						FrameStore::Spec spec;
						spec.width    = width;
						spec.height   = height;
						spec.tileEdge = 32;
						replacement.fs = new FrameStore(spec);
						replacement.w = width;
						replacement.h = height;
						replacement.fs->SetCameraExposureEV(
							static_cast<double>(cameraExposureEV_));
						if( chainConstructionTestHook_ ) {
							chainConstructionTestHook_("ensure_after_store");
						}
						replacement.sink = new FrameSink(replacement.fs);
						if( chainConstructionTestHook_ ) {
							chainConstructionTestHook_("ensure_after_sink");
						}
						replacement.obs = new BridgeObserver(*this);
						if( chainConstructionTestHook_ ) {
							chainConstructionTestHook_("ensure_after_observer_allocation");
						}
						replacement.fs->AddObserver(replacement.obs);
						if( chainConstructionTestHook_ ) {
							chainConstructionTestHook_("ensure_after_observer_registration");
						}
					} catch ( ... ) {
						lock.unlock();
						TeardownDormant_unlocked(replacement);
						throw;
					}
				}

				ParkActiveAsDormant_locked(evicted);
				framestore_ = replacement.fs;
				framesink_ = replacement.sink;
				observer_ = replacement.obs;
				replacement = DormantChain();
				framestore_->SetCameraExposureEV(
					static_cast<double>(cameraExposureEV_));
				observer_->Activate();
			}  // chainMutex_ released here

			// Post-lock teardown of the LRU eviction (if any).  Same
			// rationale as `BindFrameStore` Phase 3 — RemoveObserver
			// can wait for in-flight dispatches whose callbacks need
			// chainMutex_ shared_lock; calling it without the lock
			// lets those dispatches drain.  No-op on empty/default
			// DormantChain.
			try {
				TeardownDormant_unlocked(evicted);
			} catch ( ... ) {
				std::unique_lock<std::shared_mutex> lock(chainMutex_);
				if( evicted.fs ) {
					dormant_.push_back(evicted);
					evicted = DormantChain();
				}
				throw;
			}
		}

	} // namespace Implementation
} // namespace RISE
