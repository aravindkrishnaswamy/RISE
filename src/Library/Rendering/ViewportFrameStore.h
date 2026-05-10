//////////////////////////////////////////////////////////////////////
//
//  ViewportFrameStore.h - Platform-agnostic GUI-viewport facade
//  over a FrameStore + FrameSink + observer.
//
//  This is the L4 building block every platform GUI plugs into:
//
//    macOS SwiftUI / Windows Qt / Android Compose
//                    │
//                    │  (1) attach as IRasterizerOutput on whatever
//                    │      rasterizer is current; rasterizer feeds
//                    │      pixels into the embedded FrameStore via
//                    │      the embedded FrameSink
//                    │
//                    │  (2) on tile / frame completion callbacks,
//                    │      platform code marshals to its UI thread
//                    │      and triggers a repaint
//                    │
//                    │  (3) on UI-thread display refresh, calls
//                    │      RenderToBuffer(target_format, view_xform)
//                    │      to fill the platform-native pixel buffer
//                    │      with the latest HDR-buffer state — runs
//                    │      under FrameStore's per-tile shared_mutex
//                    │      so it's safe to call concurrent with
//                    │      writes
//                    │
//                    │  (4) on Save-As menu pick, calls SaveAs(path,
//                    │      encoder, opts) to write a file using the
//                    │      L2 IFrameEncoder pipeline
//                    │
//                    └──> ViewportFrameStore (this class)
//                          ├── FrameStore (canonical HDR buffer)
//                          ├── FrameSink (IRasterizerOutput → FrameStore)
//                          └── internal BridgeObserver (fans Mark*
//                              events out to user-supplied callbacks)
//
//  Rasterizer-swap behaviour (per design doc §7.5): the FrameStore
//  is owned by THIS class, not by any specific rasterizer.  When
//  the user changes the active rasterizer in the UI, the platform
//  code calls Detach(oldRasterizer) + Attach(newRasterizer); the
//  FrameStore + observer + tile/frame callbacks all survive
//  unchanged.  The new rasterizer's first OutputImage refills the
//  same FrameStore.  No reattachment of observers required.
//
//  Lazy allocation: the FrameStore + FrameSink + BridgeObserver
//  are constructed on the FIRST IRasterizerOutput callback, when
//  the rasterizer's image dimensions become known.  Subsequent
//  output calls reuse the chain.  Resolution changes (rare but
//  possible, e.g. camera-resolution swap mid-session) trigger a
//  reallocate-and-reattach — same pattern as
//  FileRasterizerOutput::EnsureChain (L3).
//
//  Lifetime: ViewportFrameStore inherits from Reference per the
//  RISE convention.  Platform code creates with `new`, registers
//  with `addref` (or via AddRasterizerOutput which addrefs), and
//  releases via `safe_release` when the viewport tears down.
//
//  Author: design landing L4
//  License: see LICENSE.TXT
//
//////////////////////////////////////////////////////////////////////

#ifndef VIEWPORTFRAMESTORE_H_
#define VIEWPORTFRAMESTORE_H_

#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "../Interfaces/IRasterizerOutput.h"
#include "../Utilities/Reference.h"
#include "TargetFormat.h"
#include "ViewTransform.h"

namespace RISE
{
	class IRasterizer;
	class IFrameEncoder;
	struct EncodeOpts;
	class IWriteBuffer;

	namespace Implementation
	{
		class FrameStore;
		class FrameSink;

		class ViewportFrameStore : public virtual IRasterizerOutput,
		                           public virtual Reference
		{
		public:
			//! Callback for tile-completion events.  Fires from a
			//! rasterizer worker thread; platform code is
			//! responsible for marshalling to its UI thread (Qt
			//! signals, Swift dispatch_async, Compose
			//! LaunchedEffect, etc.).  See §7.5 of the design doc.
			//!
			//! Threading: the std::function is read from the
			//! rasterizer thread on every callback fire.  Setters
			//! are NOT race-protected — set callbacks at
			//! construction time before Attach() and don't mutate
			//! mid-render.  If you need per-rasterizer-swap
			//! rebinding, marshal it through the existing callback
			//! (e.g. set a `nextCallback` field on a shared
			//! state struct that the callback reads atomically).
			using TileCompleteCallback =
				std::function<void( const Rect& roi, uint64_t generation )>;

			//! Callback for full-frame completion (also fires for
			//! pre-denoise / denoise events; see SetXxxCallback
			//! variants below for finer-grained hooks).
			using FrameCompleteCallback =
				std::function<void( unsigned int frame, uint64_t generation )>;

			ViewportFrameStore();

			// ── Callbacks ──────────────────────────────────────
			// All callback setters are NOT thread-safe — set them
			// at construction time before attaching to a rasterizer.
			// Mid-render reassignment risks data-race observation
			// (the std::function is read from the rasterizer thread).

			void SetTileCompleteCallback( TileCompleteCallback cb );
			void SetFrameCompleteCallback( FrameCompleteCallback cb );
			void SetPreDenoiseCompleteCallback( FrameCompleteCallback cb );
			void SetDenoiseCompleteCallback( FrameCompleteCallback cb );

			// ── Rasterizer attachment ─────────────────────────
			//! Add this object as an IRasterizerOutput on the given
			//! rasterizer.  The rasterizer's outputs list addrefs
			//! us.
			//!
			//! IMPORTANT: IRasterizer today only exposes
			//! AddRasterizerOutput + FreeRasterizerOutputs (the
			//! latter all-or-nothing); there is no per-output
			//! removal API.  Calling Attach(rasA), then later
			//! Attach(rasB) WITHOUT first calling
			//! `rasA->FreeRasterizerOutputs()` leaves this object
			//! in BOTH outputs lists with refcount accumulated
			//! across both — which is fine for the typical
			//! single-active-rasterizer pattern (see §7.5 of
			//! docs/FRAMESTORE_DESIGN.md), but means the platform
			//! code IS responsible for calling
			//! `oldRasterizer->FreeRasterizerOutputs()` (or
			//! letting the old rasterizer's destructor run) to
			//! avoid two rasterizers concurrently driving this
			//! VFS.  See L4 adversarial review MED-1.
			void Attach( IRasterizer* rasterizer );

			//! Inverse of Attach — but in practice a no-op due to
			//! IRasterizer's current API (no per-output removal).
			//! Provided for symmetry with Attach; the platform code
			//! must drive teardown via the rasterizer's lifetime
			//! (FreeRasterizerOutputs / dtor) to actually detach.
			//! See header doc on Attach for the full contract.
			void Detach( IRasterizer* rasterizer );

			//! L6e-2a — Bind this VFS to consume an EXTERNAL
			//! `FrameStore*` directly.  When non-null, the VFS:
			//!   * Stops allocating its own internal FrameStore in
			//!     `EnsureChain` (skips the lazy-allocate path).
			//!   * Registers its `BridgeObserver` on the external
			//!     FrameStore so tile/frame events flow to the
			//!     user-supplied callbacks.  The IRasterizerOutput
			//!     methods become no-ops — the rasterizer's per-tile
			//!     `BeginTile/EndTile` bracketing already drove the
			//!     observer chain on the same FrameStore (post-L6e-1
			//!     bracketing + L6e-1.1 RAII bulk bracket).
			//!   * Has `RenderToBuffer`, `SaveAs`, `SaveTo`,
			//!     `Generation`, `GetDimensions` all read from the
			//!     bound external FrameStore.
			//!
			//! Use case: rasterizer → FrameStore is now the canonical
			//! pixel store (post-L6c).  Pre-L6e-2 the VFS allocated a
			//! second FrameStore and `FrameSink` copied tiles across.
			//! Post-L6e-2 the VFS observes the rasterizer's FrameStore
			//! directly — eliminating the cross-store copy + the
			//! per-output FrameStore allocation.
			//!
			//! Lifetime: VFS addrefs `external` on bind and releases
			//! it on unbind / destruct.  Caller retains its own
			//! reference (typically the rasterizer's reference + the
			//! Job's reference both keep it alive across the VFS
			//! lifetime; this addref is defensive).
			//!
			//! Threading: takes `chainMutex_` unique-lock to swap
			//! the chain pointers.  Reader threads in
			//! `RenderToBuffer` / `SaveAs` / `Generation` are
			//! unaffected — they snapshot+addref under the lock and
			//! their captured snapshot stays valid even if we swap
			//! mid-render.
			//!
			//! Passing `nullptr` unbinds — VFS reverts to the legacy
			//! lazy-internal-allocate path.  The previously-bound
			//! external FrameStore is released; an internal store
			//! will be allocated on the next `Output*Image` call.
			//! Calling `BindFrameStore(nullptr)` after `BindFrameStore(extA)`
			//! does NOT preserve the dormant cache — `dormant_` is
			//! purely an internal-allocation optimisation that
			//! doesn't apply to externally-managed stores.
			//!
			//! Idempotent: re-binding the same external pointer is
			//! a no-op (avoids spurious observer remove/re-add).
			void BindFrameStore( FrameStore* external );

			//! Whether this VFS is currently bound to an external
			//! FrameStore via `BindFrameStore`.  Diagnostic — most
			//! consumers don't need to check this.
			bool IsExternallyBound() const;

			// ── State queries ─────────────────────────────────
			//! Returns the underlying FrameStore.  In INTERNAL mode
			//! (the legacy default), this is null until the first
			//! `OutputImage` call lazy-allocates the chain.  In
			//! EXTERNAL mode (post-`BindFrameStore`), this is the
			//! bound external pointer — non-null from the moment of
			//! bind.  Caller does NOT take ownership; treat as
			//! borrowed reference bounded by this object's
			//! lifetime.
			//!
			//! NOTE: this is a RAW read of the chain pointer with no
			//! lock — a concurrent resolution-change in
			//! `EnsureChain` on the rasterizer thread can invalidate
			//! the returned pointer between this call and any
			//! `Width()/Height()/GetChannel<>()` follow-up.  Use
			//! `GetDimensions()` for the common "fetch dims for a
			//! buffer-sizing decision" pattern; that accessor takes
			//! the chain mutex internally so the dims are stable.
			//! `RenderToBuffer` / `SaveAs` / `SaveTo` / `Generation`
			//! are all internally chain-mutex-safe and so they can
			//! consume `GetFrameStore()` indirectly without races —
			//! they snapshot+addref under the lock.
			FrameStore* GetFrameStore() const { return framestore_; }

			//! Returns (width, height) of the current FrameStore via
			//! the chain-mutex snapshot pattern (P2-D, L4 round-4).
			//! Returns (0, 0) if the chain hasn't been allocated yet.
			//! Use this in preference to
			//! `GetFrameStore()->Width()/Height()` when racing with
			//! a possible resolution change in the rasterizer thread.
			void GetDimensions( unsigned int& outW, unsigned int& outH ) const;

			//! Convenience: forwards to FrameStore::Generation()
			//! when the chain is allocated; returns 0 otherwise.
			//! UI repaint loops compare this to last-painted-gen
			//! to gate updates.
			uint64_t Generation() const;

			// ── Display refresh ───────────────────────────────
			//! Render the current FrameStore beauty channel into
			//! `dst` using `fmt` + `xform`.  Thread-safe at TWO
			//! levels: the FrameStore's per-tile shared_mutex
			//! protects pixel reads against concurrent rasterizer
			//! writes (L1), AND a facade-level shared_mutex
			//! addref-snapshots the FrameStore pointer before
			//! reading so a concurrent resolution-change
			//! reallocation can't invalidate the snapshot
			//! mid-render (L4 round-2 P1-2).  No-op if the
			//! FrameStore hasn't been allocated yet.
			//!
			//! `dst` is row-major in `fmt`'s pixel layout;
			//! `dstStride` is bytes per row.  `roi` may extend
			//! up to (FrameStore.Width(), FrameStore.Height()).
			void RenderToBuffer(
				void*                dst,
				size_t               dstStride,
				const Rect&          roi,
				FrameStoreOutput::TargetFormat fmt,
				const FrameStoreOutput::ViewTransform& xform ) const;

			// ── Save As ───────────────────────────────────────
			//! Encode the current FrameStore via `encoder` (typically
			//! from FrameEncoderRegistry::Get().ByFormatName(...))
			//! and write to `path` via DiskFileWriteBuffer.  Returns
			//! true on success.  No-op if the FrameStore hasn't been
			//! allocated yet (returns false).
			bool SaveAs(
				const std::string&  path,
				IFrameEncoder*      encoder,
				const EncodeOpts&   opts ) const;

			//! Variant: encode into an arbitrary IWriteBuffer (for
			//! tests, network mirrors, in-memory previews).  Same
			//! return semantics as the path-taking overload.
			bool SaveTo(
				IWriteBuffer&       dst,
				IFrameEncoder*      encoder,
				const EncodeOpts&   opts ) const;

			// ── IRasterizerOutput passthrough to FrameSink ────
			void OutputIntermediateImage(
				const IRasterImage& pImage, const Rect* pRegion ) override;
			void OutputImage(
				const IRasterImage& pImage, const Rect* pRegion,
				const unsigned int frame ) override;
			void OutputPreDenoisedImage(
				const IRasterImage& pImage, const Rect* pRegion,
				const unsigned int frame ) override;
			void OutputDenoisedImage(
				const IRasterImage& pImage, const Rect* pRegion,
				const unsigned int frame ) override;
			void SetCameraExposureCompensationEV( Scalar ev ) override;

			//! L6e-2b — Picks up the rasterizer's current FrameStore
			//! and binds to it.  Fires when the rasterizer swaps its
			//! canonical FrameStore (Job push on scene-load,
			//! resolution change, active-camera switch).  Forwards
			//! to `BindFrameStore(framestore)` — same lifecycle as
			//! a manual bind.  Passing nullptr (rasterizer cleared
			//! its FrameStore) reverts to internal-managed mode.
			void OnRasterizerFrameStoreChanged( Implementation::FrameStore* framestore ) override;

		protected:
			virtual ~ViewportFrameStore();

		private:
			//! Lazy-allocate / reallocate the FrameStore + sink +
			//! observer chain when image dimensions are first known
			//! or have changed since the last allocation.  See L3
			//! adversarial review M4 for the resolution-change
			//! rationale.
			//!
			//! L5a round-8 — the interactive viewport oscillates
			//! between a handful of preview-scale resolutions during
			//! a single drag (camera dims swap each pass, see
			//! `SceneEditController::DoOneRenderPass` adaptive
			//! scaling).  The previous behaviour
			//! (TeardownChain + fresh allocate every time the dims
			//! changed) churned the FrameStore's tile-grid
			//! allocations, observer registration, and seqlock
			//! arrays per pass — measurably visible as repeated
			//! "rasterizer image size changed" warnings during
			//! interactive editing.  EnsureChain now parks the
			//! current active chain into `dormant_` on dim change
			//! and looks up dormant entries for a matching size
			//! before allocating fresh.  The dormant cache is
			//! capped at `kMaxDormantChains` (LRU eviction) so a
			//! pathological resolution sweep can't unboundedly
			//! retain stale FrameStores.
			//!
			//! Threading: still called only from the rasterizer
			//! thread (single-active-rasterizer contract, §7.5);
			//! the dormant list is therefore only mutated under
			//! `chainMutex_` unique-lock.  Reader threads
			//! (`RenderToBuffer`, `SaveAs`, `Generation`) only ever
			//! observe the active `framestore_` snapshot — they
			//! never see the dormant entries.
			void EnsureChain( unsigned int width, unsigned int height );

			//! Tear down the active chain AND every dormant chain
			//! cached for resolution oscillation reuse (used by
			//! the destructor).  Safe to call when nothing is
			//! allocated.
			void TeardownChain();

			//! Park the current active chain into `dormant_` (with
			//! LRU eviction at `kMaxDormantChains`).  Caller must
			//! hold `chainMutex_` unique-lock.  Active pointers are
			//! cleared; caller is responsible for repopulating
			//! them with either a dormant-cache hit or a fresh
			//! allocation before returning.
			void ParkActiveAsDormant_locked();

			class BridgeObserver;

			//! Chain pointer guard.  Protects (framestore_,
			//! framesink_, observer_) against concurrent
			//! reader / writer access:
			//!   - Reader threads (UI repaints calling
			//!     RenderToBuffer / SaveAs / Generation) take a
			//!     shared_lock briefly to addref `framestore_`
			//!     into a local variable, then release the lock
			//!     and operate against the local — so a concurrent
			//!     resolution-change reallocation in the rasterizer
			//!     thread can swap the chain without invalidating
			//!     the reader's captured snapshot.
			//!   - Writer thread (EnsureChain on resolution change)
			//!     takes the unique_lock briefly to swap pointers.
			//!     Pixel writes themselves (BeginTile/EndTile/
			//!     CopyTileFromRasterImage) DON'T need this lock —
			//!     they go through the FrameStore's per-tile
			//!     shared_mutex.  This guard is only for the
			//!     facade-level pointer lifetime.
			//! See L4 round-2 review P1-2.
			mutable std::shared_mutex chainMutex_;

			FrameStore*        framestore_ = nullptr;
			FrameSink*         framesink_  = nullptr;
			BridgeObserver*    observer_   = nullptr;

			// L6e-2a — When non-null, `framestore_` ABOVE points at
			// an externally-managed FrameStore (typically the
			// rasterizer's `mFrameStore`).  In that mode:
			//   * `framesink_` is null — the IRasterizerOutput chain
			//     is the legacy copy path which is bypassed.
			//   * `observer_` is registered on the external store.
			//   * `EnsureChain` short-circuits (no lazy allocate).
			//   * `dormant_` is unused (it's an internal-allocation
			//     optimisation; external stores are managed by Job).
			//   * VFS owns one addref on the external store
			//     (defensive — the Job + rasterizer also hold refs).
			//
			// `nullptr` => internal-managed mode (the legacy path).
			// Tracked separately so we can distinguish "VFS-allocated
			// the framestore" from "Job-allocated, VFS borrows".
			FrameStore*        externalFrameStore_ = nullptr;

			// L5a round-8 — dormant-chain cache for preview-scale
			// resolution oscillation.  A `DormantChain` holds a
			// fully-built (FrameStore, FrameSink, BridgeObserver)
			// triple at a specific (width,height); on dim change
			// EnsureChain parks the current active here and either
			// reactivates a matching dormant entry or allocates a
			// new one.  The observer stays registered on its
			// FrameStore the whole time it's parked — that's safe
			// because no rasterizer is feeding the parked
			// FrameStore (the framesink is detached from the
			// active output chain), so no observer events fire on
			// dormant stores.
			//
			// Sized to cover the typical interactive preview-scale
			// sweep (1, 2, 4, 8 — four levels at heavy adaptation).
			// Past that, LRU eviction releases the least-recently-
			// active chain.  Memory budget: at 4K source res,
			// each FrameStore is ~177 MB at scale 1, ~44 at scale 2,
			// ~11 at scale 4, ~3 at scale 8 — worst-case ~235 MB
			// across active + 3 dormant.  Tunable here.
			static constexpr size_t kMaxDormantChains = 3;
			struct DormantChain {
				FrameStore*     fs   = nullptr;
				FrameSink*      sink = nullptr;
				BridgeObserver* obs  = nullptr;
				unsigned int    w    = 0;
				unsigned int    h    = 0;
			};
			std::vector<DormantChain> dormant_;

			// Stored here (not in observer) so a rebuild during
			// resolution change preserves the user's callbacks.
			TileCompleteCallback   tileCb_;
			FrameCompleteCallback  frameCb_;
			FrameCompleteCallback  preDenoiseCb_;
			FrameCompleteCallback  denoiseCb_;

			// Last-seen camera EV from SetCameraExposureCompensationEV;
			// re-applied to a freshly-allocated FrameStore so the
			// next encoder run sees the current camera state even
			// across resolution changes.
			Scalar cameraExposureEV_;
		};
	}

	using Implementation::ViewportFrameStore;
}

#endif
