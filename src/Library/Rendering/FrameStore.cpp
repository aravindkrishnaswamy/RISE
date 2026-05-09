//////////////////////////////////////////////////////////////////////
//
//  FrameStore.cpp - Canonical HDR frame buffer implementation.
//
//  See FrameStore.h for the design context, lifetime conventions,
//  and observer-attachment semantics.
//
//  Concurrency: tile-level seqlock per docs/FRAMESTORE_DESIGN.md §4.
//  Each tile carries an atomic uint64 sequence counter.  Writers
//  bump it odd→even around their write; readers retry on
//  mid-write collisions.  Writers never block readers.  No mutex
//  on the per-pixel hot path.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FrameStore.h"
#include "../Interfaces/IRenderObserver.h"
#include "../Utilities/Color/ColorUtils.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <thread>

namespace
{
	// Thread-local depth counter so RemoveObserver can detect
	// self-detach (observer calling RemoveObserver inside its
	// own callback on the same thread) and skip the wait — a
	// self-waiting thread would deadlock.  Lives in an anonymous
	// namespace at TU scope so DispatchObservers (template, in
	// FrameStore.cpp) and RemoveObserver (member, also in
	// FrameStore.cpp) share it.  See L1 adversarial review P2.
	thread_local int g_observerDispatchDepth = 0;
}

using namespace RISE;
using namespace RISE::FrameStoreOutput;

namespace RISE
{
	namespace Implementation
	{

		// ─────────────────────────────────────────────────────────────
		// BeautyRasterImageView — IRasterImage shim onto the Beauty
		// channel.  Defined here (above the FrameStore constructor)
		// so the constructor's eager `new BeautyRasterImageView(*this)`
		// sees the complete type.  Class is private and nested inside
		// FrameStore, so its declaration in the header is just a
		// forward-decl; this is the actual class body.
		// ─────────────────────────────────────────────────────────────
		class FrameStore::BeautyRasterImageView : public virtual IRasterImage,
		                                          public virtual Reference
		{
		public:
			explicit BeautyRasterImageView( FrameStore& parent )
				: parent_( parent )
			{
			}

			// Forward addref/release to the parent FrameStore so
			// any caller that addrefs the view (e.g. legacy code
			// that stores an IRasterImage* with refcount-aware
			// lifetime) effectively keeps the parent alive.  The
			// view is non-independently-owned: its lifetime is
			// tied to the FrameStore's unique_ptr<beautyView_>.
			// Without this forwarding, an external addref creates
			// a refcount-vs-unique_ptr-ownership mismatch — the
			// FrameStore destructs (via its own release reaching 0)
			// and unique_ptr deletes the view, but the external
			// holder has a dangling IRasterImage*.  Forwarding
			// makes external addref → parent stays alive → view
			// stays alive.  See L1 adversarial review HIGH-5.
			void addref() const override { parent_.addref(); }
			bool release() const override { return parent_.release(); }
			unsigned int refcount() const override { return parent_.refcount(); }

			RISEColor GetPEL( const unsigned int x, const unsigned int y ) const override
			{
				if ( !parent_.beauty_ ) return RISEColor();
				const RISEPel pel = parent_.beauty_->At( x, y );
				const Chel    a   = parent_.alpha_ ? parent_.alpha_->At( x, y ) : Chel( 1 );
				return RISEColor( pel, a );
			}

			void SetPEL( const unsigned int x, const unsigned int y,
			             const RISEColor& p ) override
			{
				if ( parent_.beauty_ ) parent_.beauty_->At( x, y ) = p.base;
				if ( parent_.alpha_  ) parent_.alpha_->At( x, y ) = p.a;
			}

			void Clear( const RISEColor& c, const Rect* rc ) override
			{
				if ( !parent_.beauty_ ) return;
				const unsigned int x0 = rc ? rc->left   : 0;
				const unsigned int y0 = rc ? rc->top    : 0;
				const unsigned int x1 = rc ? rc->right  : static_cast<unsigned int>( parent_.width_ );
				const unsigned int y1 = rc ? rc->bottom : static_cast<unsigned int>( parent_.height_ );
				for ( unsigned int y = y0; y < y1; ++y ) {
					for ( unsigned int x = x0; x < x1; ++x ) {
						parent_.beauty_->At( x, y ) = c.base;
						if ( parent_.alpha_ ) parent_.alpha_->At( x, y ) = c.a;
					}
				}
			}

			void DumpImage( IRasterImageWriter* pWriter ) const override
			{
				// Drives the writer with the same row-major pixel
				// walk pattern as RasterImage_Template::DumpImage
				// (RasterImage.h, "DumpImage" body).  Same order +
				// same per-pixel WriteColor input → byte-identical
				// output to the legacy
				// FileRasterizerOutput::WriteImageToFile pipeline,
				// which is the L2 IFrameEncoder regression gate.
				//
				// Alpha is read at full Chel (double) precision —
				// matches the legacy Color_Template<RISEPel>::a
				// type.  See L2 adversarial review HIGH-1.
				//
				// Concurrency: this method ACQUIRES every per-tile
				// shared_mutex for the duration of the dump, so
				// concurrent rasterizer-thread writes via
				// BeginTile/EndTile block until the dump completes.
				// This makes encoders (L2 IFrameEncoder, L3
				// FileEncoderObserver, L4 ViewportFrameStore::SaveAs)
				// safe to call mid-render — exactly the use case
				// the GUI's "Save As" menu needs.  The cost is a
				// brief rasterizer pause for the encode duration;
				// per-tile shared-lock acquisition itself is ~50 ns
				// per tile (cf. L4 adversarial review HIGH-1).
				//
				// We must hold ALL tile locks for the full row-major
				// walk (not lock-per-tile) because the WriteColor
				// sequence must remain row-major to preserve byte
				// identity — encoders that maintain stateful
				// compression (PNG deflate, EXR PIZ) produce
				// different bytes if the order changes.
				if ( !pWriter || !parent_.beauty_ ) return;

				const unsigned int w = static_cast<unsigned int>( parent_.width_ );
				const unsigned int h = static_cast<unsigned int>( parent_.height_ );

				// Acquire all per-tile shared_locks before the walk.
				// std::shared_lock construction blocks if a writer
				// holds the corresponding exclusive lock; once the
				// writer releases (EndTile), our acquire proceeds.
				// All locks are released on scope exit at the end
				// of this method.
				std::vector<std::shared_lock<std::shared_mutex>> tileLocks;
				const size_t tileCount =
					parent_.tileCountX_ * parent_.tileCountY_;
				tileLocks.reserve( tileCount );
				for ( size_t ty = 0; ty < parent_.tileCountY_; ++ty ) {
					for ( size_t tx = 0; tx < parent_.tileCountX_; ++tx ) {
						tileLocks.emplace_back(
							parent_.TileLockAt( tx, ty ).mtx );
					}
				}

				pWriter->BeginWrite( w, h );
				for ( unsigned int j = 0; j < h; ++j ) {
					for ( unsigned int i = 0; i < w; ++i ) {
						const RISEPel pel = parent_.beauty_->At( i, j );
						const Chel    a   = parent_.alpha_
						                    ? parent_.alpha_->At( i, j )
						                    : Chel( 1 );
						pWriter->WriteColor(
							RISEColor( pel, a ),
							i, j );
					}
				}
				pWriter->EndWrite();
			}

			void LoadImage( IRasterImageReader* /*pReader*/ ) override
			{
				assert( false && "FrameStore beauty view cannot LoadImage" );
			}

			unsigned int GetWidth()  const override
			{
				return static_cast<unsigned int>( parent_.width_ );
			}
			unsigned int GetHeight() const override
			{
				return static_cast<unsigned int>( parent_.height_ );
			}

		public:
			// Destructor is public (rather than protected per the
			// Reference convention) so the FrameStore-owning
			// std::unique_ptr can invoke it.  This view never
			// participates in IReference-style addref/release sharing
			// directly — addref/release forward to the parent — so
			// the protected-dtor "free via release()" rule doesn't
			// apply.  See L1 adversarial review HIGH-5.
			virtual ~BeautyRasterImageView() {}

		private:
			FrameStore& parent_;
		};

		// ─────────────────────────────────────────────────────────────
		// Construction / destruction
		// ─────────────────────────────────────────────────────────────

		FrameStore::FrameStore( const Spec& spec )
			: width_( spec.width )
			, height_( spec.height )
			, tileEdge_( spec.tileEdge > 0 ? spec.tileEdge : 32 )
			, tileCountX_( ( spec.width  + ( spec.tileEdge > 0 ? spec.tileEdge : 32 ) - 1 )
			               / ( spec.tileEdge > 0 ? spec.tileEdge : 32 ) )
			, tileCountY_( ( spec.height + ( spec.tileEdge > 0 ? spec.tileEdge : 32 ) - 1 )
			               / ( spec.tileEdge > 0 ? spec.tileEdge : 32 ) )
			, presence_( static_cast<size_t>( ChannelId::COUNT ), false )
			, meta_( spec.meta )
		{
			// Beauty + Alpha are always allocated.  Beauty holds the
			// RISEPel radiance; Alpha is a separate float channel.
			// Keeping Alpha as its own typed channel rather than a
			// 4th component of Beauty matches the rest of the
			// codebase (RISEColor splits .base / .a) and lets future
			// rasterizers populate Alpha independently.
			if ( width_ > 0 && height_ > 0 ) {
				beauty_ = std::make_unique<Channel<RISEPel>>( width_, height_ );
				alpha_  = std::make_unique<Channel<Chel>>(    width_, height_ );

				// Initial state: black + opaque (alpha=1).  Matches
				// the rasterizer's "begin from clear" expectation.
				beauty_->Fill( RISEPel( 0.0, 0.0, 0.0 ) );
				alpha_->Fill( Chel( 1 ) );

				presence_[ static_cast<uint32_t>( ChannelId::Beauty ) ] = true;
				presence_[ static_cast<uint32_t>( ChannelId::Alpha  ) ] = true;
			}

			// Optional AOV channels.  Each switch case sets its own
			// presence_[id] = true AFTER successful allocation, so a
			// future ChannelId added to the enum but not handled here
			// will fail the assert (debug) and leave presence_ at
			// false (release) — the safer behaviour than the previous
			// "set true unconditionally outside the switch" pattern,
			// which would tell HasChannel(NewId) "yes, exists" while
			// no storage was allocated.  See L1 adversarial review HIGH-6.
			for ( ChannelId id : spec.aovChannels ) {
				if ( id == ChannelId::Beauty || id == ChannelId::Alpha ) {
					// Already allocated; allow listing without error
					// for clients that explicitly enumerate.
					continue;
				}
				if ( width_ == 0 || height_ == 0 ) continue;

				switch ( id ) {
					case ChannelId::Albedo:
						albedo_ = std::make_unique<Channel<RISEPel>>( width_, height_ );
						albedo_->Fill( RISEPel( 0.0, 0.0, 0.0 ) );
						presence_[ static_cast<uint32_t>( ChannelId::Albedo ) ] = true;
						break;
					case ChannelId::Normal:
						normal_ = std::make_unique<Channel<Vector3>>( width_, height_ );
						normal_->Fill( Vector3( 0.0, 0.0, 0.0 ) );
						presence_[ static_cast<uint32_t>( ChannelId::Normal ) ] = true;
						break;
					case ChannelId::Depth:
						depth_ = std::make_unique<Channel<float>>( width_, height_ );
						depth_->Fill( 0.0f );
						presence_[ static_cast<uint32_t>( ChannelId::Depth ) ] = true;
						break;
					case ChannelId::ObjectId:
						objectId_ = std::make_unique<Channel<uint32_t>>( width_, height_ );
						objectId_->Fill( 0u );
						presence_[ static_cast<uint32_t>( ChannelId::ObjectId ) ] = true;
						break;
					case ChannelId::PrimitiveId:
						primitiveId_ = std::make_unique<Channel<uint32_t>>( width_, height_ );
						primitiveId_->Fill( 0u );
						presence_[ static_cast<uint32_t>( ChannelId::PrimitiveId ) ] = true;
						break;
					default:
						// Unhandled enum value — assert in debug, ignore in
						// release.  presence_ remains false so HasChannel
						// reports the truth: storage was not allocated.
						assert( false && "Unhandled AOV ChannelId in FrameStore::Spec — "
						                 "did you add an enum value without wiring "
						                 "it through the constructor switch?" );
						break;
				}
			}

			// Per-tile reader/writer locks.  std::shared_mutex is
			// non-movable so std::vector can't host TileLock
			// directly; heap-array via unique_ptr<T[]>.
			const size_t tileCount = tileCountX_ * tileCountY_;
			if ( tileCount > 0 ) {
				tileLocks_ = std::make_unique<TileLock[]>( tileCount );
			}

			// Pre-construct the IRasterImage shim view so
			// AsBeautyRasterImage() is a pure read with no
			// const_cast / lazy-init race.  See L1 adversarial
			// review LOW-6.
			beautyView_ = std::unique_ptr<BeautyRasterImageView>(
				new BeautyRasterImageView( *this ) );
		}

		FrameStore::~FrameStore()
		{
			// unique_ptr<Channel<T>> + unique_ptr<TileSeq[]> clean
			// up automatically.  observers_ is non-owning; nothing
			// to do.  beautyView_ is unique_ptr too.
		}

		// ─────────────────────────────────────────────────────────────
		// Write-side API
		// ─────────────────────────────────────────────────────────────

		void FrameStore::BeginTile( size_t tileX, size_t tileY )
		{
			// Acquire the tile's exclusive lock.  Subsequent pixel
			// writes happen under this lock; readers (Render) take
			// the shared lock and block until the writer releases.
			// This is C++-standard data-race-free, in contrast to the
			// previous atomic-seqlock which had UB on the non-atomic
			// pixel storage.  See L1 adversarial review P1.
			assert( tileX < tileCountX_ && tileY < tileCountY_ );
			TileLockAt( tileX, tileY ).mtx.lock();
		}

		void FrameStore::EndTile( size_t tileX, size_t tileY )
		{
			assert( tileX < tileCountX_ && tileY < tileCountY_ );
			TileLockAt( tileX, tileY ).mtx.unlock();

			// Bump global generation; readers gate viewport repaints
			// on this counter changing.  Release ordering pairs with
			// the acquire load on the reader side.
			const uint64_t gen = globalGeneration_.fetch_add( 1, std::memory_order_release ) + 1;

			// Compute the tile rect for the observer callback.
			const unsigned int x0 = static_cast<unsigned int>( tileX * tileEdge_ );
			const unsigned int y0 = static_cast<unsigned int>( tileY * tileEdge_ );
			const unsigned int x1 = static_cast<unsigned int>(
				std::min( ( tileX + 1 ) * tileEdge_, width_ ) );
			const unsigned int y1 = static_cast<unsigned int>(
				std::min( ( tileY + 1 ) * tileEdge_, height_ ) );
			const Rect roi( y0, x0, y1, x1 );

			// Fire OnTileComplete.  Note: callbacks run on the
			// writer's thread per the IRenderObserver contract;
			// observers must marshal to UI threads themselves.
			//
			// We snapshot the observer list under the mutex, then
			// release the mutex BEFORE invoking observers.  This
			// avoids two failure modes: (a) an observer that
			// self-detaches (calls RemoveObserver(this)) inside its
			// callback would otherwise deadlock on the non-recursive
			// mutex; (b) a slow observer would otherwise block the
			// writer for the full callback duration.  The snapshot
			// is cheap (vector of pointers) compared to even one
			// modest observer callback.  See L1 adversarial review HIGH-4.
			DispatchObservers( [&]( IRenderObserver* obs ) {
				obs->OnTileComplete( roi, gen );
			} );
		}

		void FrameStore::CopyTileFromRasterImage( size_t tileX, size_t tileY,
		                                          const IRasterImage& src,
		                                          const Rect& srcRect )
		{
			// Phase-1 ingest path: copy beauty pixels + alpha from
			// the existing rasterizer's IRasterImage into our typed
			// Beauty/Alpha channels under tile-seqlock protection.
			//
			// srcRect is the area of `src` to read (typically the
			// rasterizer's just-completed tile rect); the
			// corresponding FrameStore region is computed from
			// (tileX, tileY) + tileEdge_.

			if ( !beauty_ ) return;  // 0×0 store

			const unsigned int dstX0 = static_cast<unsigned int>( tileX * tileEdge_ );
			const unsigned int dstY0 = static_cast<unsigned int>( tileY * tileEdge_ );
			const unsigned int dstX1 = static_cast<unsigned int>(
				std::min( ( tileX + 1 ) * tileEdge_, width_ ) );
			const unsigned int dstY1 = static_cast<unsigned int>(
				std::min( ( tileY + 1 ) * tileEdge_, height_ ) );

			BeginTile( tileX, tileY );

			for ( unsigned int dy = dstY0; dy < dstY1; ++dy ) {
				const unsigned int sy = srcRect.top + ( dy - dstY0 );
				for ( unsigned int dx = dstX0; dx < dstX1; ++dx ) {
					const unsigned int sx = srcRect.left + ( dx - dstX0 );
					if ( sx >= src.GetWidth() || sy >= src.GetHeight() ) continue;

					const RISEColor pel = src.GetPEL( sx, sy );
					beauty_->At( dx, dy ) = pel.base;
					if ( alpha_ ) {
						alpha_->At( dx, dy ) = pel.a;
					}
				}
			}

			EndTile( tileX, tileY );
		}

		void FrameStore::MarkFrameComplete( unsigned frame )
		{
			meta_.frame = frame;
			const uint64_t gen = globalGeneration_.fetch_add( 1, std::memory_order_release ) + 1;

			DispatchObservers( [&]( IRenderObserver* obs ) {
				obs->OnFrameComplete( frame, gen );
			} );
		}

		void FrameStore::MarkPreDenoiseComplete( unsigned frame )
		{
			// Update meta so observers reading Meta().frame inside
			// the callback see the current frame, not whatever the
			// previous MarkFrameComplete left.  See L1 adversarial
			// review MED-4.
			meta_.frame = frame;
			const uint64_t gen = globalGeneration_.fetch_add( 1, std::memory_order_release ) + 1;

			DispatchObservers( [&]( IRenderObserver* obs ) {
				obs->OnPreDenoiseComplete( frame, gen );
			} );
		}

		void FrameStore::MarkDenoiseComplete( unsigned frame )
		{
			meta_.frame = frame;
			const uint64_t gen = globalGeneration_.fetch_add( 1, std::memory_order_release ) + 1;

			DispatchObservers( [&]( IRenderObserver* obs ) {
				obs->OnDenoiseComplete( frame, gen );
			} );
		}

		// Snapshot the observer list under the mutex, increment the
		// in-flight counter, release the mutex, invoke callbacks
		// without the mutex held, then decrement and notify any
		// thread waiting in RemoveObserver.
		//
		// IMPORTANT — per-iteration recheck against observers_:
		// the same-thread RemoveObserver path skips the wait-for-
		// in-flight (otherwise it would self-deadlock).  That means
		// observer A's callback can remove-and-destroy observer B
		// that's LATER in this snapshot.  Without a recheck before
		// invoking each entry we'd dereference B's freed pointer on
		// the next iteration.  We re-acquire the mutex briefly per
		// iteration to verify the observer is still registered;
		// remove-during-dispatch entries are silently skipped.
		// (Cross-thread removal is already handled by the wait
		// protocol in RemoveObserver, but the per-iteration check
		// is a single uniform rule that handles both.)  See L1
		// adversarial review round 3 P2.
		//
		// Cost: one mutex acquire/release per snapshot entry, which
		// is negligible compared to the cost of an observer
		// callback (encoder writes, UI signals, etc.).
		//
		// This protocol guarantees:
		//   1. No recursive lock: observers can call AddObserver /
		//      RemoveObserver inside their callbacks.
		//   2. No iterator invalidation: we iterate a stack copy.
		//   3. No UAF on freed observers: per-iteration recheck.
		//   4. Cross-thread RemoveObserver-then-destroy is safe:
		//      the wait inside RemoveObserver pairs with the in-flight
		//      counter so the caller knows no dispatch is mid-callback
		//      on the just-removed observer when RemoveObserver
		//      returns.
		// See L1 adversarial review P2 (rounds 2 and 3).
		template <typename Fn>
		void FrameStore::DispatchObservers( Fn&& fn )
		{
			std::vector<IRenderObserver*> snapshot;
			{
				std::lock_guard<std::mutex> lock( observerMutex_ );
				snapshot = observers_;
				++observerDispatchInFlight_;
			}
			++g_observerDispatchDepth;
			for ( IRenderObserver* obs : snapshot ) {
				// Recheck registration before invoking.  An earlier
				// callback in this same dispatch may have removed
				// (and possibly destroyed) `obs` since the snapshot
				// was taken.
				bool stillRegistered;
				{
					std::lock_guard<std::mutex> lock( observerMutex_ );
					stillRegistered = std::find( observers_.begin(),
					                             observers_.end(),
					                             obs ) != observers_.end();
				}
				if ( stillRegistered ) {
					fn( obs );
				}
			}
			--g_observerDispatchDepth;
			{
				std::lock_guard<std::mutex> lock( observerMutex_ );
				--observerDispatchInFlight_;
			}
			observerDispatchDone_.notify_all();
		}

		// ─────────────────────────────────────────────────────────────
		// Observer registration
		// ─────────────────────────────────────────────────────────────

		void FrameStore::AddObserver( IRenderObserver* observer )
		{
			if ( !observer ) return;
			std::lock_guard<std::mutex> lock( observerMutex_ );
			// Reject duplicates silently; matches IRasterizerOutput
			// behaviour (callers that re-register an observer on
			// scene reload don't need to first remove).
			auto it = std::find( observers_.begin(), observers_.end(), observer );
			if ( it == observers_.end() ) {
				observers_.push_back( observer );
			}
		}

		void FrameStore::RemoveObserver( IRenderObserver* observer )
		{
			if ( !observer ) return;
			std::unique_lock<std::mutex> lock( observerMutex_ );
			auto it = std::find( observers_.begin(), observers_.end(), observer );
			if ( it != observers_.end() ) {
				observers_.erase( it );
			}

			// Wait for any in-flight dispatch whose snapshot may
			// still hold the just-removed observer pointer to
			// finish — otherwise the caller could legally destroy
			// `observer` immediately after this returns, but a
			// dispatcher thread already partway through invoking
			// observer.OnXxx() would dereference freed memory.
			//
			// Self-detach (observer calls RemoveObserver from
			// inside its own callback ON THE SAME THREAD) skips
			// the wait: the dispatcher IS this thread, and waiting
			// on g_observerDispatchDepth to reach 0 would deadlock
			// because only this thread can decrement it (after
			// returning from the callback).  In that case the
			// caller is mid-callback and the observer is implicitly
			// kept alive by the call stack until the callback
			// returns; the caller's responsibility is not to
			// destroy `this` until they return.  See L1 adversarial
			// review P2.
			if ( g_observerDispatchDepth == 0 ) {
				observerDispatchDone_.wait( lock, [this]{
					return observerDispatchInFlight_ == 0;
				} );
			}
		}

		// ─────────────────────────────────────────────────────────────
		// Read-side API
		// ─────────────────────────────────────────────────────────────

		// Encode one pixel.  Inline-friendly — called per-pixel from
		// Render's inner loop.  See ApplyViewTransformLinear (L0) for
		// the shared pipeline; this function adds Stage 5 (transfer)
		// + Stage 6 (quantise into target layout).
		void FrameStore::EncodePixel(
			const RISEPel& linearROMM,
			double alpha,
			void* dst,
			TargetFormat fmt,
			const ViewTransform& xform ) const
		{
			const TargetFormatInfo& info = GetTargetFormatInfo( fmt );

			// Stages 1-4 (exposure, white balance, primaries, tone curve).
			// Tone curve runs only when target is LDR fixed.
			double r, g, b;
			ApplyViewTransformLinear( xform, info.colorSpace,
			                          info.isLDRFixed,
			                          linearROMM.r, linearROMM.g, linearROMM.b,
			                          r, g, b );

			// Stage 5: transfer function.
			r = ApplyTransfer( info.transferFn, r );
			g = ApplyTransfer( info.transferFn, g );
			b = ApplyTransfer( info.transferFn, b );

			// Alpha handling:
			//   - LDR fixed targets (8-bit / 16-bit fixed point):
			//     sanitise NaN/Inf/negative to 0 and clamp >1 to 1
			//     so the quantiser has well-defined input.
			//   - HDR float targets (RGBA32F_*, RGBA16F_*): pass
			//     alpha through bit-identically.  Archival paths
			//     (EXR / .hdr) need to preserve out-of-range alpha
			//     for round-trip fidelity with reconstruction-
			//     filter ringing or premultiplied workflows.
			//
			// NaN/Inf check uses bit-pattern memcpy because under
			// `-ffast-math` the compiler may constant-fold both
			// `std::isnan(a)` and `(a == a)` to "not NaN" via
			// type-based reasoning.  See FrameStoreColorSpace.cpp
			// Sanitise comment for the broader rationale.  L1
			// adversarial review MED-8 + P3.
			double a = alpha;
			if ( info.isLDRFixed ) {
				static_assert( sizeof(double) == 8, "double must be 64-bit IEEE 754" );
				uint64_t bits;
				std::memcpy( &bits, &a, sizeof(bits) );
				constexpr uint64_t kExpMask = 0x7FF0000000000000ULL;
				constexpr uint64_t kSignBit = 0x8000000000000000ULL;
				if ( ( bits & kExpMask ) == kExpMask ) a = 0.0;     // Inf or NaN → 0
				else if ( bits & kSignBit )            a = 0.0;     // negative → 0
				if ( a > 1.0 ) a = 1.0;
			}

			// Stage 6: pack into target pixel layout.
			if ( !info.isFloat ) {
				// 8-bit / 16-bit fixed point.  Quantise via round-to-nearest.
				if ( info.bytesPerPixel == 4 || info.bytesPerPixel == 3 ) {
					// 8-bit
					auto Q8 = []( double x ) -> uint8_t {
						double v = x * 255.0 + 0.5;
						if ( v < 0.0 )   v = 0.0;
						if ( v > 255.0 ) v = 255.0;
						return static_cast<uint8_t>( v );
					};
					const uint8_t R = Q8( r );
					const uint8_t G = Q8( g );
					const uint8_t B = Q8( b );
					const uint8_t A = Q8( a );
					uint8_t* p = static_cast<uint8_t*>( dst );
					switch ( info.channelOrder ) {
						case ChannelOrder::RGBA: p[0]=R; p[1]=G; p[2]=B; p[3]=A; break;
						case ChannelOrder::RGB:  p[0]=R; p[1]=G; p[2]=B;          break;
						case ChannelOrder::BGRA: p[0]=B; p[1]=G; p[2]=R; p[3]=A;  break;
					}
				} else {
					// 16-bit unsigned.  Today this is RGBA16_sRGB only
					// (4 channels).  Defensive assert: any future 3-channel
					// 16-bit format would silently overrun the destination
					// pixel by 2 bytes per pixel without this guard.  See
					// L1 adversarial review MED-5.
					assert( info.channelCount == 4 && info.channelOrder == ChannelOrder::RGBA
					        && "16-bit branch only supports 4-channel RGBA layouts" );
					auto Q16 = []( double x ) -> uint16_t {
						double v = x * 65535.0 + 0.5;
						if ( v < 0.0 )      v = 0.0;
						if ( v > 65535.0 )  v = 65535.0;
						return static_cast<uint16_t>( v );
					};
					const uint16_t R = Q16( r );
					const uint16_t G = Q16( g );
					const uint16_t B = Q16( b );
					const uint16_t A = Q16( a );
					uint16_t* p = static_cast<uint16_t*>( dst );
					p[0]=R; p[1]=G; p[2]=B; p[3]=A;
				}
			} else if ( info.isHalfFloat ) {
				// 16-bit float (4-channel; only RGBA16F formats exist).
				// Use the *Bits helpers from FrameStoreColorSpace so
				// the float→half conversion never goes through a
				// `float`-typed temporary.  Under -ffast-math the
				// compiler may fold operations on float-typed values
				// assuming they're finite, so we keep the entire
				// pipeline in uint32_t bit-space.  See L1 adversarial
				// review HIGH-1.
				auto DoubleToHalfBits = []( double x ) -> uint16_t {
					const float f = static_cast<float>( x );
					uint32_t fbits;
					std::memcpy( &fbits, &f, sizeof(fbits) );
					return FloatBitsToHalf( fbits );
				};
				const uint16_t R = DoubleToHalfBits( r );
				const uint16_t G = DoubleToHalfBits( g );
				const uint16_t B = DoubleToHalfBits( b );
				const uint16_t A = DoubleToHalfBits( a );
				uint16_t* p = static_cast<uint16_t*>( dst );
				p[0]=R; p[1]=G; p[2]=B; p[3]=A;
			} else {
				// 32-bit float (RGBA32F or RGB32F).
				float* p = static_cast<float*>( dst );
				p[0] = static_cast<float>( r );
				p[1] = static_cast<float>( g );
				p[2] = static_cast<float>( b );
				if ( info.hasAlpha ) {
					p[3] = static_cast<float>( a );
				}
			}
		}

		void FrameStore::Render(
			void* dst,
			size_t dstStride,
			const Rect& roi,
			TargetFormat fmt,
			const ViewTransform& xform ) const
		{
			if ( !dst || !beauty_ ) return;

			const unsigned int x0 = roi.left;
			const unsigned int y0 = roi.top;
			const unsigned int x1 = std::min( roi.right,  static_cast<unsigned int>( width_ ) );
			const unsigned int y1 = std::min( roi.bottom, static_cast<unsigned int>( height_ ) );
			if ( x1 <= x0 || y1 <= y0 ) return;

			const TargetFormatInfo& info = GetTargetFormatInfo( fmt );
			const size_t bpp = info.bytesPerPixel;

			// Iterate by tile so the seqlock works at the right
			// granularity.  For each tile, capture seq before+after,
			// retry on mismatch.  Pixels outside the tile-aligned
			// clipped region are ignored.
			const size_t tx0 = x0 / tileEdge_;
			const size_t ty0 = y0 / tileEdge_;
			const size_t tx1 = ( x1 + tileEdge_ - 1 ) / tileEdge_;
			const size_t ty1 = ( y1 + tileEdge_ - 1 ) / tileEdge_;

			for ( size_t ty = ty0; ty < ty1; ++ty ) {
				for ( size_t tx = tx0; tx < tx1; ++tx ) {
					const unsigned int tileX0 = static_cast<unsigned int>( std::max<size_t>( tx * tileEdge_, x0 ) );
					const unsigned int tileY0 = static_cast<unsigned int>( std::max<size_t>( ty * tileEdge_, y0 ) );
					const unsigned int tileX1 = static_cast<unsigned int>( std::min<size_t>( ( tx + 1 ) * tileEdge_, x1 ) );
					const unsigned int tileY1 = static_cast<unsigned int>( std::min<size_t>( ( ty + 1 ) * tileEdge_, y1 ) );

					// Take the tile's shared lock.  Multiple Render
					// readers can hold this concurrently; if a
					// writer holds the exclusive lock (BeginTile/
					// EndTile window) we block here until they
					// release.  std::shared_mutex provides the
					// happens-before edges that make pixel reads
					// data-race-free under the C++ memory model.
					// See L1 adversarial review P1.
					std::shared_lock<std::shared_mutex> readLock(
						TileLockAt( tx, ty ).mtx );

					// Copy + transform pixels in [tileX0, tileX1) × [tileY0, tileY1).
					for ( unsigned int y = tileY0; y < tileY1; ++y ) {
						const RISEPel* beautyRow = beauty_->Row( y );
						const Chel*    alphaRow  = alpha_ ? alpha_->Row( y ) : nullptr;
						uint8_t* dstRow = static_cast<uint8_t*>( dst )
						                  + static_cast<size_t>( y - y0 ) * dstStride
						                  + static_cast<size_t>( tileX0 - x0 ) * bpp;
						for ( unsigned int x = tileX0; x < tileX1; ++x ) {
							const double a = alphaRow ? alphaRow[x] : 1.0;
							EncodePixel( beautyRow[x], a, dstRow, fmt, xform );
							dstRow += bpp;
						}
					}
				}
			}
		}

		// ─────────────────────────────────────────────────────────────
		// Back-compat IRasterImage shim accessors.
		// (The class body itself lives at the top of this file so it
		// is complete before the FrameStore constructor's eager
		// `new BeautyRasterImageView(*this)` instantiation.)
		// ─────────────────────────────────────────────────────────────

		IRasterImage& FrameStore::AsBeautyRasterImage()
		{
			// View is constructed eagerly in the FrameStore
			// constructor — no lazy-init race.
			return *beautyView_;
		}

		const IRasterImage& FrameStore::AsBeautyRasterImage() const
		{
			return *beautyView_;
		}

	} // namespace Implementation
} // namespace RISE
