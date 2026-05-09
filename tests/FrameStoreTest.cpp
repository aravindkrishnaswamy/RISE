//////////////////////////////////////////////////////////////////////
//
//  FrameStoreTest.cpp - L1 regression gate for FrameStore +
//  Channel<T> + tile seqlock + Render readback.
//
//  Coverage:
//    1. Construction: width/height/tileEdge geometry; channel
//       presence per Spec.aovChannels; Beauty + Alpha always
//       present.
//    2. Channel<T>: bounds, fill, row/column access; type-safety
//       via ChannelTraits.
//    3. Tile seqlock: BeginTile/EndTile bracket pixel writes,
//       generation counter advances, observer fires once per
//       commit.
//    4. Concurrent reader / writer: multi-threaded stress test
//       proves no torn reads under contention.
//    5. Render() readback: identity ViewTransform on a known
//       beauty buffer produces expected RGBA8_sRGB bytes;
//       exposure scaling propagates; tone curve gated by
//       TargetFormatInfo.isLDRFixed.
//    6. Region readback: Render with a sub-rect produces the
//       right pixel for that rect.
//    7. Reference lifetime: addref/release behaves; safe_release
//       cleans up.
//    8. AsBeautyRasterImage shim: SetPEL / GetPEL / Clear /
//       width/height match the underlying channel.
//    9. Observer attach/detach: idempotent attach, silent detach
//       of unknown observer, observer fires per commit + per frame.
//
//////////////////////////////////////////////////////////////////////

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include "../src/Library/Rendering/FrameStore.h"
#include "../src/Library/Interfaces/IRenderObserver.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::FrameStoreOutput;
using RISE::Implementation::FrameStore;

namespace
{
	int gFailCount = 0;
	int gPassCount = 0;

	void Check( bool cond, const std::string& label )
	{
		if ( cond ) {
			++gPassCount;
		} else {
			++gFailCount;
			std::cerr << "FAIL: " << label << "\n";
		}
	}

	bool ApproxEq( double a, double b, double eps )
	{
		const double d = a - b;
		return ( d >= -eps ) && ( d <= eps );
	}

	// Test helper: builds a small store and returns the handle.
	// Per RISE convention, `new Reference` starts at refcount 1, so
	// the caller releases exactly once at the end (no extra addref).
	FrameStore* MakeStore( size_t w, size_t h, size_t tile = 8,
	                       std::vector<ChannelId> aovs = {} )
	{
		FrameStore::Spec spec;
		spec.width = w;
		spec.height = h;
		spec.tileEdge = tile;
		spec.aovChannels = std::move( aovs );
		return new FrameStore( spec );
	}

	// ─── Section 1: construction & geometry ───────────────────────
	void TestConstructionAndGeometry()
	{
		FrameStore* store = MakeStore( 64, 32, 16 );

		Check( store->Width()  == 64, "Width=64" );
		Check( store->Height() == 32, "Height=32" );
		Check( store->TileEdge() == 16, "TileEdge=16" );
		Check( store->TileCountX() == 4, "TileCountX = 64/16 = 4" );
		Check( store->TileCountY() == 2, "TileCountY = 32/16 = 2" );

		// Beauty + Alpha auto-allocated.
		Check( store->HasChannel( ChannelId::Beauty ), "Beauty auto-allocated" );
		Check( store->HasChannel( ChannelId::Alpha ),  "Alpha auto-allocated" );
		// AOVs not requested → absent.
		Check( !store->HasChannel( ChannelId::Albedo ), "Albedo not allocated" );
		Check( !store->HasChannel( ChannelId::Normal ), "Normal not allocated" );

		// Initial state: black + opaque.
		auto* beauty = store->GetChannel<ChannelId::Beauty>();
		Check( beauty != nullptr, "Beauty channel pointer non-null" );
		Check( beauty->Width() == 64 && beauty->Height() == 32, "Beauty dims" );
		Check( beauty->At( 0, 0 ).r == 0.0 && beauty->At( 0, 0 ).g == 0.0
		    && beauty->At( 0, 0 ).b == 0.0, "Initial Beauty = (0,0,0)" );

		auto* alpha = store->GetChannel<ChannelId::Alpha>();
		Check( alpha != nullptr, "Alpha channel pointer non-null" );
		Check( alpha->At( 0, 0 ) == 1.0f, "Initial Alpha = 1.0" );

		// Ragged tile geometry: 65x33 tile=16 → 5x3 tiles, last tile partially out.
		FrameStore* ragged = MakeStore( 65, 33, 16 );
		Check( ragged->TileCountX() == 5, "Ragged TileCountX" );
		Check( ragged->TileCountY() == 3, "Ragged TileCountY" );
		ragged->release();

		store->release();
	}

	// ─── Section 2: AOV channel allocation ────────────────────────
	void TestAOVChannels()
	{
		FrameStore* store = MakeStore( 16, 16, 8,
			{ ChannelId::Albedo, ChannelId::Normal, ChannelId::Depth,
			  ChannelId::ObjectId, ChannelId::PrimitiveId } );

		Check( store->HasChannel( ChannelId::Albedo ),     "Albedo opt-in allocated" );
		Check( store->HasChannel( ChannelId::Normal ),     "Normal opt-in allocated" );
		Check( store->HasChannel( ChannelId::Depth ),      "Depth opt-in allocated" );
		Check( store->HasChannel( ChannelId::ObjectId ),   "ObjectId opt-in allocated" );
		Check( store->HasChannel( ChannelId::PrimitiveId ), "PrimitiveId opt-in allocated" );

		// Compile-time typed access:
		Channel<RISEPel>*  ab = store->GetChannel<ChannelId::Albedo>();
		Channel<Vector3>*  nm = store->GetChannel<ChannelId::Normal>();
		Channel<float>*    dp = store->GetChannel<ChannelId::Depth>();
		Channel<uint32_t>* oi = store->GetChannel<ChannelId::ObjectId>();
		Channel<uint32_t>* pi = store->GetChannel<ChannelId::PrimitiveId>();

		Check( ab && ab->Width() == 16 && ab->Height() == 16, "Albedo dims" );
		Check( nm && nm->Width() == 16, "Normal dims" );
		Check( dp && dp->Width() == 16, "Depth dims" );
		Check( oi && oi->Width() == 16, "ObjectId dims" );
		Check( pi && pi->Width() == 16, "PrimitiveId dims" );

		// Channel<T>::At and Fill work.
		nm->At( 5, 7 ) = Vector3( 1.0, 0.0, 0.0 );
		Check( nm->At( 5, 7 ).x == 1.0 && nm->At( 5, 7 ).y == 0.0,
			"Channel<Vector3>::At assignment" );
		dp->Fill( 42.0f );
		Check( dp->At( 0, 0 ) == 42.0f && dp->At( 15, 15 ) == 42.0f,
			"Channel<float>::Fill" );

		store->release();
	}

	// ─── Section 3: tile seqlock + observer firing ────────────────
	struct CountingObserver : public IRenderObserver
	{
		std::atomic<int> tileCount{ 0 };
		std::atomic<int> frameCount{ 0 };
		std::atomic<uint64_t> lastGen{ 0 };

		void OnTileComplete( const Rect& /*roi*/, uint64_t generation ) override
		{
			++tileCount;
			lastGen.store( generation, std::memory_order_release );
		}
		void OnFrameComplete( unsigned /*frame*/, uint64_t generation ) override
		{
			++frameCount;
			lastGen.store( generation, std::memory_order_release );
		}
	};

	void TestSeqlockAndObserver()
	{
		FrameStore* store = MakeStore( 32, 16, 8 );

		CountingObserver obs;
		store->AddObserver( &obs );

		// Idempotent attach.
		store->AddObserver( &obs );
		Check( true, "AddObserver idempotent (no crash on duplicate)" );

		const uint64_t gen0 = store->Generation();

		// Single tile commit.
		store->BeginTile( 0, 0 );
		auto* beauty = store->GetChannel<ChannelId::Beauty>();
		beauty->At( 1, 1 ) = RISEPel( 0.5, 0.25, 0.125 );
		store->EndTile( 0, 0 );

		Check( store->Generation() > gen0, "Generation advances on EndTile" );
		Check( obs.tileCount.load() == 1, "Observer fired once on EndTile" );
		Check( obs.lastGen.load() == store->Generation(),
			"Observer received current generation" );

		// Frame complete.
		store->MarkFrameComplete( 0 );
		Check( obs.frameCount.load() == 1, "Observer fired on MarkFrameComplete" );

		// Detach + commit again → no further fires.
		store->RemoveObserver( &obs );
		store->BeginTile( 1, 0 );
		store->EndTile( 1, 0 );
		Check( obs.tileCount.load() == 1, "Observer detached: no further tile fires" );

		// Detach unknown observer is silent.
		CountingObserver other;
		store->RemoveObserver( &other );
		Check( true, "RemoveObserver(unattached) silent" );

		store->release();
	}

	// ─── Section 4: concurrent reader / writer (seqlock stress) ───
	void TestConcurrentSeqlock()
	{
		FrameStore* store = MakeStore( 32, 32, 8 );

		std::atomic<bool> stop{ false };
		std::atomic<int>  tornReads{ 0 };
		std::atomic<int>  iterations{ 0 };

		// Writer thread: bumps tile (0,0) + fills with a known pattern,
		// then commits.  Pattern: each pixel encodes the writer's
		// epoch counter as a uniform color.  Readers must see one
		// consistent epoch per tile read.
		//
		// IMPORTANT: writerGen sequences through values whose linear
		// magnitudes differ enough that quantisation through the encode
		// pipeline produces distinct R bytes per epoch.  If two
		// adjacent epochs encoded to the same byte, the torn-read
		// detector would be blind to a tear at the boundary.  See L1
		// adversarial review MED-7.  We use values 0.1, 0.2, 0.3, ...
		// (cycling), each separated by 25.5 quantised units in sRGB —
		// well above any rounding ambiguity.
		std::thread writer( [&]() {
			auto* beauty = store->GetChannel<ChannelId::Beauty>();
			int epoch = 0;
			while ( !stop.load() ) {
				epoch = ( epoch + 1 ) % 9;  // 1..9
				const double v = 0.1 * static_cast<double>( epoch );
				store->BeginTile( 0, 0 );
				for ( unsigned y = 0; y < 8; ++y ) {
					for ( unsigned x = 0; x < 8; ++x ) {
						beauty->At( x, y ) = RISEPel( v, v, v );
					}
				}
				store->EndTile( 0, 0 );
			}
		} );

		// Reader function: Render() into an RGBA8 buffer for the
		// (0,0) tile and verify all 64 pixels are byte-identical
		// across all 4 channels (R, G, B, A).  Comparing all 4
		// bytes (not just R) closes the false-negative window
		// where two epochs happened to round to the same R byte.
		auto readerBody = [&]() {
			std::vector<uint8_t> buf( 8 * 8 * 4, 0 );
			ViewTransform xf = ViewTransform::Identity();
			while ( !stop.load() ) {
				const Rect roi( 0, 0, 8, 8 );
				store->Render( buf.data(), 8 * 4, roi,
				               TargetFormat::RGBA8_sRGB, xf );

				// Compare the full 4-byte pattern of pixel 0 against
				// every other pixel.  All 64 must match — otherwise
				// the read tore between two writer epochs.
				const uint32_t pat0 =
					( static_cast<uint32_t>( buf[0] )       )
					| ( static_cast<uint32_t>( buf[1] ) << 8 )
					| ( static_cast<uint32_t>( buf[2] ) << 16 )
					| ( static_cast<uint32_t>( buf[3] ) << 24 );
				bool uniform = true;
				for ( int i = 1; i < 64; ++i ) {
					const uint32_t pat =
						( static_cast<uint32_t>( buf[ i * 4 + 0 ] )       )
						| ( static_cast<uint32_t>( buf[ i * 4 + 1 ] ) << 8 )
						| ( static_cast<uint32_t>( buf[ i * 4 + 2 ] ) << 16 )
						| ( static_cast<uint32_t>( buf[ i * 4 + 3 ] ) << 24 );
					if ( pat != pat0 ) { uniform = false; break; }
				}
				if ( !uniform ) tornReads.fetch_add( 1 );
				iterations.fetch_add( 1 );
			}
		};

		// Two reader threads to amplify contention vs the single
		// writer.  Run for 500 ms — long enough on Apple Silicon
		// (where reorderings are rare events) to accumulate
		// thousands of cross-thread interleavings.  This is still
		// far short of what TSan would catch immediately, but it's
		// the best stress signal we can get without TSan integration.
		std::thread r1( readerBody );
		std::thread r2( readerBody );

		std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
		stop.store( true );
		writer.join();
		r1.join();
		r2.join();

		std::ostringstream os;
		os << "concurrent readers (2) saw " << iterations.load()
		   << " tile reads, " << tornReads.load() << " torn";
		Check( tornReads.load() == 0, os.str() );
		Check( iterations.load() > 100, "reader threads ran enough iterations to stress" );

		store->release();
	}

	// ─── Section 4b: observer self-detach safety ──────────────────
	// Per L1 adversarial review HIGH-4: an observer that calls
	// RemoveObserver(this) inside its own callback must not deadlock
	// on the observer mutex.  The DispatchObservers snapshot pattern
	// guarantees this.
	struct SelfDetachingObserver : public IRenderObserver
	{
		FrameStore* store = nullptr;
		std::atomic<int> fires{ 0 };

		void OnTileComplete( const Rect&, uint64_t ) override
		{
			++fires;
			// Detach self while we're being dispatched — would
			// deadlock under naive "lock then iterate" pattern.
			if ( store ) store->RemoveObserver( this );
		}
	};

	void TestObserverSelfDetach()
	{
		FrameStore* store = MakeStore( 16, 16, 8 );
		SelfDetachingObserver obs;
		obs.store = store;
		store->AddObserver( &obs );

		// First commit fires + self-detaches; second commit should
		// see no fire (observer already removed).
		store->BeginTile( 0, 0 );
		store->EndTile( 0, 0 );
		Check( obs.fires.load() == 1, "self-detaching observer fires once" );

		store->BeginTile( 0, 0 );
		store->EndTile( 0, 0 );
		Check( obs.fires.load() == 1,
			"self-detaching observer does not fire after self-detach" );

		store->release();
	}

	// ─── Section 4c: cross-thread RemoveObserver waits for in-flight ─
	// Per L1 adversarial review P2: RemoveObserver must wait for
	// any in-flight dispatch whose snapshot may hold the observer
	// pointer to complete.  Otherwise a caller that calls
	// RemoveObserver-then-destroys the observer races a dispatcher
	// that's part-way through invoking observer.OnXxx().
	//
	// Test pattern:
	//   - Slow observer's callback sleeps for ~5ms.
	//   - Writer thread commits tiles in a loop, firing the
	//     dispatch.
	//   - Main thread sleeps briefly, then RemoveObserver +
	//     destroys the observer.  RemoveObserver MUST NOT return
	//     until the in-flight callback finishes — otherwise
	//     destroying the observer would UAF the in-flight callback.
	struct SlowObserver : public IRenderObserver
	{
		std::atomic<int> active{ 0 };  // mirrors "callback in flight"
		std::atomic<int> fires{ 0 };

		void OnTileComplete( const Rect&, uint64_t ) override
		{
			active.fetch_add( 1 );
			std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
			active.fetch_sub( 1 );
			fires.fetch_add( 1 );
		}
	};

	// ─── Section 4d: same-thread cascade removal must not UAF ─────
	// Per L1 adversarial review round 3 P2: observer A's callback
	// removing-and-destroying observer B (where B appears LATER in
	// the same dispatch snapshot) must not lead the dispatcher to
	// dereference B's freed pointer in the next iteration.  The
	// fix is a per-iteration recheck against observers_ before
	// invoking each snapshot entry.
	struct CascadeCounter : public IRenderObserver
	{
		std::atomic<int> fires{ 0 };
		void OnTileComplete( const Rect&, uint64_t ) override
		{
			++fires;
		}
	};

	struct CascadeRemover : public IRenderObserver
	{
		FrameStore*      store      = nullptr;
		IRenderObserver* victim     = nullptr;
		std::atomic<int> fires{ 0 };
		void OnTileComplete( const Rect&, uint64_t ) override
		{
			++fires;
			// Remove + destroy the victim DURING dispatch.  Without
			// the per-iteration recheck, the dispatch loop would
			// later dereference victim's freed pointer.
			if ( store && victim ) {
				store->RemoveObserver( victim );
				delete victim;
				victim = nullptr;
			}
		}
	};

	void TestObserverCascadeRemovalNoUAF()
	{
		FrameStore* store = MakeStore( 8, 8, 8 );

		// Heap-allocate the cascade so the remover can `delete` the
		// victim — replicating realistic same-thread destruction.
		auto* a = new CascadeRemover();
		auto* b = new CascadeCounter();   // victim
		auto* c = new CascadeCounter();

		a->store  = store;
		a->victim = b;

		// AddObserver order matters: A first, then B (which A will
		// kill mid-dispatch), then C (which must still fire).
		store->AddObserver( a );
		store->AddObserver( b );
		store->AddObserver( c );

		// Single tile commit triggers DispatchObservers with
		// snapshot = [A, B, C].  A's callback removes-and-destroys
		// B; the next iteration must skip B (now freed) and
		// proceed to C.
		store->BeginTile( 0, 0 );
		store->EndTile( 0, 0 );

		Check( a->fires.load() == 1, "A fired once" );
		Check( c->fires.load() == 1, "C still fires after B removed mid-dispatch" );
		// Reaching this assertion at all means no UAF on B's freed
		// pointer during the dispatch loop.
		Check( true, "no UAF on freed observer B" );

		store->RemoveObserver( a );
		store->RemoveObserver( c );
		delete a;
		delete c;

		store->release();
	}

	void TestObserverRemoveWaitsForInFlight()
	{
		FrameStore* store = MakeStore( 16, 16, 8 );
		SlowObserver obs;
		store->AddObserver( &obs );

		std::atomic<bool> stopWriter{ false };
		std::thread writer( [&]() {
			while ( !stopWriter.load() ) {
				store->BeginTile( 0, 0 );
				store->EndTile( 0, 0 );
			}
		} );

		// Wait for the slow observer to start at least once.
		while ( obs.active.load() == 0 ) {
			std::this_thread::yield();
		}

		// Now RemoveObserver while the slow observer is mid-callback.
		// RemoveObserver must wait until obs.active goes to 0
		// before returning, OR the in-flight callback finishes
		// strictly before RemoveObserver returns.
		store->RemoveObserver( &obs );
		const int activeAfterRemove = obs.active.load();

		// Critical assertion: when RemoveObserver returns, no
		// callback can be in flight on this observer.  Otherwise
		// the caller would be free to destroy it and create a UAF.
		Check( activeAfterRemove == 0,
			"RemoveObserver waits for in-flight callback (no UAF window)" );

		stopWriter.store( true );
		writer.join();

		store->release();
	}

	// ─── Section 5: Render readback identity & exposure ───────────
	void TestRenderReadback()
	{
		FrameStore* store = MakeStore( 4, 4, 4 );

		// Fill beauty with a known pattern: pixel (x, y) = (x*0.1, y*0.1, 0).
		auto* beauty = store->GetChannel<ChannelId::Beauty>();
		store->BeginTile( 0, 0 );
		for ( unsigned y = 0; y < 4; ++y ) {
			for ( unsigned x = 0; x < 4; ++x ) {
				beauty->At( x, y ) = RISEPel( x * 0.1, y * 0.1, 0.0 );
			}
		}
		store->EndTile( 0, 0 );

		// Render with identity transform into RGBA8_sRGB.
		std::vector<uint8_t> buf( 4 * 4 * 4, 0 );
		ViewTransform xf = ViewTransform::Identity();
		store->Render( buf.data(), 4 * 4, Rect( 0, 0, 4, 4 ),
		               TargetFormat::RGBA8_sRGB, xf );

		// Pixel (0, 0) = ROMM(0, 0, 0) → encoded sRGB(0, 0, 0).
		Check( buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 255,
			"render (0,0) = #00000000FF (black, opaque)" );

		// Pixel (3, 3) = ROMM(0.3, 0.3, 0) → after ROMM→sRGB linear ≈
		// (0.36, 0.31, 0.28).  Then sRGB transfer ≈ (0.65, 0.59, 0.57).
		// Quantised: ~166, 151, 145.  Just check it's nonzero and
		// reasonable; precise values depend on the published ROMM
		// chromatic adaptation.  IMPORTANT: snapshot the BYTE value
		// (not the pointer) — subsequent Render() calls into the
		// same buffer overwrite the pointed-to bytes.
		const uint8_t identityR = buf[ ( 3 * 4 + 3 ) * 4 + 0 ];
		const uint8_t identityA = buf[ ( 3 * 4 + 3 ) * 4 + 3 ];
		Check( identityR > 100 && identityR < 200,
			"render (3,3) R in expected range" );
		Check( identityA == 255, "render (3,3) alpha opaque" );

		// Exposure +1 EV doubles linear, then re-encodes through sRGB.
		// On (3, 3) brighter: R should be larger than identity.
		ViewTransform xfBright = ViewTransform::ForLDRDisplay( 1.0f, eDisplayTransform_None );
		store->Render( buf.data(), 4 * 4, Rect( 0, 0, 4, 4 ),
		               TargetFormat::RGBA8_sRGB, xfBright );
		const uint8_t brightR = buf[ ( 3 * 4 + 3 ) * 4 + 0 ];
		Check( brightR > identityR,
			"exposure +1 EV brightens R" );

		// Exposure -1 EV halves; should darken.
		ViewTransform xfDark = ViewTransform::ForLDRDisplay( -1.0f, eDisplayTransform_None );
		store->Render( buf.data(), 4 * 4, Rect( 0, 0, 4, 4 ),
		               TargetFormat::RGBA8_sRGB, xfDark );
		const uint8_t darkR = buf[ ( 3 * 4 + 3 ) * 4 + 0 ];
		Check( darkR < identityR,
			"exposure -1 EV darkens R" );

		store->release();
	}

	// ─── Section 6: tone curve gated by isLDRFixed ────────────────
	void TestToneCurveGating()
	{
		FrameStore* store = MakeStore( 1, 1, 8 );
		auto* beauty = store->GetChannel<ChannelId::Beauty>();

		// Use input 2.0 (1 stop over white).  ACES at this level
		// compresses meaningfully below 1.0; ACES at 10.0 saturates
		// to its natural ceiling of ~1.0, defeating the gating
		// observation since with-tone-curve and without-tone-curve
		// both round-trip to ~255.
		store->BeginTile( 0, 0 );
		beauty->At( 0, 0 ) = RISEPel( 2.0, 2.0, 2.0 );
		store->EndTile( 0, 0 );

		// LDR target WITHOUT tone curve: linear 2.0 is above sRGB
		// max so the transfer function (followed by quantisation)
		// produces saturated white (255).
		ViewTransform xfNoTone = ViewTransform::ForLDRDisplay( 0.0f, eDisplayTransform_None );
		uint8_t bufNoTone[4] = { 0 };
		store->Render( bufNoTone, 4, Rect( 0, 0, 1, 1 ),
		               TargetFormat::RGBA8_sRGB, xfNoTone );
		Check( bufNoTone[0] == 255,
			"LDR target, no tone curve: 2.0 → saturated 255" );

		// LDR target WITH ACES: ACES(2.224) ≈ 0.93, then sRGB
		// transfer ≈ 0.97, then quant → ~247.  Materially below 255.
		ViewTransform xfACES = ViewTransform::ForLDRDisplay( 0.0f, eDisplayTransform_ACES );
		uint8_t bufACES[4] = { 0 };
		store->Render( bufACES, 4, Rect( 0, 0, 1, 1 ),
		               TargetFormat::RGBA8_sRGB, xfACES );
		Check( bufACES[0] < 255 && bufACES[0] > 200,
			"LDR target, ACES tone curve: compresses 2.0 below saturated white" );
		Check( bufACES[0] < bufNoTone[0],
			"ACES output strictly less than no-tone-curve at same input" );

		// HDR float target (RGBA32F_Linear) with the SAME ACES
		// ViewTransform: tone curve is GATED OFF by
		// TargetFormatInfo.isLDRFixed = false, so the raw linear
		// value passes through (~2.0 after primaries conversion;
		// matrix coefficients on white are slightly above unity so
		// > 1 expected).
		float buf32[4] = { 0 };
		store->Render( buf32, 16, Rect( 0, 0, 1, 1 ),
		               TargetFormat::RGBA32F_Linear, xfACES );
		Check( buf32[0] > 1.5f,
			"HDR float target preserves > 1.0 even when tone curve is set in xform" );

		store->release();
	}

	// ─── Section 7: reference lifetime ────────────────────────────
	void TestReferenceLifetime()
	{
		// Per RISE convention (Reference.cpp:23) the constructor
		// initialises m_nRefcount to 1, so a freshly-allocated
		// Reference-derived object owns the first reference.
		FrameStore::Spec spec;
		spec.width = 4;
		spec.height = 4;
		FrameStore* store = new FrameStore( spec );

		Check( store->refcount() == 1, "fresh FrameStore has refcount 1 (RISE convention)" );

		store->addref();
		Check( store->refcount() == 2, "addref → refcount 2" );

		store->addref();
		Check( store->refcount() == 3, "addref → refcount 3" );

		// IMPORTANT: Reference::release() (Reference.cpp:63) returns
		// `!bDestroy` — i.e. TRUE when the object is STILL ALIVE
		// after the release, FALSE when the release destroyed it.
		// The IReference comment claims the opposite ("TRUE if the
		// object was deleted") but the implementation contradicts it
		// and `safe_release` ignores the return anyway, so callers
		// in the wild aren't affected.  This test exercises the
		// actual implementation behaviour.
		bool released = store->release();
		Check( released && store->refcount() == 2,
			"release at refcount 3 returns true (still alive), decrements to 2" );

		released = store->release();
		Check( released && store->refcount() == 1,
			"release at refcount 2 returns true (still alive), decrements to 1" );

		released = store->release();
		Check( !released, "release at refcount 1 returns false (destroyed)" );
		// store is now freed; do not access.
	}

	// ─── Section 8: AsBeautyRasterImage shim ──────────────────────
	void TestBeautyRasterImageShim()
	{
		FrameStore* store = MakeStore( 8, 4, 4 );

		// Get the IRasterImage view.
		IRasterImage& img = store->AsBeautyRasterImage();
		Check( img.GetWidth()  == 8, "shim width" );
		Check( img.GetHeight() == 4, "shim height" );

		// SetPEL through shim writes into Beauty + Alpha channels.
		RISEColor c( RISEPel( 0.5, 0.25, 0.125 ), 0.7 );
		img.SetPEL( 3, 2, c );

		auto* beauty = store->GetChannel<ChannelId::Beauty>();
		auto* alpha  = store->GetChannel<ChannelId::Alpha>();
		Check( beauty->At( 3, 2 ).r == 0.5
		    && beauty->At( 3, 2 ).g == 0.25
		    && beauty->At( 3, 2 ).b == 0.125,
			"shim SetPEL writes Beauty" );
		Check( ApproxEq( alpha->At( 3, 2 ), 0.7f, 1e-6f ),
			"shim SetPEL writes Alpha" );

		// GetPEL returns what was written.
		RISEColor back = img.GetPEL( 3, 2 );
		Check( back.base.r == 0.5 && back.base.g == 0.25
		    && ApproxEq( back.a, 0.7, 1e-6 ),
			"shim GetPEL round-trips" );

		// Clear with a specific color writes the rectangle.
		RISEColor white( RISEPel( 1.0, 1.0, 1.0 ), 1.0 );
		Rect rc( 0, 0, 2, 2 );
		img.Clear( white, &rc );
		Check( beauty->At( 0, 0 ).r == 1.0 && beauty->At( 1, 1 ).g == 1.0,
			"shim Clear with rect" );
		// Outside the rect: untouched (still default 0).  Use (5,3) to be safe.
		Check( beauty->At( 5, 3 ).r == 0.0,
			"shim Clear leaves outside-rect untouched" );

		store->release();
	}

	// ─── Section 8b: HDR archival float targets ───────────────────
	// Per L1 adversarial review P3:
	//   - RGBA32F_Linear must apply NO Sanitise step in its transfer
	//     function — negatives, NaN, and Inf must pass through so
	//     reconstruction-filter ringing and out-of-gamut samples are
	//     preserved in archival output.
	//   - RGBA32F_ROMM_Linear paired with ViewTransform::Identity()
	//     must produce a buffer that's bit-identical to the FrameStore's
	//     beauty channel (modulo float-vs-double precision) — i.e.,
	//     no primaries conversion, no transfer, no clamping.
	void TestHDRArchivalIdentity()
	{
		FrameStore* store = MakeStore( 4, 1, 8 );
		auto* beauty = store->GetChannel<ChannelId::Beauty>();

		// Set 4 pixels covering the full range of pathological float
		// inputs an archival path must preserve:
		//   pixel 0: ordinary positive HDR value
		//   pixel 1: negative R (filter ringing)
		//   pixel 2: NaN G (poison-input chaos test)
		//   pixel 3: +Inf B (saturated highlight)
		const uint64_t kPosInfBits = 0x7FF0000000000000ULL;
		const uint64_t kQNaNBits   = 0x7FF8000000000000ULL;
		double posInf, nanD;
		std::memcpy( &posInf, &kPosInfBits, sizeof(posInf) );
		std::memcpy( &nanD,   &kQNaNBits,   sizeof(nanD) );

		store->BeginTile( 0, 0 );
		beauty->At( 0, 0 ) = RISEPel( 2.5,  1.0,  0.5  );  // ordinary HDR
		beauty->At( 1, 0 ) = RISEPel( -0.3, 0.5,  0.5  );  // negative R
		beauty->At( 2, 0 ) = RISEPel( 0.5,  nanD, 0.5  );  // NaN G
		beauty->At( 3, 0 ) = RISEPel( 0.5,  0.5,  posInf ); // +Inf B
		store->EndTile( 0, 0 );

		ViewTransform xf = ViewTransform::Identity();

		// ─ Test ROMM-native archival ─
		// Identity through RGBA32F_ROMM_Linear must produce
		// EXACT bit-identical floats (modulo double→float cast).
		// Rect signature is (top, left, bottom, right) — for a
		// 4-wide × 1-tall image we want top=0, left=0, bottom=1, right=4.
		float rommBuf[ 4 * 4 ] = { 0 };
		store->Render( rommBuf, 4 * 4 * sizeof(float),
			Rect( 0, 0, 1, 4 ),
			TargetFormat::RGBA32F_ROMM_Linear, xf );

		// Pixel 0: ordinary HDR survives intact.
		Check( rommBuf[ 0 * 4 + 0 ] == 2.5f, "ROMM archival pixel 0 R = 2.5" );
		Check( rommBuf[ 0 * 4 + 1 ] == 1.0f, "ROMM archival pixel 0 G = 1.0" );
		Check( rommBuf[ 0 * 4 + 2 ] == 0.5f, "ROMM archival pixel 0 B = 0.5" );

		// Pixel 1: negative R PRESERVED (key archival invariant).
		Check( rommBuf[ 1 * 4 + 0 ] == -0.3f,
			"ROMM archival preserves negative R (no Sanitise clamp)" );

		// Pixel 2: NaN G PRESERVED.  Verify via bit pattern (under
		// -ffast-math, isnan() may constant-fold to false).
		uint32_t nanGBits;
		std::memcpy( &nanGBits, &rommBuf[ 2 * 4 + 1 ], sizeof(nanGBits) );
		const bool gIsNaN = ( ( nanGBits & 0x7F800000u ) == 0x7F800000u )
		                 && ( ( nanGBits & 0x007FFFFFu ) != 0u );
		Check( gIsNaN, "ROMM archival preserves NaN G (bit-pattern check)" );

		// Pixel 3: +Inf B PRESERVED.
		uint32_t infBBits;
		std::memcpy( &infBBits, &rommBuf[ 3 * 4 + 2 ], sizeof(infBBits) );
		const bool bIsPosInf = ( infBBits == 0x7F800000u );
		Check( bIsPosInf, "ROMM archival preserves +Inf B (bit-pattern check)" );

		// ─ Test sRGB-primaries archival (industry-default EXR) ─
		// RGBA32F_Linear converts ROMM → sRGB linear (matrix
		// multiply) but applies NO transfer-function clamp.  That
		// means: positive HDR survives intact, negatives PASS THROUGH
		// (filter ringing preserved), NaN/Inf can be perturbed by
		// the matrix (3x3 multiply) but the matrix is bounded so
		// finite inputs produce finite outputs and pathological
		// inputs propagate.
		float srgbBuf[ 4 * 4 ] = { 0 };
		store->Render( srgbBuf, 4 * 4 * sizeof(float),
			Rect( 0, 0, 1, 4 ),
			TargetFormat::RGBA32F_Linear, xf );

		// Pixel 0 ordinary positive: should be roughly 2.5 with
		// some rebalancing across channels (matrix is white-point
		// preserving but not channel-identity).  All three should
		// be positive finite.
		Check( srgbBuf[ 0 * 4 + 0 ] > 0.0f && srgbBuf[ 0 * 4 + 0 ] < 10.0f,
			"sRGB archival pixel 0 R finite positive" );

		// Pixel 1 negative R — after ROMM→sRGB matrix, the negative
		// can spread across all 3 sRGB channels but at least one
		// must be negative (the matrix doesn't zero negatives).
		// Crucially, no ZERO floor was applied.
		const bool anySRGBNegative =
			   srgbBuf[ 1 * 4 + 0 ] < 0.0f
			|| srgbBuf[ 1 * 4 + 1 ] < 0.0f
			|| srgbBuf[ 1 * 4 + 2 ] < 0.0f;
		Check( anySRGBNegative,
			"sRGB archival preserves negative through matrix (no Sanitise floor)" );

		// Alpha is 1.0 (default) for all pixels and should pass
		// through bit-identically on float targets per our gated
		// alpha-sanitise rule (skips clamp on float targets).
		Check( srgbBuf[ 0 * 4 + 3 ] == 1.0f, "alpha float passthrough" );
		Check( rommBuf[ 1 * 4 + 3 ] == 1.0f, "alpha ROMM passthrough" );

		// ─ Verify alpha out-of-range survives float archival ─
		// Set pixel 0's alpha to -0.5 (negative — premultiplied
		// reconstruction artifact) and 5.0 (over-1 HDR alpha).
		// Both should pass through bit-identically on float targets.
		store->BeginTile( 0, 0 );
		auto* alpha = store->GetChannel<ChannelId::Alpha>();
		alpha->At( 0, 0 ) = -0.5f;
		alpha->At( 1, 0 ) = 5.0f;
		store->EndTile( 0, 0 );

		store->Render( rommBuf, 4 * 4 * sizeof(float),
			Rect( 0, 0, 1, 4 ),
			TargetFormat::RGBA32F_ROMM_Linear, xf );
		Check( rommBuf[ 0 * 4 + 3 ] == -0.5f,
			"float archival preserves negative alpha (no LDR clamp)" );
		Check( rommBuf[ 1 * 4 + 3 ] == 5.0f,
			"float archival preserves alpha > 1 (no LDR clamp)" );

		// ─ Same alphas through LDR target should clamp ─
		uint8_t ldrBuf[ 4 * 4 ] = { 0 };
		store->Render( ldrBuf, 4 * 4,
			Rect( 0, 0, 1, 4 ),
			TargetFormat::RGBA8_sRGB, xf );
		Check( ldrBuf[ 0 * 4 + 3 ] == 0,
			"LDR target clamps negative alpha to 0" );
		Check( ldrBuf[ 1 * 4 + 3 ] == 255,
			"LDR target clamps alpha > 1 to 255" );

		store->release();
	}

	// ─── Section 9: CopyTileFromRasterImage ingest path ───────────
	void TestCopyTileFromRasterImage()
	{
		// Build a source IRasterImage from FrameStore "B" then copy
		// a tile of B into FrameStore "A" via the ingest path.
		FrameStore* a = MakeStore( 16, 16, 8 );
		FrameStore* b = MakeStore( 16, 16, 8 );

		// Fill B's tile (1, 0) with a known color.
		auto* bBeauty = b->GetChannel<ChannelId::Beauty>();
		auto* bAlpha  = b->GetChannel<ChannelId::Alpha>();
		for ( unsigned y = 0; y < 8; ++y ) {
			for ( unsigned x = 8; x < 16; ++x ) {
				bBeauty->At( x, y ) = RISEPel( 0.7, 0.4, 0.2 );
				bAlpha->At( x, y ) = 0.5f;
			}
		}

		// Copy B's tile (1, 0) into A's tile (1, 0).
		IRasterImage& bView = b->AsBeautyRasterImage();
		const Rect srcRect( 0, 8, 8, 16 );
		a->CopyTileFromRasterImage( 1, 0, bView, srcRect );

		auto* aBeauty = a->GetChannel<ChannelId::Beauty>();
		auto* aAlpha  = a->GetChannel<ChannelId::Alpha>();
		Check( aBeauty->At( 9, 3 ).r == 0.7 && aBeauty->At( 14, 6 ).g == 0.4,
			"CopyTileFromRasterImage copies Beauty" );
		Check( ApproxEq( aAlpha->At( 9, 3 ), 0.5f, 1e-6f ),
			"CopyTileFromRasterImage copies Alpha" );

		// Tile (0, 0) (untouched) still default.
		Check( aBeauty->At( 0, 0 ).r == 0.0,
			"CopyTileFromRasterImage doesn't touch other tiles" );

		// Generation advanced.
		Check( a->Generation() > 0, "CopyTileFromRasterImage bumps generation" );

		a->release();
		b->release();
	}
}

int main()
{
	std::cout << "FrameStoreTest L1 — buffer + tile seqlock + Render readback\n";
	std::cout << "------------------------------------------------------------\n";

	TestConstructionAndGeometry();
	TestAOVChannels();
	TestSeqlockAndObserver();
	TestObserverSelfDetach();
	TestObserverCascadeRemovalNoUAF();
	TestObserverRemoveWaitsForInFlight();
	TestConcurrentSeqlock();
	TestRenderReadback();
	TestToneCurveGating();
	TestReferenceLifetime();
	TestBeautyRasterImageShim();
	TestHDRArchivalIdentity();
	TestCopyTileFromRasterImage();

	std::cout << "------------------------------------------------------------\n";
	std::cout << "passed " << gPassCount << ", failed " << gFailCount << "\n";
	return gFailCount == 0 ? 0 : 1;
}
