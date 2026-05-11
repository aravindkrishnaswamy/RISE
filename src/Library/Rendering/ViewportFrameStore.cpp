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
#include "../Interfaces/IRasterizer.h"
#include "../Interfaces/IRenderObserver.h"
#include "../Interfaces/IRasterImage.h"
#include "../Interfaces/IFrameEncoder.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/DiskFileWriteBuffer.h"

#include <algorithm>
#include <cassert>
#include <mutex>
#include <shared_mutex>

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	namespace Implementation
	{

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
				: parent_( parent )
			{
			}

			void OnTileComplete( const Rect& roi, uint64_t generation ) override
			{
				if ( parent_.tileCb_ ) parent_.tileCb_( roi, generation );
			}

			void OnFrameComplete( unsigned int frame, uint64_t generation ) override
			{
				if ( parent_.frameCb_ ) parent_.frameCb_( frame, generation );
			}

			void OnPreDenoiseComplete( unsigned int frame, uint64_t generation ) override
			{
				if ( parent_.preDenoiseCb_ ) parent_.preDenoiseCb_( frame, generation );
			}

			void OnDenoiseComplete( unsigned int frame, uint64_t generation ) override
			{
				if ( parent_.denoiseCb_ ) parent_.denoiseCb_( frame, generation );
			}

		private:
			ViewportFrameStore& parent_;
		};

		// ─────────────────────────────────────────────────────────────
		// Construction / destruction
		// ─────────────────────────────────────────────────────────────

		ViewportFrameStore::ViewportFrameStore()
			: cameraExposureEV_( Scalar( 0 ) )
		{
		}

		ViewportFrameStore::~ViewportFrameStore()
		{
			// Same Output*-vs-dtor contract as FileRasterizerOutput:
			// the platform code is responsible for ensuring no
			// rasterizer thread is mid-OutputImage when this
			// destructs (typically by Detach()-ing first AND
			// joining any rasterizer threads).
			//
			// L8 review round 3 — DEADLOCK FIX: route through the
			// phased BindFrameStore(nullptr) path rather than holding
			// chainMutex_ unique_lock around `TeardownChain()`'s
			// `RemoveObserver` wait.  BindFrameStore's Phase 1
			// snapshots + clears under the lock, Phase 2/3 tear
			// down OUTSIDE the lock (so observer dispatches that
			// re-enter chainMutex_ via vfs->RenderToBuffer can
			// complete), Phase 4 is a no-op for external==nullptr.
			// See BindFrameStore comment for the full rationale.
			//
			// Snapshot semantics for in-flight readers: same as
			// pre-fix.  Reader threads that already captured an
			// addref'd `framestore_` snapshot continue to hold it
			// alive past Phase 1's clear; their work completes
			// against the captured pointer; they release.  Phase 3
			// then sees zero refs and destroys the FrameStore.
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
		// Replacement: `BindFrameStore(nullptr)` does the same
		// teardown work via the phased pattern (snapshot + drop
		// lock + RemoveObserver + cleanup + nothing-to-install).
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
			rasterizer->AddRasterizerOutput( this );

			// L6e-2b — Auto-bind to the rasterizer's current
			// FrameStore (if non-null).  Two cases at Attach time:
			//   * Rasterizer already has a FrameStore (typical: Job
			//     pushed it on scene-load before any bridge attached
			//     a VFS).  Bind immediately.
			//   * Rasterizer's FrameStore is null (atypical: VFS
			//     attached pre-Job-push).  Stay in internal-managed
			//     mode for now; when Job's
			//     `PushJobFrameStoreToRasterizers` later fires
			//     `Rasterizer::SetFrameStore`, our
			//     `OnRasterizerFrameStoreChanged` override picks it
			//     up automatically (we're now in the rasterizer's
			//     outputs list).
			//
			// Benign TOCTOU: if a second thread invokes
			// `SetFrameStore(newFs)` after our
			// `AddRasterizerOutput` but before the
			// `GetFrameStore()` read below, we receive the
			// notification (newFs) AND race-read the new value.
			// Result: we bind to newFs twice (once via
			// notification, once via the GetFrameStore read).
			// `BindFrameStore` is idempotent on same-pointer rebind,
			// so the second bind is a no-op.  See L6e-2b
			// adversarial review P2.
			FrameStore* fs = rasterizer->GetFrameStore();
			if ( fs ) {
				BindFrameStore( fs );
			}
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
			// L8 review round 3 — DEADLOCK FIX.
			//
			// Pre-fix: this method held `chainMutex_` unique_lock the
			// whole way, including across `TeardownChain()`'s
			// `RemoveObserver` call.  `RemoveObserver` blocks until
			// any in-flight observer dispatch on the old store
			// returns.  But the observer chain ultimately calls back
			// into `vfs->RenderToBuffer` (the bridge's tile-complete
			// callback path), which itself takes `chainMutex_`
			// shared_lock.  Worker thread blocked waiting for
			// shared_lock → main thread blocked waiting for worker's
			// dispatch to finish → DEADLOCK.  Reproduced as user-
			// reported hang on second-render-after-scene-reload.
			//
			// Post-fix: phase the work so `RemoveObserver` runs
			// WITHOUT `chainMutex_` held:
			//   1. Snapshot old state under unique_lock + clear
			//      member pointers.  Concurrent readers from this
			//      point see "no chain" (RenderToBuffer no-ops).
			//   2. Release the lock.
			//   3. Call `RemoveObserver` + cleanup.  Workers can
			//      acquire shared_lock for RenderToBuffer; their
			//      dispatches complete; in-flight counter
			//      decrements; RemoveObserver returns.
			//   4. Re-acquire the lock to install the new state.
			//
			// Consequence: brief gap between Phase 2 and Phase 4
			// where readers see no chain.  Acceptable — same window
			// as a transient unbind-then-bind cycle, and the bridge's
			// tile callbacks during that gap simply don't update.

			// ----- Phase 1: snapshot + clear under unique_lock. -----
			FrameStore*     oldExternal = nullptr;
			FrameStore*     oldFs       = nullptr;
			FrameSink*      oldSink     = nullptr;
			BridgeObserver* oldObs      = nullptr;
			std::vector<DormantChain> oldDormant;
			{
				std::unique_lock<std::shared_mutex> lock( chainMutex_ );

				// Idempotent — re-binding the same pointer is a no-op,
				// avoids tearing down + re-registering an observer that
				// would point at the same store.
				if ( external == externalFrameStore_ ) {
					return;
				}

				// Snapshot member state into locals + clear members.
				oldExternal = externalFrameStore_;
				oldFs       = framestore_;
				oldSink     = framesink_;
				oldObs      = observer_;
				oldDormant.swap( dormant_ );

				externalFrameStore_ = nullptr;
				framestore_         = nullptr;
				framesink_          = nullptr;
				observer_           = nullptr;
			}

			// ----- Phase 2/3: teardown OUTSIDE chainMutex_. -----
			// RemoveObserver can wait for in-flight observer
			// dispatches without blocking workers that need
			// chainMutex_ shared_lock for RenderToBuffer.
			if ( oldFs && oldObs ) {
				oldFs->RemoveObserver( oldObs );
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
			// Drain dormant cache (only populated in internal mode;
			// external mode bypasses the cache entirely).
			for ( auto& d : oldDormant ) {
				if ( d.fs && d.obs ) {
					d.fs->RemoveObserver( d.obs );
				}
				delete d.obs;
				safe_release( d.sink );
				safe_release( d.fs );
			}

			// ----- Phase 4: install new state under unique_lock. -----
			// Bind to the new external (if non-null).  `nullptr`
			// reverts to internal-managed mode — the next
			// `Output*Image` call will lazy-allocate a fresh internal
			// store via `EnsureChain`.
			if ( external ) {
				std::unique_lock<std::shared_mutex> lock( chainMutex_ );
				external->addref();  // VFS owns one defensive ref
				externalFrameStore_ = external;
				framestore_         = external;
				// `framesink_` stays null — the IRasterizerOutput
				// chain becomes a no-op for VFS bound to an external
				// store (the rasterizer's per-tile bracketing already
				// fires observers on the same store).

				// Build observer + register on the external store.
				// `BridgeObserver` is internal-only lifecycle managed
				// by us, same pattern as `EnsureChain`.
				observer_ = new BridgeObserver( *this );
				external->AddObserver( observer_ );

				// Re-apply the camera EV snapshot (matches
				// `EnsureChain` post-allocate / dormant-rehydrate).
				external->MutableMeta().cameraExposureEV =
					static_cast<double>( cameraExposureEV_ );

				GlobalLog()->PrintEx( eLog_Event,
					"ViewportFrameStore::BindFrameStore: bound to "
					"external FrameStore %ux%u",
					static_cast<unsigned int>( external->Width() ),
					static_cast<unsigned int>( external->Height() ) );
			} else {
				GlobalLog()->PrintEx( eLog_Event,
					"ViewportFrameStore::BindFrameStore: unbound — "
					"reverted to internal-managed mode" );
			}
		}

		bool ViewportFrameStore::IsExternallyBound() const
		{
			std::shared_lock<std::shared_mutex> lock( chainMutex_ );
			return externalFrameStore_ != nullptr;
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
			FrameStore* snap = SnapshotFrameStore( chainMutex_, framestore_ );
			if ( !snap ) return 0;
			const uint64_t gen = snap->Generation();
			snap->release();
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
			FrameStore* snap = SnapshotFrameStore( chainMutex_, framestore_ );
			if ( !snap ) return;
			outW = static_cast<unsigned int>( snap->Width() );
			outH = static_cast<unsigned int>( snap->Height() );
			snap->release();
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
			FrameStore* snap = SnapshotFrameStore( chainMutex_, framestore_ );
			if ( !snap ) return;
			snap->Render( dst, dstStride, roi, fmt, xform, nonBlocking );
			snap->release();
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
			FrameStore* snap = SnapshotFrameStore( chainMutex_, framestore_ );
			if ( !snap ) return false;

			DiskFileWriteBuffer* buf = new DiskFileWriteBuffer( path.c_str() );
			if ( !buf->ReadyToWrite() ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"ViewportFrameStore::SaveAs: failed to open '%s' for writing",
					path.c_str() );
				safe_release( buf );
				snap->release();
				return false;
			}

			GlobalLog()->PrintNew( buf, __FILE__, __LINE__, "DiskFileWriteBuffer" );
			encoder->Encode( *snap, *buf, opts );
			safe_release( buf );
			snap->release();

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
			FrameStore* snap = SnapshotFrameStore( chainMutex_, framestore_ );
			if ( !snap ) return false;
			encoder->Encode( *snap, dst, opts );
			snap->release();
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
			FrameStore* snap = SnapshotFrameStore( chainMutex_, framestore_ );
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

			snap->release();
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
			FrameSink* sinkSnap = SnapshotFrameSink( chainMutex_, framesink_ );
			if ( sinkSnap ) {
				sinkSnap->OutputImage( pImage, pRegion, frame );
				sinkSnap->release();
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
			FrameSink* sinkSnap = SnapshotFrameSink( chainMutex_, framesink_ );
			if ( sinkSnap ) {
				sinkSnap->OutputPreDenoisedImage( pImage, pRegion, frame );
				sinkSnap->release();
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
			FrameSink* sinkSnap = SnapshotFrameSink( chainMutex_, framesink_ );
			if ( sinkSnap ) {
				sinkSnap->OutputDenoisedImage( pImage, pRegion, frame );
				sinkSnap->release();
			}
		}

		void ViewportFrameStore::SetCameraExposureCompensationEV( Scalar ev )
		{
			cameraExposureEV_ = ev;
			// Snapshot framestore_ under chain-mutex shared lock
			// so a concurrent reallocation (which holds unique_lock)
			// can't race the meta write.
			FrameStore* snap = SnapshotFrameStore( chainMutex_, framestore_ );
			if ( snap ) {
				snap->MutableMeta().cameraExposureEV =
					static_cast<double>( ev );
				snap->release();
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
				if ( externalFrameStore_ ) {
					return;
				}
			}

			// Fast path: dims match — read framestore_ pointer
			// under shared_lock (other readers may be in
			// RenderToBuffer / SaveAs / Generation concurrently).
			{
				std::shared_lock<std::shared_mutex> lock( chainMutex_ );
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
			{
				std::unique_lock<std::shared_mutex> lock( chainMutex_ );

				// Re-check under unique_lock (a concurrent rasterizer
				// thread shouldn't be possible per the contract above,
				// but defensive against future contract relaxations).
				if ( framestore_ &&
				     framestore_->Width()  == width &&
				     framestore_->Height() == height )
				{
					return;
				}

				// L5a round-8 — preview-scale oscillation reuse.  Park
				// the current active (if any) into dormant_ rather than
				// destroying it.  A subsequent pass at the same dims
				// (very common during interactive drag adaptation; see
				// SceneEditController::DoOneRenderPass scale ramp)
				// reactivates the parked entry instead of paying the
				// allocation + observer-registration cost.  LRU
				// eviction on overflow returns an entry to evict; we
				// tear it down OUTSIDE the lock below.
				ParkActiveAsDormant_locked( evicted );

				// Look for a dormant entry that matches the requested
				// dims.  Iterate front-to-back so a hit on the most-
				// recently-parked entry is also the cheapest.
				bool reactivated = false;
				for ( auto it = dormant_.begin(); it != dormant_.end(); ++it ) {
					if ( it->w == width && it->h == height ) {
						framestore_ = it->fs;
						framesink_  = it->sink;
						observer_   = it->obs;
						dormant_.erase( it );
						// Camera EV may have changed while this chain
						// was parked — re-apply the latest snapshot so
						// encoders / RenderToBuffer see current state.
						framestore_->MutableMeta().cameraExposureEV =
							static_cast<double>( cameraExposureEV_ );
						reactivated = true;
						break;
					}
				}

				if ( !reactivated ) {
					// Cache miss — allocate fresh chain at the new dims.
					// Logged at info level so the interactive preview-
					// scale sweep doesn't spam warnings (unlike the
					// previous unconditional reallocate-and-warn path).
					GlobalLog()->PrintEx( eLog_Info,
						"ViewportFrameStore:: allocating new FrameStore "
						"chain for %ux%u (dormant cache size %zu).",
						width, height, dormant_.size() );

					FrameStore::Spec spec;
					spec.width    = width;
					spec.height   = height;
					spec.tileEdge = 32;  // match rasterizer tile size; not load-bearing
					framestore_ = new FrameStore( spec );  // refcount = 1
					framestore_->MutableMeta().cameraExposureEV =
						static_cast<double>( cameraExposureEV_ );

					framesink_ = new FrameSink( framestore_ );

					// Build observer + register on the FrameStore.
					// `new` because BridgeObserver doesn't extend
					// Reference (it's an internal-only lifecycle
					// managed by us).  AddObserver is a brief
					// lock-and-push — safe to call under chainMutex_.
					observer_ = new BridgeObserver( *this );
					framestore_->AddObserver( observer_ );
				}
			}  // chainMutex_ released here

			// Post-lock teardown of the LRU eviction (if any).  Same
			// rationale as `BindFrameStore` Phase 3 — RemoveObserver
			// can wait for in-flight dispatches whose callbacks need
			// chainMutex_ shared_lock; calling it without the lock
			// lets those dispatches drain.  No-op on empty/default
			// DormantChain.
			TeardownDormant_unlocked( evicted );
		}

	} // namespace Implementation
} // namespace RISE
