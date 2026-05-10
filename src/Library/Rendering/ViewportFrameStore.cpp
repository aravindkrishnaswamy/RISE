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
			// Take the chain-mutex unique-lock so any reader
			// thread that's mid-RenderToBuffer / SaveAs and
			// holding only an addref'd snapshot of framestore_
			// has finished its captured-pointer dereference (the
			// readers release the mutex before doing the actual
			// work, but they do hold the snapshot's addref the
			// whole time — releasing the chain in TeardownChain
			// drops our reference, the reader's addref keeps
			// the FrameStore alive until they release).
			std::unique_lock<std::shared_mutex> lock( chainMutex_ );
			TeardownChain();
		}

		// Caller MUST hold chainMutex_ as unique_lock when calling this.
		// L5a round-8 — also tears down every dormant chain cached
		// for preview-scale oscillation reuse; only invoked from the
		// destructor (since EnsureChain now parks rather than tears
		// down on dim change).
		// L6e-2a — also handles external-bind teardown.  In external
		// mode `framestore_` is an ALIAS for `externalFrameStore_`
		// (same pointer, single addref held on `externalFrameStore_`).
		// We must release through `externalFrameStore_` exactly once
		// and clear the alias; releasing `framestore_` directly would
		// be redundant (same refcount) but indistinguishable from
		// internal-mode teardown.
		void ViewportFrameStore::TeardownChain()
		{
			if ( framestore_ && observer_ ) {
				framestore_->RemoveObserver( observer_ );
			}
			delete observer_;
			observer_ = nullptr;

			safe_release( framesink_ );
			framesink_ = nullptr;

			if ( externalFrameStore_ ) {
				// External-bound: framestore_ is an alias; the addref
				// lives on externalFrameStore_.  Clear the alias,
				// release through the canonical pointer.
				framestore_ = nullptr;
				safe_release( externalFrameStore_ );
				externalFrameStore_ = nullptr;
			} else {
				// Internal-mode: VFS owned the FrameStore via
				// `framestore_`'s addref.
				safe_release( framestore_ );
				framestore_ = nullptr;
			}

			// Drain dormant cache (only populated in internal mode;
			// external mode bypasses the cache entirely).  Each
			// entry's observer was registered on its FrameStore at
			// allocation time and kept registered through parking;
			// remove + delete before releasing the FrameStore,
			// mirroring the active chain's teardown order above.
			for ( auto& d : dormant_ ) {
				if ( d.fs && d.obs ) {
					d.fs->RemoveObserver( d.obs );
				}
				delete d.obs;
				safe_release( d.sink );
				safe_release( d.fs );
			}
			dormant_.clear();
		}

		// Caller MUST hold chainMutex_ as unique_lock when calling this.
		// Moves the current active triple onto the front of dormant_
		// (MRU position) and clears the active pointers.  If the
		// dormant cache is at capacity, evicts the LRU entry (back of
		// the vector) — its FrameStore is fully torn down (observer
		// removed + deleted, sink released, store released).
		void ViewportFrameStore::ParkActiveAsDormant_locked()
		{
			if ( !framestore_ ) {
				return;  // nothing to park
			}

			// Evict LRU first if we'd otherwise exceed the cap.  The
			// cap is `kMaxDormantChains` for parked entries
			// (active is in addition to that).
			if ( dormant_.size() >= kMaxDormantChains ) {
				DormantChain& evict = dormant_.back();
				if ( evict.fs && evict.obs ) {
					evict.fs->RemoveObserver( evict.obs );
				}
				delete evict.obs;
				safe_release( evict.sink );
				safe_release( evict.fs );
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
			std::unique_lock<std::shared_mutex> lock( chainMutex_ );

			// Idempotent — re-binding the same pointer is a no-op,
			// avoids tearing down + re-registering an observer that
			// would point at the same store.
			if ( external == externalFrameStore_ ) {
				return;
			}

			// Tear down whatever's currently active.  TeardownChain
			// is now external-aware (handles both internal-mode +
			// external-mode teardown correctly under one entry
			// point).
			TeardownChain();

			// Bind to the new external (if non-null).  `nullptr`
			// reverts to internal-managed mode — the next
			// `Output*Image` call will lazy-allocate a fresh internal
			// store via `EnsureChain`.
			if ( external ) {
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
			const FrameStoreOutput::ViewTransform& xform ) const
		{
			FrameStore* snap = SnapshotFrameStore( chainMutex_, framestore_ );
			if ( !snap ) return;
			snap->Render( dst, dstStride, roi, fmt, xform );
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
			// eviction on overflow keeps total memory bounded.
			ParkActiveAsDormant_locked();

			// Look for a dormant entry that matches the requested
			// dims.  Iterate front-to-back so a hit on the most-
			// recently-parked entry is also the cheapest.
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
					return;
				}
			}

			// Cache miss — allocate fresh chain at the new dims.
			// Logged at info level so the interactive preview-scale
			// sweep doesn't spam warnings (unlike the previous
			// unconditional reallocate-and-warn path).
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
			// `new` because BridgeObserver doesn't extend Reference
			// (it's an internal-only lifecycle managed by us).
			observer_ = new BridgeObserver( *this );
			framestore_->AddObserver( observer_ );
		}

	} // namespace Implementation
} // namespace RISE
