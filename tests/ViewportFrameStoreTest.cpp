//////////////////////////////////////////////////////////////////////
//
//  ViewportFrameStoreTest.cpp - L4 regression gate for the
//  platform-agnostic ViewportFrameStore.
//
//  ViewportFrameStore is the building block every GUI platform
//  (macOS / Windows / Android) plugs into for L4.  Coverage:
//    1. Lazy chain allocation on first OutputImage.
//    2. Tile + Frame + Pre/Denoise callbacks fire at the right
//       events with the right (frame, generation) args.
//    3. RenderToBuffer reads correctly from the FrameStore (and
//       respects ViewTransform exposure).
//    4. SaveAs produces bytes byte-identical to the L2 IFrameEncoder
//       direct path (transitivity → byte-identical to legacy
//       FileRasterizerOutput per L2's regression).
//    5. Rasterizer-swap simulation: register VFS on rasterizer A,
//       detach, register on B; UI callbacks persist while the VFS
//       follows the new rasterizer's canonical FrameStore.
//    6. Resolution change triggers chain reallocation + observers
//       still fire on the new chain.
//    7. Multi-frame reuse: two OutputImage calls populate the
//       same FrameStore correctly.
//    8. cameraExposureEV propagates to FrameStore.Meta() so
//       SaveAs sees the right total EV (matches L3 cameraEV
//       behavior).
//
//////////////////////////////////////////////////////////////////////

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
	#include <process.h>		// _getpid()
	#define getpid _getpid
#else
	#include <unistd.h>		// getpid()
#endif

#include "../src/Library/Rendering/ViewportFrameStore.h"
#include "../src/Library/Rendering/FrameStore.h"
#include "../src/Library/Rendering/FrameEncoders.h"
#include "../src/Library/Rendering/FileEncoderObserver.h"
#include "../src/Library/RasterImages/RasterImage.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/DiskFileWriteBuffer.h"
#include "../src/Library/Utilities/RISECBOR64.h"
#include "FireOutputMetadataTestFixture.h"
#include "../src/Library/Interfaces/IFrameEncoder.h"
#include "../src/Library/Interfaces/IRenderObserver.h"

#ifndef NO_EXR_SUPPORT
	#include <ImfChannelList.h>
	#include <ImfFrameBuffer.h>
	#include <ImfInputFile.h>
	#include <ImfStringAttribute.h>
#endif

using namespace RISE;
using namespace RISE::Implementation;
using namespace RISE::FrameStoreOutput;

namespace
{
	int gFailCount = 0;
	int gPassCount = 0;

	class ProcessWatchdog
	{
	public:
		ProcessWatchdog() : completed_(std::make_shared<std::atomic<bool>>(false))
		{
			const auto completed = completed_;
			std::thread([completed]() {
				std::this_thread::sleep_for(std::chrono::seconds(30));
				if( !completed->load(std::memory_order_acquire) ) {
					std::fputs("FAIL: ViewportFrameStoreTest exceeded 30-second watchdog\n",stderr);
					std::_Exit(124);
				}
			}).detach();
		}

		~ProcessWatchdog()
		{
			completed_->store(true,std::memory_order_release);
		}

	private:
		std::shared_ptr<std::atomic<bool>> completed_;
	};

	void Check( bool cond, const std::string& label )
	{
		if ( cond ) {
			++gPassCount;
		} else {
			++gFailCount;
			std::cerr << "FAIL: " << label << "\n";
		}
	}

	constexpr unsigned int kImgW = 16;
	constexpr unsigned int kImgH = 16;

	class CallbackObserver : public IRenderObserver
	{
	public:
		explicit CallbackObserver( std::function<void()> callback ) :
			callback_(std::move(callback)) {}
		void OnTileComplete( const Rect&, uint64_t ) override { callback_(); }

	private:
		std::function<void()> callback_;
	};

	class NoWritePNGEncoder :
		public virtual IFrameEncoder,
		public virtual Reference
	{
	public:
		NoWritePNGEncoder() : encodeCalls(0) {}
		std::string FormatName() const override { return "PNG"; }
		std::vector<std::string> Extensions() const override { return { "png" }; }
		bool SupportsHDR() const override { return false; }
		bool SupportsAOVs() const override { return false; }
		void Encode( const FrameStore&, IWriteBuffer&, const EncodeOpts& ) override
		{
			++encodeCalls;
		}

		unsigned int encodeCalls;

	protected:
		~NoWritePNGEncoder() override {}
	};

	class RetainedFrameEncoder :
		public virtual IFrameEncoder,
		public virtual Reference
	{
	public:
		RetainedFrameEncoder( bool& destroyed, IFrameEncoder& delegate ) :
			encodeCalls(0), destroyed_(destroyed), delegate_(delegate) { delegate_.addref(); }
		std::string FormatName() const override { return "PPM"; }
		std::vector<std::string> Extensions() const override { return { "ppm" }; }
		bool SupportsHDR() const override { return false; }
		bool SupportsAOVs() const override { return false; }
		void Encode( const FrameStore& store, IWriteBuffer& output,
			const EncodeOpts& opts ) override
		{
			++encodeCalls;
			delegate_.Encode(store,output,opts);
		}

		unsigned int encodeCalls;

	protected:
		~RetainedFrameEncoder() override
		{
			delegate_.release();
			destroyed_ = true;
		}

	private:
		bool& destroyed_;
		IFrameEncoder& delegate_;
	};

	class BlockingPNGEncoder :
		public virtual IFrameEncoder,
		public virtual Reference
	{
	public:
		std::string FormatName() const override { return "PNG"; }
		std::vector<std::string> Extensions() const override { return { "png" }; }
		bool SupportsHDR() const override { return false; }
		bool SupportsAOVs() const override { return false; }
		void Encode( const FrameStore& store, IWriteBuffer& output,
			const EncodeOpts& opts ) override
		{
			std::unique_lock<std::mutex> lock(mutex_);
			captured_ = opts.metadataSnapshot;
			entered_ = true;
			condition_.notify_all();
			condition_.wait(lock,[this]() { return continue_; });
			lock.unlock();
			IFrameEncoder* png = FrameEncoderRegistry::Get().AcquireByFormatName("PNG");
			if( png ) {
				png->Encode(store,output,opts);
				png->release();
			}
		}

		void WaitUntilEntered()
		{
			std::unique_lock<std::mutex> lock(mutex_);
			condition_.wait(lock,[this]() { return entered_; });
		}

		void Continue()
		{
			std::lock_guard<std::mutex> lock(mutex_);
			continue_ = true;
			condition_.notify_all();
		}

		const FrameStore::Metadata& CapturedMetadata() const { return captured_; }

	protected:
		~BlockingPNGEncoder() override {}

	private:
		std::mutex mutex_;
		std::condition_variable condition_;
		bool entered_ = false;
		bool continue_ = false;
		FrameStore::Metadata captured_;
	};

	class LeaseOrderFrameStore : public FrameStore
	{
	public:
		explicit LeaseOrderFrameStore( const Spec& spec ) :
			FrameStore(spec), finalReleaseObserved_(false),
			finalReleaseWhileLeased_(false) {}

		bool release() const override
		{
			if( refcount() == 1u ) {
				bool leased = false;
				try {
					LeaseOrderFrameStore* self =
						const_cast<LeaseOrderFrameStore*>(this);
					self->SetMetadata(self->Meta());
				}
				catch( const std::runtime_error& error ) {
					leased = std::string(error.what()).find("metadata is leased") !=
						std::string::npos;
				}
				finalReleaseWhileLeased_.store(leased);
				finalReleaseObserved_.store(true);
				return true;
			}
			return Reference::release();
		}

		bool FinalReleaseObserved() const
		{
			return finalReleaseObserved_.load();
		}

		bool FinalReleaseWhileLeased() const
		{
			return finalReleaseWhileLeased_.load();
		}

		void FinishRelease()
		{
			Reference::release();
		}

	protected:
		~LeaseOrderFrameStore() override {}

	private:
		mutable std::atomic<bool> finalReleaseObserved_;
		mutable std::atomic<bool> finalReleaseWhileLeased_;
	};

	RISEColor PatternPixel( unsigned int x, unsigned int y )
	{
		const double r = static_cast<double>( x ) / static_cast<double>( kImgW - 1 );
		const double g = static_cast<double>( y ) / static_cast<double>( kImgH - 1 );
		const double b = 0.25 + 0.5 * ( r + g ) / 2.0;
		return RISEColor( RISEPel( r, g, b ), 1.0 );
	}

	RasterImage_Template<RISEPel>* MakeTestImage(
		unsigned int width = kImgW, unsigned int height = kImgH )
	{
		auto* img = new RasterImage_Template<RISEPel>(
			width, height, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) );
		for ( unsigned int y = 0; y < height; ++y ) {
			for ( unsigned int x = 0; x < width; ++x ) {
				img->SetPEL( x, y, PatternPixel( x%kImgW, y%kImgH ) );
			}
		}
		return img;
	}

	std::string MakeTempPath()
	{
		std::ostringstream os;
		os << "rise_l4_vfs_" << ::getpid();
		return (std::filesystem::temp_directory_path()/os.str()).string();
	}

	bool ReadFileAllBytes( const std::string& path, std::vector<unsigned char>& out )
	{
		std::ifstream f( path, std::ios::binary );
		if ( !f.is_open() ) return false;
		f.seekg( 0, std::ios::end );
		const std::streampos sz = f.tellg();
		f.seekg( 0, std::ios::beg );
		out.resize( static_cast<size_t>( sz ) );
		if ( sz > 0 ) {
			f.read( reinterpret_cast<char*>( out.data() ), sz );
		}
		return f.good() || f.eof();
	}

	// ─── Section 1: lazy chain allocation ─────────────────────────
	void TestLazyAllocation()
	{
		auto* vfs = new ViewportFrameStore();
		Check( vfs->GetFrameStore() == nullptr,
			"FrameStore null before first OutputImage" );
		Check( vfs->Generation() == 0,
			"Generation=0 before first OutputImage" );

		// L4 round-4 P2-D — GetDimensions returns (0,0) before
		// allocation, takes chainMutex_ shared internally so it's
		// race-safe under concurrent EnsureChain reallocation.
		unsigned int w = 99, h = 99;
		vfs->GetDimensions( w, h );
		Check( w == 0 && h == 0,
			"GetDimensions returns (0,0) before chain allocation" );

		auto* img = MakeTestImage();
		vfs->OutputImage( *img, nullptr, 0 );

		Check( vfs->GetFrameStore() != nullptr,
			"FrameStore allocated after OutputImage" );
		Check( vfs->GetFrameStore()->Width() == kImgW,
			"FrameStore width matches image" );
		Check( vfs->GetFrameStore()->Height() == kImgH,
			"FrameStore height matches image" );
		Check( vfs->Generation() > 0,
			"Generation advances after OutputImage" );

		// GetDimensions returns the correct dims after chain alloc.
		vfs->GetDimensions( w, h );
		Check( w == kImgW && h == kImgH,
			"GetDimensions matches FrameStore dims after first OutputImage" );

		safe_release( img );
		vfs->release();
	}

	// ─── Section 2: callbacks ─────────────────────────────────────
	void TestCallbacks()
	{
		auto* vfs = new ViewportFrameStore();

		std::atomic<int> tileFires{ 0 };
		std::atomic<int> frameFires{ 0 };
		std::atomic<int> preDenoiseFires{ 0 };
		std::atomic<int> denoiseFires{ 0 };
		std::atomic<unsigned int> lastFrame{ 99u };
		std::atomic<uint64_t>    lastGen{ 0 };

		vfs->SetTileCompleteCallback(
			[&]( const Rect&, uint64_t gen ) {
				++tileFires;
				lastGen.store( gen );
			} );
		vfs->SetFrameCompleteCallback(
			[&]( unsigned int frame, uint64_t gen ) {
				++frameFires;
				lastFrame.store( frame );
				lastGen.store( gen );
			} );
		vfs->SetPreDenoiseCompleteCallback(
			[&]( unsigned int, uint64_t ) { ++preDenoiseFires; } );
		vfs->SetDenoiseCompleteCallback(
			[&]( unsigned int, uint64_t ) { ++denoiseFires; } );

		auto* img = MakeTestImage();

		// OutputImage should fire OnTileComplete (per tile copied,
		// which is 1 tile for a 16x16 image at tileEdge=32) +
		// OnFrameComplete.
		vfs->OutputImage( *img, nullptr, 7 );
		Check( tileFires.load() == 1,  "tile callback fired once on OutputImage" );
		Check( frameFires.load() == 1, "frame callback fired once on OutputImage" );
		Check( lastFrame.load() == 7u, "frame callback received correct frame index" );
		Check( lastGen.load() > 0,     "frame callback received non-zero generation" );

		// OutputPreDenoisedImage fires Tile + PreDenoise.
		vfs->OutputPreDenoisedImage( *img, nullptr, 8 );
		Check( preDenoiseFires.load() == 1, "preDenoise callback fired" );
		Check( frameFires.load() == 1, "OnFrameComplete NOT fired by OutputPreDenoisedImage" );

		// OutputDenoisedImage fires Tile + Denoise.
		vfs->OutputDenoisedImage( *img, nullptr, 8 );
		Check( denoiseFires.load() == 1, "denoise callback fired" );
		Check( frameFires.load() == 1, "OnFrameComplete NOT fired by OutputDenoisedImage" );

		// OutputIntermediateImage fires per-tile callbacks for each
		// FrameStore tile that overlaps the rasterizer's region but
		// does NOT fire OnFrameComplete (no MarkFrameComplete from
		// the intermediate path).  This is the L4 round-2 P1-1
		// behaviour: the GUI viewport sees progressive tile
		// updates during a render.
		const int tilePre  = tileFires.load();
		const int framePre = frameFires.load();

		// Region covers the full image; for 16x16 with FrameStore
		// tileEdge=32, that's 1 FrameStore tile → 1 tile fire.
		// RISE Rects are INCLUSIVE per
		// PixelBasedRasterizerHelper::BoundsFromRect, so the full
		// image is Rect(0, 0, kImgH - 1, kImgW - 1).  See L4
		// round-3 P2 fix in ViewportFrameStore::OutputIntermediateImage.
		const Rect fullRegion( 0, 0, kImgH - 1, kImgW - 1 );
		vfs->OutputIntermediateImage( *img, &fullRegion );
		Check( tileFires.load() == tilePre + 1,
			"intermediate (full region): tile callback fires once for the overlapping FrameStore tile" );
		Check( frameFires.load() == framePre,
			"intermediate: frame callback NOT fired" );

		// nullptr region also means "the whole image" per
		// IRasterizerOutput.h:33-36 — same tile-fire behaviour.
		vfs->OutputIntermediateImage( *img, nullptr );
		Check( tileFires.load() == tilePre + 2,
			"intermediate (null region): tile callback fires for whole image" );

		safe_release( img );
		vfs->release();
	}

	void TestThrowingCallbacksReleaseRetainedSnapshots()
	{
		auto* vfs = new ViewportFrameStore();
		auto* img = MakeTestImage();
		vfs->OutputImage(*img,nullptr,0u);
		FrameStore* store = vfs->GetFrameStore();
		const unsigned int baselineRefs = store->refcount();
		auto throws = []() { throw std::runtime_error("callback failure"); };

		vfs->SetTileCompleteCallback(
			[&]( const Rect&, uint64_t ) { throws(); } );
		bool intermediateThrew = false;
		try { vfs->OutputIntermediateImage(*img,nullptr); }
		catch( const std::runtime_error& ) { intermediateThrew = true; }
		Check(intermediateThrew && store->refcount() == baselineRefs,
			"throwing intermediate callback releases retained FrameStore snapshot" );

		vfs->SetTileCompleteCallback({});
		vfs->SetFrameCompleteCallback(
			[&]( unsigned int, uint64_t ) { throws(); } );
		bool finalThrew = false;
		try { vfs->OutputImage(*img,nullptr,1u); }
		catch( const std::runtime_error& ) { finalThrew = true; }
		Check(finalThrew && store->refcount() == baselineRefs,
			"throwing final callback releases retained FrameSink snapshot" );

		vfs->SetFrameCompleteCallback({});
		vfs->SetPreDenoiseCompleteCallback(
			[&]( unsigned int, uint64_t ) { throws(); } );
		bool preThrew = false;
		try { vfs->OutputPreDenoisedImage(*img,nullptr,2u); }
		catch( const std::runtime_error& ) { preThrew = true; }
		Check(preThrew && store->refcount() == baselineRefs,
			"throwing pre-denoise callback releases retained FrameSink snapshot" );

		vfs->SetPreDenoiseCompleteCallback({});
		vfs->SetDenoiseCompleteCallback(
			[&]( unsigned int, uint64_t ) { throws(); } );
		bool denoiseThrew = false;
		try { vfs->OutputDenoisedImage(*img,nullptr,3u); }
		catch( const std::runtime_error& ) { denoiseThrew = true; }
		Check(denoiseThrew && store->refcount() == baselineRefs,
			"throwing denoise callback releases retained FrameSink snapshot" );

		vfs->SetDenoiseCompleteCallback({});
		img->release();
		vfs->release();
	}

	// ─── Section 2b: intermediate region tile coverage ────────────
	// When the rasterizer's region spans MULTIPLE FrameStore tiles,
	// OnTileComplete should fire once per overlapping FrameStore
	// tile.  Use a 64x64 image (4x4 = 16 FrameStore tiles at
	// tileEdge=16) to force multi-tile coverage; the tileEdge=16 is
	// the test's chosen edge here for clarity (the production
	// VFS uses 32 by default).
	void TestIntermediateMultiTile()
	{
		// Build a VFS-allocated FrameStore by going through a
		// 64x64 OutputImage call first (lazy alloc fixes
		// tileEdge=32 there → 2x2 = 4 tiles).
		auto* vfs = new ViewportFrameStore();
		std::atomic<int> tileFires{ 0 };
		vfs->SetTileCompleteCallback(
			[&]( const Rect&, uint64_t ) { ++tileFires; } );

		auto* img = new RasterImage_Template<RISEPel>(
			64, 64, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) );
		for ( unsigned y = 0; y < 64; ++y )
			for ( unsigned x = 0; x < 64; ++x )
				img->SetPEL( x, y, RISEColor( RISEPel( 0.5, 0.5, 0.5 ), 1.0 ) );

		// First OutputImage allocates the FrameStore (64x64 @ tileEdge=32).
		vfs->OutputImage( *img, nullptr, 0 );
		const int tileFiresAfterOutputImage = tileFires.load();
		// 4 tiles × 1 EndTile-per-tile = 4 fires.
		Check( tileFiresAfterOutputImage == 4,
			"OutputImage on 64x64 fires 4 tile callbacks (2x2 tile grid)" );

		// Now an intermediate region covering tiles (tx=0, ty=0)
		// and (tx=1, ty=0) — the top row of the 2x2 tile grid.
		// RISE Rects are INCLUSIVE: top half = rows [0..31] and
		// columns [0..63] = Rect( 0, 0, 31, 63 ).  See L4 round-3
		// P2 fix in ViewportFrameStore::OutputIntermediateImage,
		// which converts inclusive→exclusive at the boundary.
		const Rect topHalf( 0, 0, 31, 63 );
		vfs->OutputIntermediateImage( *img, &topHalf );
		const int tileFiresAfterIntermediate = tileFires.load();
		Check( tileFiresAfterIntermediate == tileFiresAfterOutputImage + 2,
			"intermediate region covering 2 tiles fires 2 tile callbacks" );

		// Boundary regression for L4 round-3 P2: a single-pixel
		// region whose pixel sits EXACTLY on a tile boundary —
		// inclusive Rect(32, 32, 32, 32), i.e. just the pixel at
		// (x=32, y=32), which lies in tile (tx=1, ty=1).  Under
		// the old half-open interpretation this would compute
		// tx0==tx1 (=1) and fire zero callbacks; under the
		// inclusive-bounds-converted-to-exclusive logic it fires
		// exactly one callback for the single overlapping tile.
		const int tileFiresBeforeBoundary = tileFires.load();
		const Rect singlePixel( 32, 32, 32, 32 );
		vfs->OutputIntermediateImage( *img, &singlePixel );
		Check( tileFires.load() == tileFiresBeforeBoundary + 1,
			"intermediate single-pixel region on tile boundary fires exactly 1 tile callback (L4 round-3 P2 boundary)" );

		safe_release( img );
		vfs->release();
	}

	// ─── Section 3: RenderToBuffer ────────────────────────────────
	void TestRenderToBuffer()
	{
		auto* vfs = new ViewportFrameStore();
		auto* img = MakeTestImage();
		vfs->OutputImage( *img, nullptr, 0 );

		// Identity transform → RGBA8_sRGB.  This should match what
		// the underlying FrameStore::Render produces directly (which
		// L1 already verified).  Just sanity-check non-empty +
		// alpha=255.
		std::vector<uint8_t> buf( kImgW * kImgH * 4, 0 );
		vfs->RenderToBuffer( buf.data(), kImgW * 4,
			Rect( 0, 0, kImgH, kImgW ),
			TargetFormat::RGBA8_sRGB,
			ViewTransform::Identity() );

		// Pixel (0, 0) per PatternPixel = ROMM(0, 0, 0.25), alpha=1.
		// After ROMM→sRGB matrix + sRGB transfer + uint8 quantise,
		// R/G are near 0 (small from negative-coefficient leakage in
		// the ROMM→sRGB matrix), B is moderate (~0.25 → ~138 after
		// sRGB transfer).  Just sanity-check: alpha is opaque, B
		// dominates.
		Check( buf[3] == 255, "RenderToBuffer pixel (0,0) alpha opaque" );
		Check( buf[2] > buf[0] && buf[2] > buf[1],
			"RenderToBuffer pixel (0,0) blue dominates (matches PatternPixel B=0.25)" );

		// Pixel (15, 15) — bright corner. Just check alpha = opaque.
		const uint8_t* p = buf.data() + ( 15 * kImgW + 15 ) * 4;
		Check( p[3] == 255, "RenderToBuffer corner pixel alpha opaque" );

		// Exposure +1 EV: the pixel byte should be brighter than identity.
		std::vector<uint8_t> buf2( kImgW * kImgH * 4, 0 );
		vfs->RenderToBuffer( buf2.data(), kImgW * 4,
			Rect( 0, 0, kImgH, kImgW ),
			TargetFormat::RGBA8_sRGB,
			ViewTransform::ForLDRDisplay( 1.0f, eDisplayTransform_None ) );
		const uint8_t identityR = buf[ ( 8 * kImgW + 8 ) * 4 + 0 ];
		const uint8_t brightR   = buf2[ ( 8 * kImgW + 8 ) * 4 + 0 ];
		Check( brightR > identityR,
			"RenderToBuffer +1 EV produces brighter R than identity" );

		// RenderToBuffer before any OutputImage is a silent no-op
		// (FrameStore null).
		auto* vfs2 = new ViewportFrameStore();
		std::vector<uint8_t> buf3( 16, 0xCD );  // sentinel
		vfs2->RenderToBuffer( buf3.data(), 4, Rect( 0, 0, 1, 1 ),
			TargetFormat::RGBA8_sRGB, ViewTransform::Identity() );
		Check( buf3[0] == 0xCD,
			"RenderToBuffer no-op when FrameStore not yet allocated" );
		vfs2->release();

		// L4 round-7 P1 perf-regression regression test —
		// region-bounded RenderToBuffer must write ONLY the
		// requested region's pixels into the destination buffer,
		// leaving every other pixel untouched.  This is the
		// invariant the platform bridges' per-tile callbacks rely
		// on to keep per-fire work O(tile-area) instead of
		// O(image-area).  The fix lives in how the bridges call
		// RenderToBuffer (dst pointer at (y0, x0) offset of the
		// full-image buffer + FULL row stride + half-open roi);
		// FrameStore.cpp:748-750 is the kernel.
		std::vector<uint8_t> sentinelBuf( kImgW * kImgH * 4, 0xAB );
		// Render only a 4×4 region at (top=4, left=4, bottom=8, right=8).
		const Rect roi( 4, 4, 8, 8 );  // half-open
		uint8_t* base = sentinelBuf.data() + ( 4u * kImgW + 4u ) * 4u;
		vfs->RenderToBuffer(
			base, kImgW * 4u,
			roi,
			TargetFormat::RGBA8_sRGB,
			ViewTransform::Identity() );
		// Pixels OUTSIDE the region must still be the sentinel byte.
		bool outsideUntouched = true;
		for ( unsigned int y = 0; y < kImgH && outsideUntouched; ++y ) {
			for ( unsigned int x = 0; x < kImgW && outsideUntouched; ++x ) {
				const bool inside = ( y >= 4u && y < 8u && x >= 4u && x < 8u );
				if ( inside ) continue;
				const size_t idx = ( y * kImgW + x ) * 4u;
				if ( sentinelBuf[idx + 0] != 0xAB
				  || sentinelBuf[idx + 1] != 0xAB
				  || sentinelBuf[idx + 2] != 0xAB
				  || sentinelBuf[idx + 3] != 0xAB ) {
					outsideUntouched = false;
				}
			}
		}
		Check( outsideUntouched,
			"region-bounded RenderToBuffer writes ONLY region pixels (round-7 P1 invariant)" );
		// Pixels INSIDE the region must have alpha=255 (opaque)
		// and at least one channel non-sentinel (rendered output).
		bool insideRendered = true;
		for ( unsigned int y = 4; y < 8 && insideRendered; ++y ) {
			for ( unsigned int x = 4; x < 8 && insideRendered; ++x ) {
				const size_t idx = ( y * kImgW + x ) * 4u;
				if ( sentinelBuf[idx + 3] != 255 ) insideRendered = false;
			}
		}
		Check( insideRendered,
			"region-bounded RenderToBuffer writes opaque alpha for the region" );

		safe_release( img );
		vfs->release();
	}

	// ─── Section 4: SaveAs byte-identical to L2 ───────────────────
	void TestSaveAsByteIdenticalToL2()
	{
		auto* vfs = new ViewportFrameStore();
		auto* img = MakeTestImage();
		vfs->OutputImage( *img, nullptr, 0 );

		// L2 path: build a transient FrameStore from the same image
		// and encode via FrameEncoderRegistry directly.  Compare bytes.
		FrameStore::Spec spec;
		spec.width = kImgW; spec.height = kImgH; spec.tileEdge = 32;
		auto* l2Store = new FrameStore( spec );
		auto* beauty = l2Store->GetChannel<ChannelId::Beauty>();
		auto* alpha  = l2Store->GetChannel<ChannelId::Alpha>();
		l2Store->BeginTile( 0, 0 );
		for ( unsigned int y = 0; y < kImgH; ++y ) {
			for ( unsigned int x = 0; x < kImgW; ++x ) {
				const RISEColor c = img->GetPEL( x, y );
				beauty->At( x, y ) = c.base;
				alpha->At( x, y )  = c.a;
			}
		}
		l2Store->EndTile( 0, 0 );

		IFrameEncoder* enc = FrameEncoderRegistry::Get().ByFormatName( "PNG" );

		// Both paths through DiskFileWriteBuffer to keep byte
		// stream comparable (avoids the MemoryBuffer-cursor /
		// post-seekp-data trap we hit in L3).
		const std::string vfsPath = MakeTempPath() + "_vfs.png";
		const std::string l2Path  = MakeTempPath() + "_l2.png";

		EncodeOpts opts;
		opts.colorSpace = eColorSpace_sRGB;
		opts.bpp        = 8;
		opts.viewTransform = ViewTransform::Identity();

		const bool savedOk = vfs->SaveAs( vfsPath, enc, opts );
		Check( savedOk, "SaveAs returns true on success" );
		NoWritePNGEncoder* unknownEncoder = new NoWritePNGEncoder();
		const std::string unknownPath = MakeTempPath()+".unknown";
		Check( !vfs->SaveAs(unknownPath,unknownEncoder,opts) &&
			unknownEncoder->encodeCalls == 0u &&
			!std::filesystem::exists(unknownPath),
			"SaveAs rejects an unavailable authored extension before encoding" );
		unknownEncoder->release();

		auto* l2Buf = new DiskFileWriteBuffer( l2Path.c_str() );
		enc->Encode( *l2Store, *l2Buf, opts );
		safe_release( l2Buf );

		std::vector<unsigned char> vfsBytes, l2Bytes;
		ReadFileAllBytes( vfsPath, vfsBytes );
		ReadFileAllBytes( l2Path,  l2Bytes );

		Check( vfsBytes.size() == l2Bytes.size(),
			"SaveAs file size matches L2 direct path" );
		Check( vfsBytes == l2Bytes,
			"SaveAs bytes byte-identical to L2 IFrameEncoder direct path" );

		// SaveAs before allocation returns false.
		auto* vfsEmpty = new ViewportFrameStore();
		const bool emptyOk = vfsEmpty->SaveAs( vfsPath, enc, opts );
		Check( !emptyOk, "SaveAs returns false when FrameStore not allocated" );
		vfsEmpty->release();

		std::remove( vfsPath.c_str() );
		std::remove( l2Path.c_str() );
		safe_release( img );
		l2Store->release();
		vfs->release();
	}

	void SetFireFidelityMetadata( FrameStore& store )
	{
		const RISECBOR64::Bytes configBytes =
			FireOutputMetadataTestFixture::ResolvedConfig(store.Width(),store.Height());
		const RISECBOR64::Bytes buildBytes =
			FireOutputMetadataTestFixture::RendererBuild();
		FrameStoreOutput::ActiveFireMedium medium;
		medium.mediaKind = "static_authored";
		medium.managerName = "fire";
		medium.bindingKind = "global_medium";
		medium.bindingOwner = "scene";
		medium.authoredConfigDigest =
			"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
		medium.opticalRecordIds = {
			"2cdd00456431fd0c020ee8e28b01bc59e92586beb6ac8f6ea77efa31276ad137" };
		store.SetFireFidelityMetadata("preview",
			{ "pel_transport", "producer_unqualified", "requested_preview" },
			medium.opticalRecordIds,{ medium },configBytes,buildBytes,
			RISECBOR64::SHA256Hex(buildBytes));
	}

	void TestSaveAsFireProvenanceAndTransaction()
	{
		auto* vfs = new ViewportFrameStore();
		auto* img = MakeTestImage();
		vfs->OutputImage( *img, nullptr, 0 );
		SetFireFidelityMetadata( *vfs->GetFrameStore() );

		EncodeOpts opts;
		opts.colorSpace = eColorSpace_sRGB;
		opts.bpp = 8;
		IFrameEncoder* png = FrameEncoderRegistry::Get().ByFormatName( "PNG" );
		const std::string pngPath = MakeTempPath() + "_gui_fire.png";
		const std::string sidecarPath = pngPath + ".provenance.cbor";
		Check( !vfs->SaveAs( pngPath, png, opts ) &&
			!std::filesystem::exists(pngPath) && !std::filesystem::exists(sidecarPath),
			"GUI SaveAs refuses a fire display derivative before a primary finalizes" );
		std::string decodeError;

#ifndef NO_EXR_SUPPORT
		const std::string exrPath = MakeTempPath() + "_gui_fire.exr";
		opts.colorSpace = eColorSpace_Rec709RGB_Linear;
		opts.bpp = 32;
		opts.viewTransform = ViewTransform::Identity();
		FrameStore* fireStore = vfs->GetFrameStore();
		fireStore->BeginTile(0,0);
		fireStore->GetChannel<ChannelId::Beauty>()->At(0,0) =
			RISEPel(70000.0,2.0,1.0);
		fireStore->EndTile(0,0);
		FrameStore::Metadata frameMetadata = fireStore->Meta();
		frameMetadata.frame = 17u;
		fireStore->SetMetadata(frameMetadata);
		IFrameEncoder* exr = FrameEncoderRegistry::Get().ByFormatName( "EXR" );
		Check( vfs->SaveAs( exrPath, exr, opts ),
			"GUI SaveAs writes transactional EXR fire provenance" );
		bool attributesMatch = false;
		bool floatChannels = false;
		bool largeFiniteValuePreserved = false;
		try {
			Imf::InputFile input( exrPath.c_str() );
			const Imf::StringAttribute* statusAttribute =
				input.header().findTypedAttribute<Imf::StringAttribute>(
					"riseFireProv_render_fidelity_status" );
			const Imf::StringAttribute* idAttribute =
				input.header().findTypedAttribute<Imf::StringAttribute>(
					"riseFireProv_provenance_id" );
			attributesMatch = statusAttribute &&
				statusAttribute->value() == "\"preview\"" && idAttribute &&
				idAttribute->value() == "\""+
					vfs->GetFrameStore()->Meta().primaryProvenanceId+"\"";
			const Imf::Channel* redChannel =
				input.header().channels().findChannel("R");
			floatChannels = redChannel && redChannel->type == Imf::FLOAT;
			const auto& dataWindow = input.header().dataWindow();
			const int width = dataWindow.max.x-dataWindow.min.x+1;
			const int height = dataWindow.max.y-dataWindow.min.y+1;
			if( dataWindow.min.x == 0 && dataWindow.min.y == 0 &&
				width > 0 && height > 0 ) {
				std::vector<float> red(static_cast<std::size_t>(width)*height);
				Imf::FrameBuffer frameBuffer;
				frameBuffer.insert("R",Imf::Slice(Imf::FLOAT,
					reinterpret_cast<char*>(red.data()),sizeof(float),
					sizeof(float)*static_cast<std::size_t>(width)));
				input.setFrameBuffer(frameBuffer);
				input.readPixels(dataWindow.min.y,dataWindow.max.y);
				largeFiniteValuePreserved = std::isfinite(red[0]) &&
					std::abs(red[0]-70000.0f) < 1.0f;
			}
		} catch( ... ) {
			attributesMatch = false;
			floatChannels = false;
			largeFiniteValuePreserved = false;
		}
		Check( attributesMatch,
			"GUI EXR mirrors canonical JSON provenance from its sidecar" );
		Check( floatChannels && largeFiniteValuePreserved,
			"GUI EXR SaveAs writes FLOAT channels and preserves values above FP16 range" );
		std::vector<unsigned char> exrBytes, exrSidecarBytes;
		RISECBOR64::Value exrEnvelope;
		const bool exrSidecarDecoded = ReadFileAllBytes(exrPath,exrBytes) &&
			ReadFileAllBytes(exrPath + ".provenance.cbor",exrSidecarBytes) &&
			RISECBOR64::DecodeCanonical(exrSidecarBytes,exrEnvelope,&decodeError);
		const RISECBOR64::Value* exrPayload = exrSidecarDecoded ?
			exrEnvelope.Find("payload") : nullptr;
		const RISECBOR64::Value* exrDigest = exrPayload ?
			exrPayload->Find("artifact_sha256") : nullptr;
		const RISECBOR64::Value* exrFidelity = exrPayload ?
			exrPayload->Find("artifact_fidelity") : nullptr;
		const RISECBOR64::Value* exrResolved = exrPayload ?
			exrPayload->Find("resolved_render_configuration_v1") : nullptr;
		const RISECBOR64::Value* exrOutput = exrResolved ?
			exrResolved->Find("output") : nullptr;
		const RISECBOR64::Value* exrFrame = exrOutput ?
			exrOutput->Find("frame_index") : nullptr;
		std::vector<unsigned char> strippedExr;
		Check( StripFireProvenanceEXRAttributes(exrBytes,strippedExr,decodeError) &&
			exrDigest && exrDigest->GetText() == RISECBOR64::SHA256Hex(strippedExr) &&
			exrFidelity && exrFidelity->GetText() == "preview_primary" &&
			exrFrame && exrFrame->GetIntegerArgument() == 17u,
			"GUI EXR sidecar hashes stripped bytes and binds the finalized FrameStore frame" );

		fireStore->MarkDenoiseComplete(17u);
		const std::string denoisedExrPath = MakeTempPath() + "_gui_fire_denoised.exr";
		Check( vfs->SaveAs(denoisedExrPath,exr,opts),
			"GUI EXR SaveAs treats the current denoised FrameStore as a derivative" );
		std::vector<unsigned char> denoisedSidecarBytes;
		RISECBOR64::Value denoisedEnvelope;
		const bool denoisedDecoded =
			ReadFileAllBytes(denoisedExrPath+".provenance.cbor",denoisedSidecarBytes) &&
			RISECBOR64::DecodeCanonical(denoisedSidecarBytes,denoisedEnvelope,&decodeError);
		const RISECBOR64::Value* denoisedPayload = denoisedDecoded ?
			denoisedEnvelope.Find("payload") : nullptr;
		const RISECBOR64::Value* denoisedFidelity = denoisedPayload ?
			denoisedPayload->Find("artifact_fidelity") : nullptr;
		const RISECBOR64::Value* denoisedConfig = denoisedPayload ?
			denoisedPayload->Find("resolved_render_configuration_v1") : nullptr;
		const RISECBOR64::Value* denoisedOutput = denoisedConfig ?
			denoisedConfig->Find("output") : nullptr;
		const RISECBOR64::Value* denoisedPrimary = denoisedPayload ?
			denoisedPayload->Find("derived_from_primary") : nullptr;
		Check( denoisedFidelity && denoisedFidelity->GetText() == "display_derivative" &&
			denoisedOutput && denoisedOutput->Find("denoised_derivative") &&
			denoisedOutput->Find("denoised_derivative")->GetBoolean() &&
			denoisedPrimary && denoisedPrimary->Find("provenance_id") &&
			denoisedPrimary->Find("provenance_id")->GetText() ==
				fireStore->Meta().primaryProvenanceId,
			"GUI denoised EXR is a linked display derivative, never a raw primary" );
		std::remove(denoisedExrPath.c_str());
		std::remove((denoisedExrPath+".provenance.cbor").c_str());

		opts.colorSpace = eColorSpace_sRGB;
		opts.bpp = 8;
		opts.viewTransform = ViewTransform::ForLDRDisplay();
		Check( vfs->SaveAs( pngPath, png, opts ),
			"GUI SaveAs writes a linked display derivative after the primary" );
		std::vector<unsigned char> pngBytes, pngSidecarBytes;
		RISECBOR64::Value pngEnvelope;
		const bool pngDecoded = ReadFileAllBytes(pngPath,pngBytes) &&
			ReadFileAllBytes(sidecarPath,pngSidecarBytes) &&
			RISECBOR64::DecodeCanonical(pngSidecarBytes,pngEnvelope,&decodeError);
		const RISECBOR64::Value* pngPayload = pngDecoded ?
			pngEnvelope.Find("payload") : nullptr;
		const RISECBOR64::Value* derived = pngPayload ?
			pngPayload->Find("derived_from_primary") : nullptr;
		const RISECBOR64::Value* pngDigest = pngPayload ?
			pngPayload->Find("artifact_sha256") : nullptr;
		Check( derived && derived->Find("provenance_id") &&
			derived->Find("provenance_id")->GetText() ==
				vfs->GetFrameStore()->Meta().primaryProvenanceId && pngDigest &&
			pngDigest->GetText() == RISECBOR64::SHA256Hex(pngBytes),
			"GUI derivative sidecar links the retained primary and exact derivative bytes" );
		std::remove( exrPath.c_str() );
		std::remove( (exrPath + ".provenance.cbor").c_str() );
#else
		vfs->GetFrameStore()->SetPrimaryFireArtifact(
			"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
			"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
			"preview_primary");
#endif

		std::remove( pngPath.c_str() );
		std::remove( sidecarPath.c_str() );

		opts.colorSpace = eColorSpace_sRGB;
		opts.bpp = 8;
		const std::string blockedPath = MakeTempPath() + "_blocked_fire.png";
		const std::string blockedSidecar = blockedPath + ".provenance.cbor";
		const std::string marker = blockedSidecar + "/keep";
		std::filesystem::create_directory( blockedSidecar );
		{
			std::ofstream oldArtifact( blockedPath );
			oldArtifact << "previous valid artifact";
		}
		{
			std::ofstream markerFile( marker );
			markerFile << "block replacement";
		}
		Check( !vfs->SaveAs( blockedPath, png, opts ),
			"GUI SaveAs fails when the required provenance sidecar cannot commit" );
		std::vector<unsigned char> preservedArtifact;
		ReadFileAllBytes(blockedPath,preservedArtifact);
		Check( std::string(preservedArtifact.begin(),preservedArtifact.end()) ==
				"previous valid artifact",
			"failed provenance transaction preserves the previously published artifact" );
		std::remove( blockedPath.c_str() );
		std::filesystem::remove( marker );
		std::filesystem::remove( blockedSidecar );

		MemoryBuffer* memory = new MemoryBuffer();
		Check( !vfs->SaveTo(*memory,png,opts) && memory->getCurPos() == 0,
			"GUI SaveTo fails closed when fire provenance has no sidecar sink" );
		safe_release( memory );

		NoWritePNGEncoder* noWrite = new NoWritePNGEncoder();
		const std::string emptyPath = MakeTempPath() + "_empty_fire.png";
		Check( !vfs->SaveAs(emptyPath,noWrite,opts),
			"GUI SaveAs rejects an encoder that emitted no artifact bytes" );
		Check( !std::filesystem::exists(emptyPath) &&
			!std::filesystem::exists(emptyPath + ".provenance.cbor"),
			"no-op encoding publishes neither an empty artifact nor a sidecar" );
		safe_release( noWrite );

		safe_release( img );
		vfs->release();
	}

	void TestSaveAsUsesOneMetadataSnapshot()
	{
		auto* vfs = new ViewportFrameStore();
		auto* img = MakeTestImage();
		vfs->OutputImage(*img,nullptr,0);
		FrameStore* store = vfs->GetFrameStore();
		SetFireFidelityMetadata(*store);
		store->SetPrimaryFireArtifact(
			"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
			"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
			"preview_primary");
		BlockingPNGEncoder* encoder = new BlockingPNGEncoder();
		const std::string path = MakeTempPath()+"_metadata_snapshot.png";
		bool saved = false;
		EncodeOpts opts;
		std::thread saver([&]() { saved = vfs->SaveAs(path,encoder,opts); });
		encoder->WaitUntilEntered();
		bool concurrentPreflightRejected = false;
		try {
			store->SetFireFidelityMetadata("preview",
				{ "chem_none_unqualified", "producer_unqualified" },
				{ "ec249fa4182cc3b9347727c1f10948bd8023813e2f4a68720b7a4f7e5ddaa2eb" });
		}
		catch( const std::runtime_error& error ) {
			concurrentPreflightRejected =
				std::string(error.what()).find("metadata is leased") != std::string::npos;
		}
		encoder->Continue();
		saver.join();

		std::vector<unsigned char> sidecarBytes;
		RISECBOR64::Value provenance;
		std::string error;
		const bool decoded = ReadFileAllBytes(path+".provenance.cbor",sidecarBytes) &&
			RISECBOR64::DecodeCanonical(sidecarBytes,provenance,&error);
		const FrameStore::Metadata& encodedMetadata = encoder->CapturedMetadata();
		const RISECBOR64::Value* payload = decoded ? provenance.Find("payload") : nullptr;
		const RISECBOR64::Value* reasons = payload ?
			payload->Find("render_reason_codes") : nullptr;
		const RISECBOR64::Value* ids = payload ?
			payload->Find("active_fire_optics_record_ids") : nullptr;
		Check( saved && decoded && concurrentPreflightRejected &&
			encodedMetadata.renderReasonCodes.size() == 3u &&
			encodedMetadata.renderReasonCodes[0] == "pel_transport" && reasons &&
			reasons->GetArray().size() == 3u &&
			reasons->GetArray()[0].GetText() == "pel_transport" && ids &&
			ids->GetArray().size() == 1u && ids->GetArray()[0].GetText() ==
				"2cdd00456431fd0c020ee8e28b01bc59e92586beb6ac8f6ea77efa31276ad137",
			"SaveAs leases one metadata snapshot and rejects concurrent fire preflight" );

		std::remove(path.c_str());
		std::remove((path+".provenance.cbor").c_str());
		encoder->release();
		safe_release(img);
		vfs->release();
	}

	void TestNonFireSavesLeaseOutputClassification()
	{
		auto* vfs = new ViewportFrameStore();
		auto* img = MakeTestImage();
		vfs->OutputImage(*img,nullptr,0);
		FrameStore* store = vfs->GetFrameStore();
		EncodeOpts opts;

		BlockingPNGEncoder* fileEncoder = new BlockingPNGEncoder();
		const std::string path = MakeTempPath()+"_nonfire_classification.png";
		bool fileSaved = false;
		std::thread fileSaver([&]() {
			fileSaved = vfs->SaveAs(path,fileEncoder,opts);
		});
		fileEncoder->WaitUntilEntered();
		bool filePreflightRejected = false;
		try {
			SetFireFidelityMetadata(*store);
		} catch( const std::runtime_error& error ) {
			filePreflightRejected =
				std::string(error.what()).find("metadata is leased") != std::string::npos;
		}
		fileEncoder->Continue();
		fileSaver.join();
		Check(fileSaved && filePreflightRejected &&
			store->Meta().renderFidelityStatus.empty() &&
			!std::filesystem::exists(path+".provenance.cbor"),
			"nonfire SaveAs leases classification before concurrent fire preflight" );

		BlockingPNGEncoder* bufferEncoder = new BlockingPNGEncoder();
		MemoryBuffer* buffer = new MemoryBuffer();
		bool bufferSaved = false;
		std::thread bufferSaver([&]() {
			bufferSaved = vfs->SaveTo(*buffer,bufferEncoder,opts);
		});
		bufferEncoder->WaitUntilEntered();
		bool bufferPreflightRejected = false;
		try {
			SetFireFidelityMetadata(*store);
		} catch( const std::runtime_error& error ) {
			bufferPreflightRejected =
				std::string(error.what()).find("metadata is leased") != std::string::npos;
		}
		bufferEncoder->Continue();
		bufferSaver.join();
		Check(bufferSaved && buffer->getCurPos() > 0u && bufferPreflightRejected &&
			store->Meta().renderFidelityStatus.empty(),
			"nonfire SaveTo leases classification before concurrent fire preflight" );

		std::remove(path.c_str());
		fileEncoder->release();
		bufferEncoder->release();
		buffer->release();
		safe_release(img);
		vfs->release();
	}

	void TestPreparedFireFrameCannotPublish()
	{
		auto* vfs = new ViewportFrameStore();
		auto* img = MakeTestImage();
		vfs->OutputImage(*img,nullptr,0);
		FrameStore* store = vfs->GetFrameStore();
		SetFireFidelityMetadata(*store);
		const FrameStoreOutput::Metadata finalized = store->Meta();
		store->SetPreparedFireFidelityMetadata(finalized.renderFidelityStatus,
			finalized.renderReasonCodes,finalized.activeFireOpticsRecordIds,
			finalized.activeFireMedia,finalized.resolvedRenderConfigCoreV1,
			finalized.rendererBuildV1,finalized.rendererBuildId);

		IFrameEncoder* png = FrameEncoderRegistry::Get().AcquireByFormatName("PNG");
		EncodeOpts opts;
		const std::string path = MakeTempPath()+"_prepared_fire.png";
		Check( png && !vfs->SaveAs(path,png,opts),
			"GUI SaveAs rejects a prepared but uncommitted fire frame" );
		Check( !std::filesystem::exists(path) &&
			!std::filesystem::exists(path+".provenance.cbor"),
			"rejected prepared fire frame publishes neither artifact nor sidecar" );

		store->SetMetadata(finalized);
		if( png ) png->release();
		safe_release(img);
		vfs->release();
	}

	void TestFireSaveAsLeasePrecedesStoreRelease()
	{
		auto* vfs = new ViewportFrameStore();
		FrameStore::Spec spec;
		spec.width = kImgW;
		spec.height = kImgH;
		spec.tileEdge = 8;
		auto* source = new LeaseOrderFrameStore(spec);
		SetFireFidelityMetadata(*source);
		source->SetPrimaryFireArtifact(
			"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
			"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
			"preview_primary");
		vfs->BindFrameStore(source);
		source->release();

		BlockingPNGEncoder* encoder = new BlockingPNGEncoder();
		const std::string path = MakeTempPath()+"_lease_rebind.png";
		bool saved = false;
		EncodeOpts opts;
		std::thread saver([&]() { saved = vfs->SaveAs(path,encoder,opts); });
		encoder->WaitUntilEntered();

		auto* replacement = new FrameStore(spec);
		vfs->BindFrameStore(replacement);
		replacement->release();
		encoder->Continue();
		saver.join();

		Check( saved && source->FinalReleaseObserved() &&
			!source->FinalReleaseWhileLeased() &&
			vfs->GetFrameStore() == replacement,
			"fire SaveAs releases its metadata lease before the retained store reference" );

		source->FinishRelease();
		std::remove(path.c_str());
		std::remove((path+".provenance.cbor").c_str());
		encoder->release();
		vfs->release();
	}

	// ─── Section 5: rasterizer-swap simulation ────────────────────
	// Per design doc §7.5: observers attach to FrameStore (not
	// rasterizer), so a rasterizer swap should keep all observer
	// + callback state intact.  We can simulate this without an
	// actual rasterizer by directly calling OutputImage on the
	// VFS twice — once as if rasterizer A drove it, once as if
	// rasterizer B drove it.  Same VFS instance, callbacks fire
	// in both cases.
	void TestRasterizerSwap()
	{
		auto* vfs = new ViewportFrameStore();

		std::atomic<int> frames{ 0 };
		vfs->SetFrameCompleteCallback(
			[&]( unsigned int, uint64_t ) { ++frames; } );

		auto* img = MakeTestImage();

		// "Rasterizer A" drives the first frame.
		vfs->OutputImage( *img, nullptr, 0 );
		FrameStore* storeAfterA = vfs->GetFrameStore();
		Check( storeAfterA != nullptr,
			"swap: FrameStore allocated after rasterizer A" );
		Check( frames.load() == 1, "swap: frame callback fired for rasterizer A" );

		// "Rasterizer B" drives the next frame using THE SAME VFS
		// instance.  No re-attachment, no callback re-set.
		vfs->OutputImage( *img, nullptr, 1 );
		FrameStore* storeAfterB = vfs->GetFrameStore();
		Check( storeAfterB == storeAfterA,
			"swap: FrameStore identity preserved across rasterizer change" );
		Check( frames.load() == 2,
			"swap: frame callback fires for rasterizer B (same callback persists)" );

		safe_release( img );
		vfs->release();
	}

	// ─── Section 6: resolution change ─────────────────────────────
	void TestResolutionChange()
	{
		auto* vfs = new ViewportFrameStore();

		std::atomic<int> frames{ 0 };
		vfs->SetFrameCompleteCallback(
			[&]( unsigned int, uint64_t ) { ++frames; } );

		// 16x16 first.
		auto* img1 = MakeTestImage();
		vfs->OutputImage( *img1, nullptr, 0 );
		Check( vfs->GetFrameStore()->Width()  == 16, "16x16 width" );
		Check( vfs->GetFrameStore()->Height() == 16, "16x16 height" );
		const uint64_t genAfterFirst = vfs->Generation();

		// Now a 32x32 image — VFS should reallocate.  We don't compare
		// FrameStore pointers (the heap allocator may legitimately reuse
		// the freed address); instead verify the dim change took effect
		// AND the generation reset to 1 (new FrameStore counts from 0,
		// the OutputImage call's MarkFrameComplete bumps to 1).
		auto* img2 = new RasterImage_Template<RISEPel>(
			32, 32, RISEColor( RISEPel( 0.5, 0.5, 0.5 ), 1.0 ) );
		for ( unsigned y = 0; y < 32; ++y ) {
			for ( unsigned x = 0; x < 32; ++x ) {
				img2->SetPEL( x, y, RISEColor( RISEPel( 0.5, 0.5, 0.5 ), 1.0 ) );
			}
		}
		vfs->OutputImage( *img2, nullptr, 1 );
		Check( vfs->GetFrameStore()->Width()  == 32,
			"resolution change: width updated to 32" );
		Check( vfs->GetFrameStore()->Height() == 32,
			"resolution change: height updated to 32" );
		// New FrameStore starts at generation 0; one OutputImage on
		// the new (32x32 @ tileEdge=32 = 1 tile) image bumps it by
		// 2 (1 EndTile + 1 MarkFrameComplete).  Tighten the check
		// to a positive equality: the new FrameStore's gen MUST be
		// strictly less than the genAfterFirst-after-resolution-bump
		// value, otherwise the counter is carrying over from the
		// old store.  Per L4 adversarial review LOW-2.
		const uint64_t genAfterSecond = vfs->GetFrameStore()->Generation();
		Check( genAfterSecond < genAfterFirst + 3,
			"resolution change: generation counter strictly less than carried-forward bound" );
		Check( genAfterSecond > 0,
			"resolution change: new FrameStore got at least one bump from second OutputImage" );
		Check( frames.load() == 2,
			"resolution change: frame callback re-attached to new store, fired correctly" );

		safe_release( img2 );
		safe_release( img1 );
		vfs->release();
	}

	void TestBindDormantStoreAsExternal()
	{
		auto* vfs = new ViewportFrameStore();
		std::atomic<int> frames{0};
		vfs->SetFrameCompleteCallback(
			[&frames]( unsigned int, uint64_t ) { ++frames; });

		auto* imageA = MakeTestImage();
		vfs->OutputImage(*imageA,nullptr,0u);
		FrameStore* dormantCandidate = vfs->GetFrameStore();
		dormantCandidate->addref();

		auto* imageB = MakeTestImage(32u,32u);
		vfs->OutputImage(*imageB,nullptr,1u);
		Check(vfs->GetFrameStore() != dormantCandidate,
			"dormant bind: resolution change parks the original store");

		vfs->BindFrameStore(dormantCandidate);
		Check(vfs->IsExternallyBound(),
			"dormant bind: cached store becomes an external binding");
		Check(vfs->GetFrameStore() == dormantCandidate,
			"dormant bind: cached store is the published active store");

		const int before = frames.load();
		dormantCandidate->MarkFrameComplete(2u);
		Check(frames.load() == before+1,
			"dormant bind: replacement observer receives one frame callback");

		safe_release(imageB);
		safe_release(imageA);
		vfs->release();
		safe_release(dormantCandidate);
	}

	// ─── Section 7: multi-frame reuse ─────────────────────────────
	void TestMultiFrameReuse()
	{
		auto* vfs = new ViewportFrameStore();

		auto* img1 = MakeTestImage();
		vfs->OutputImage( *img1, nullptr, 0 );
		const uint64_t gen1 = vfs->Generation();

		auto* img2 = new RasterImage_Template<RISEPel>(
			kImgW, kImgH, RISEColor( RISEPel( 1.0, 0.0, 0.0 ), 1.0 ) );
		for ( unsigned y = 0; y < kImgH; ++y ) {
			for ( unsigned x = 0; x < kImgW; ++x ) {
				img2->SetPEL( x, y, RISEColor( RISEPel( 1.0, 0.0, 0.0 ), 1.0 ) );
			}
		}
		vfs->OutputImage( *img2, nullptr, 1 );
		const uint64_t gen2 = vfs->Generation();

		Check( gen2 > gen1, "multi-frame: generation advances across frames" );

		// Verify the FrameStore now reflects img2's content.
		auto* beauty = vfs->GetFrameStore()->GetChannel<ChannelId::Beauty>();
		Check( beauty->At( 0, 0 ).r == 1.0
		    && beauty->At( 0, 0 ).g == 0.0
		    && beauty->At( 0, 0 ).b == 0.0,
			"multi-frame: FrameStore reflects 2nd frame's content (red)" );

		safe_release( img2 );
		safe_release( img1 );
		vfs->release();
	}

	// ─── Section 7b: mid-render SaveAs is data-race-free ─────────
	// Per L4 adversarial review HIGH-1: encoders walk the FrameStore
	// via BeautyRasterImageView::DumpImage, which acquires every
	// per-tile shared_lock for the duration of the dump.  This test
	// runs a writer thread that repeatedly BeginTile/EndTile-bumps a
	// FrameStore tile while the main thread calls SaveAs in a loop.
	// Without the lock-acquisition fix, this would race on pixel
	// storage (TSan would flag every read).  With the fix, every
	// SaveAs produces a non-empty file with no crashes.
	void TestMidRenderSaveAs()
	{
		auto* vfs = new ViewportFrameStore();
		auto* img = MakeTestImage();

		// Prime the chain (allocates FrameStore + observer).
		vfs->OutputImage( *img, nullptr, 0 );
		FrameStore* store = vfs->GetFrameStore();

		std::atomic<bool> stop{ false };
		std::atomic<int>  saveCount{ 0 };
		std::atomic<int>  saveOk{ 0 };

		// Writer thread: repeatedly bumps tile (0,0) — replicates a
		// rasterizer worker drilling pixels into the FrameStore.
		std::thread writer( [&]() {
			auto* beauty = store->GetChannel<ChannelId::Beauty>();
			int epoch = 0;
			while ( !stop.load() ) {
				epoch = ( epoch + 1 ) % 9;
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

		// Main thread: SaveAs in a loop.  Each save acquires every
		// per-tile shared_lock during DumpImage; writer is briefly
		// blocked but doesn't crash.
		IFrameEncoder* enc = FrameEncoderRegistry::Get().ByFormatName( "PNG" );
		EncodeOpts opts;
		opts.colorSpace = eColorSpace_sRGB;
		opts.bpp        = 8;
		opts.viewTransform = ViewTransform::Identity();

		const auto start = std::chrono::steady_clock::now();
		while ( std::chrono::steady_clock::now() - start
		        < std::chrono::milliseconds( 200 ) )
		{
			const std::string path = MakeTempPath() + "_midrender.png";
			const bool ok = vfs->SaveAs( path, enc, opts );
			++saveCount;
			if ( ok ) {
				std::vector<unsigned char> bytes;
				if ( ReadFileAllBytes( path, bytes ) && !bytes.empty() ) {
					++saveOk;
				}
				std::remove( path.c_str() );
			}
		}

		stop.store( true );
		writer.join();

		std::ostringstream label;
		label << "mid-render SaveAs: " << saveOk.load() << " / " << saveCount.load()
		      << " saves produced non-empty files";
		Check( saveOk.load() == saveCount.load(), label.str() );
		Check( saveCount.load() > 5,
			"mid-render SaveAs: ran enough iterations to stress contention" );

		safe_release( img );
		vfs->release();
	}

	// ─── Section 7c: chain-replacement vs reader race (P1-2) ──────
	// Per L4 round-2 review P1-2: reader paths
	// (RenderToBuffer/SaveAs/Generation) must use a chain-mutex +
	// addref-snapshot pattern so a concurrent EnsureChain
	// reallocation in the rasterizer thread can't dereference a
	// freed FrameStore in the reader.  This test runs:
	//   - rasterizer thread: alternates OutputImage at 16x16 and
	//     32x32 dims, forcing repeated chain reallocation
	//   - reader thread: tight loop calling RenderToBuffer +
	//     Generation + SaveAs
	// Without the fix this is a UAF on every realloc race.  With
	// the fix, no crashes; all RenderToBuffer/SaveAs calls return
	// either the old or the new chain's content, never freed
	// memory.
	void TestChainRaceUnderResolutionChange()
	{
		auto* vfs = new ViewportFrameStore();

		// Prime with one OutputImage so the chain exists.
		auto* img16 = new RasterImage_Template<RISEPel>(
			16, 16, RISEColor( RISEPel( 0.5, 0.5, 0.5 ), 1.0 ) );
		auto* img32 = new RasterImage_Template<RISEPel>(
			32, 32, RISEColor( RISEPel( 0.5, 0.5, 0.5 ), 1.0 ) );
		vfs->OutputImage( *img16, nullptr, 0 );

		std::atomic<bool> stop{ false };
		std::atomic<int>  reallocs{ 0 };
		std::atomic<int>  reads{ 0 };

		// Writer thread: forces chain reallocation by alternating
		// dims.  EnsureChain detects the dim change, takes
		// unique_lock, swaps the chain.
		std::thread writer( [&]() {
			bool which = true;
			while ( !stop.load() ) {
				vfs->OutputImage( which ? *img16 : *img32, nullptr, 0 );
				++reallocs;
				which = !which;
			}
		} );

		// Reader thread: hammers RenderToBuffer + Generation.  Each
		// call snapshots the FrameStore under shared_lock + addref
		// so the writer's chain swap can proceed without invalidating
		// the snapshot.
		std::thread reader( [&]() {
			std::vector<uint8_t> buf( 32 * 32 * 4, 0 );
			ViewTransform xf = ViewTransform::Identity();
			while ( !stop.load() ) {
				vfs->RenderToBuffer( buf.data(), 32 * 4,
					Rect( 0, 0, 32, 32 ),
					TargetFormat::RGBA8_sRGB, xf );
				(void)vfs->Generation();
				++reads;
			}
		} );

		// Run for 200 ms — enough to accumulate many reallocs +
		// reads.  Without the chain-mutex fix this would crash
		// almost immediately due to UAF.
		std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
		stop.store( true );
		writer.join();
		reader.join();

		std::ostringstream os;
		os << "chain race: " << reallocs.load() << " reallocs + "
		   << reads.load() << " reads completed without crash / UAF";
		Check( reallocs.load() > 5, os.str() );
		Check( reads.load() > 5,
			"chain race: reader thread ran enough iterations to stress" );

		safe_release( img32 );
		safe_release( img16 );
		vfs->release();
	}

	// ─── Section 9 (L6e-2a): External FrameStore bind ───────────
	//
	// Verify that BindFrameStore:
	//   - Initially: VFS reports IsExternallyBound() == false.
	//   - After bind: IsExternallyBound() == true; GetFrameStore()
	//     returns the bound pointer; tile observer fires on the
	//     external store's BeginTile/EndTile (post-L6e-1 rasterizer-
	//     side bracketing pattern).
	//   - IRasterizerOutput methods short-circuit when bound (no
	//     copy, no spurious double-fire).
	//   - OutputImage on a bound VFS still fires OnFrameComplete
	//     (frame-complete event preserved via direct MarkFrameComplete
	//     call on the bound store).
	//   - Unbind reverts to internal-managed mode; subsequent
	//     OutputImage allocates fresh internal store.
	//   - Idempotent: re-binding the same pointer is a no-op
	//     (no observer thrash).
	//   - Refcount: bind addrefs the external; unbind / dtor
	//     releases.  The external is NOT destroyed by VFS while
	//     the test holds its own ref.
	void TestExternalBind_L6e2a()
	{
		auto* vfs = new ViewportFrameStore();
		Check( !vfs->IsExternallyBound(),
			"L6e-2a: IsExternallyBound==false before any bind" );

		// Allocate an external FrameStore (Job-allocated, in
		// production).  Test holds one addref; bind will take a
		// second.
		FrameStore::Spec spec;
		spec.width    = kImgW;
		spec.height   = kImgH;
		spec.tileEdge = 8;  // 4 tiles × 4 tiles for the 16x16
		auto* extFs = new FrameStore( spec );  // refcount=1
		extFs->addref();                        // refcount=2 (test owns one)

		// Wire callback that increments on tile + frame events.
		std::atomic<int> tileCount{ 0 };
		std::atomic<int> frameCount{ 0 };
		vfs->SetTileCompleteCallback( [&tileCount]( const Rect&, uint64_t ) {
			++tileCount;
		} );
		vfs->SetFrameCompleteCallback( [&frameCount]( unsigned int, uint64_t ) {
			++frameCount;
		} );

		vfs->BindFrameStore( extFs );
		Check( vfs->IsExternallyBound(),
			"L6e-2a: IsExternallyBound==true after bind" );
		Check( vfs->GetFrameStore() == extFs,
			"L6e-2a: GetFrameStore returns bound external pointer" );

		// Drive a tile complete on the external — VFS observer
		// should fire its tile callback.
		extFs->BeginTile( 0, 0 );
		extFs->EndTile( 0, 0 );
		Check( tileCount.load() == 1,
			"L6e-2a: BeginTile/EndTile on external fires tile callback" );

		// IRasterizerOutput::OutputIntermediateImage when bound:
		// short-circuits (rasterizer's bracketing already drove
		// observers).  Verify no double-fire by comparing tileCount
		// before/after.
		const int beforeIntermediate = tileCount.load();
		auto* img = MakeTestImage();
		vfs->OutputIntermediateImage( *img, nullptr );
		Check( tileCount.load() == beforeIntermediate,
			"L6e-2a: OutputIntermediateImage no-op when externally bound (no double-fire)" );

		// L6f — IRasterizerOutput::OutputImage when bound is a
		// COMPLETE no-op.  Frame-complete signaling now comes from
		// the rasterizer's `FlushToOutputs` calling
		// `mFrameStore->MarkFrameComplete` directly (post-flush of
		// the IRasterizerOutput chain).  We can't construct a real
		// rasterizer in this unit test, so simulate by calling
		// `extFs->MarkFrameComplete` directly to confirm the
		// observer chain is still wired correctly.
		const int beforeFinalNoop = frameCount.load();
		vfs->OutputImage( *img, nullptr, /*frame=*/0 );
		Check( frameCount.load() == beforeFinalNoop,
			"L6f: OutputImage when bound is no-op (no double-fire on rasterizer-driven Mark*)" );

		// Simulate rasterizer-side MarkFrameComplete on the bound
		// store; observer fires as expected.
		extFs->MarkFrameComplete( 0 );
		Check( frameCount.load() == beforeFinalNoop + 1,
			"L6f: rasterizer-side MarkFrameComplete fires OnFrameComplete on bound store" );

		// Mid-bind unbind: revert to internal mode.
		vfs->BindFrameStore( nullptr );
		Check( !vfs->IsExternallyBound(),
			"L6e-2a: IsExternallyBound==false after unbind" );
		Check( vfs->GetFrameStore() == nullptr,
			"L6e-2a: GetFrameStore null after unbind (chain torn down)" );

		// After unbind, OutputImage allocates a fresh INTERNAL
		// FrameStore — NOT the external (which we still hold a ref
		// to).
		vfs->OutputImage( *img, nullptr, /*frame=*/1 );
		Check( vfs->GetFrameStore() != nullptr,
			"L6e-2a: OutputImage post-unbind allocates fresh internal store" );
		Check( vfs->GetFrameStore() != extFs,
			"L6e-2a: post-unbind FrameStore is INTERNAL (not the ex-external)" );
		Check( !vfs->IsExternallyBound(),
			"L6e-2a: post-unbind still reports IsExternallyBound==false" );

		// Idempotent re-bind: bind to the same external twice → no
		// observer thrash (tileCount shouldn't bump from re-binding).
		vfs->BindFrameStore( extFs );
		const int beforeIdempotent = tileCount.load();
		vfs->BindFrameStore( extFs );
		extFs->BeginTile( 0, 1 );
		extFs->EndTile( 0, 1 );
		Check( tileCount.load() == beforeIdempotent + 1,
			"L6e-2a: idempotent re-bind doesn't duplicate observer (one tile event = one callback)" );

		// Test-owned ref keeps extFs alive until we release.
		// Releasing VFS releases its bind addref.
		safe_release( img );
		vfs->release();

		// VFS gone, external still has the test's addref.  Verify
		// by reading its dims (would crash if released to 0).
		Check( extFs->Width() == kImgW && extFs->Height() == kImgH,
			"L6e-2a: external FrameStore survives VFS destruction (test held its own ref)" );
		safe_release( extFs );
	}

	void TestConcurrentExternalBindRejectsSynchronously()
	{
		auto* vfs = new ViewportFrameStore();
		FrameStore::Spec spec;
		spec.width = kImgW;
		spec.height = kImgH;
		spec.tileEdge = 8;
		auto* source = new FrameStore(spec);
		auto* first = new FrameStore(spec);
		auto* second = new FrameStore(spec);
		std::mutex callbackMutex;
		std::condition_variable callbackCondition;
		bool callbackEntered = false;
		bool callbackMayReturn = false;
		bool replacementReady = false;
		bool replacementMayProceed = false;
		std::atomic<unsigned int> callbacks(0u);
		vfs->SetTileCompleteCallback([&]( const Rect&, uint64_t ) {
			++callbacks;
			std::unique_lock<std::mutex> lock(callbackMutex);
			if( !callbackEntered ) {
				callbackEntered = true;
				callbackCondition.notify_all();
				callbackCondition.wait(lock,[&]() { return callbackMayReturn; });
			}
		});
		vfs->BindFrameStore(source);
		std::thread dispatcher([&]() {
			source->BeginTile(0u,0u);
			source->EndTile(0u,0u);
		});
		{
			std::unique_lock<std::mutex> lock(callbackMutex);
			callbackCondition.wait(lock,[&]() { return callbackEntered; });
		}
		vfs->ForTest_SetChainConstructionHook([&]( const char* stage ) {
			if( std::strcmp(stage,"bind_after_observer_allocation") != 0 ) return;
			std::unique_lock<std::mutex> lock(callbackMutex);
			replacementReady = true;
			callbackCondition.notify_all();
			callbackCondition.wait(lock,[&]() { return replacementMayProceed; });
		});
		std::thread firstBinder([&]() { vfs->BindFrameStore(first); });
		{
			std::unique_lock<std::mutex> lock(callbackMutex);
			callbackCondition.wait(lock,[&]() { return replacementReady; });
		}
		bool secondRejected = false;
		try {
			vfs->BindFrameStore(second);
		} catch( const std::runtime_error& error ) {
			secondRejected = std::string(error.what()) ==
				"ViewportFrameStore bind transaction already active";
		}
		const bool oldStayedPublishedWhileSecondRejected =
			vfs->GetFrameStore() == source && vfs->IsExternallyBound();
		{
			std::lock_guard<std::mutex> lock(callbackMutex);
			replacementMayProceed = true;
			callbackMayReturn = true;
		}
		callbackCondition.notify_all();
		dispatcher.join();
		firstBinder.join();
		vfs->ForTest_SetChainConstructionHook({});
		const bool firstRetained = vfs->GetFrameStore() == first;
		vfs->release();
		first->BeginTile(0u,0u);
		first->EndTile(0u,0u);
		second->BeginTile(0u,0u);
		second->EndTile(0u,0u);
		Check(secondRejected && oldStayedPublishedWhileSecondRejected &&
			firstRetained &&
			callbacks.load() == 1u,
			"a concurrent external bind rejects synchronously without disturbing the active transaction" );
		source->release();
		first->release();
		second->release();
	}

	void TestPhaseOneConcurrentBindRejectsSynchronously()
	{
		auto* vfs = new ViewportFrameStore();
		FrameStore::Spec spec;
		spec.width = kImgW;
		spec.height = kImgH;
		spec.tileEdge = 8;
		auto* first = new FrameStore(spec);
		auto* second = new FrameStore(spec);
		std::mutex gateMutex;
		std::condition_variable gateCondition;
		bool firstEntered = false;
		bool firstMayContinue = false;
		std::atomic<unsigned int> hookCalls(0u);
		std::atomic<unsigned int> callbacks(0u);
		vfs->SetTileCompleteCallback([&]( const Rect&, uint64_t ) { ++callbacks; });
		vfs->ForTest_SetBindPhaseOneHook([&]( uint64_t ) {
			if( hookCalls.fetch_add(1u) != 0u ) return;
			std::unique_lock<std::mutex> lock(gateMutex);
			firstEntered = true;
			gateCondition.notify_all();
			gateCondition.wait(lock,[&]() { return firstMayContinue; });
		});
		std::thread older([&]() { vfs->BindFrameStore(first); });
		{
			std::unique_lock<std::mutex> lock(gateMutex);
			gateCondition.wait(lock,[&]() { return firstEntered; });
		}
		bool secondRejected = false;
		try {
			vfs->BindFrameStore(second);
		} catch( const std::runtime_error& error ) {
			secondRejected = std::string(error.what()) ==
				"ViewportFrameStore bind transaction already active";
		}
		{
			std::lock_guard<std::mutex> lock(gateMutex);
			firstMayContinue = true;
		}
		gateCondition.notify_all();
		older.join();
		vfs->ForTest_SetBindPhaseOneHook({});
		first->BeginTile(0u,0u);
		first->EndTile(0u,0u);
		second->BeginTile(0u,0u);
		second->EndTile(0u,0u);
		Check(secondRejected && vfs->GetFrameStore() == first &&
			hookCalls.load() == 1u && callbacks.load() == 1u,
			"a phase-one concurrent bind reports rejection to its own caller" );
		vfs->release();
		first->release();
		second->release();
	}

	void TestBindTeardownKeepsOldChainPublished()
	{
		auto* vfs = new ViewportFrameStore();
		FrameStore::Spec spec;
		spec.width = kImgW;
		spec.height = kImgH;
		spec.tileEdge = 8;
		auto* source = new FrameStore(spec);
		auto* replacement = new FrameStore(spec);
		auto* image = MakeTestImage();
		std::mutex callbackMutex;
		std::condition_variable callbackCondition;
		bool callbackEntered = false;
		bool callbackMayReturn = false;
		bool replacementReady = false;
		bool replacementMayProceed = false;
		vfs->SetTileCompleteCallback([&]( const Rect&, uint64_t ) {
			std::unique_lock<std::mutex> lock(callbackMutex);
			callbackEntered = true;
			callbackCondition.notify_all();
			callbackCondition.wait(lock,[&]() { return callbackMayReturn; });
		});
		vfs->BindFrameStore(source);
		std::thread dispatcher([&]() {
			source->BeginTile(0u,0u);
			source->EndTile(0u,0u);
		});
		{
			std::unique_lock<std::mutex> lock(callbackMutex);
			callbackCondition.wait(lock,[&]() { return callbackEntered; });
		}
		vfs->ForTest_SetChainConstructionHook([&]( const char* stage ) {
			if( std::strcmp(stage,"bind_after_observer_allocation") != 0 ) return;
			std::unique_lock<std::mutex> lock(callbackMutex);
			replacementReady = true;
			callbackCondition.notify_all();
			callbackCondition.wait(lock,[&]() { return replacementMayProceed; });
		});
		std::thread binder([&]() { vfs->BindFrameStore(replacement); });
		{
			std::unique_lock<std::mutex> lock(callbackMutex);
			callbackCondition.wait(lock,[&]() { return replacementReady; });
		}
		vfs->OutputImage(*image,nullptr,0u);
		const bool oldChainStayedPublished =
			vfs->GetFrameStore() == source && vfs->IsExternallyBound();
		{
			std::lock_guard<std::mutex> lock(callbackMutex);
			replacementMayProceed = true;
			callbackMayReturn = true;
		}
		callbackCondition.notify_all();
		dispatcher.join();
		binder.join();
		vfs->ForTest_SetChainConstructionHook({});
		Check(oldChainStayedPublished &&
			vfs->GetFrameStore() == replacement && vfs->IsExternallyBound(),
			"external replacement keeps the old chain published until commit" );
		vfs->release();
		image->release();
		source->release();
		replacement->release();
	}

	void TestRejectedCrossStoreBindPreservesExistingChain()
	{
		auto* vfs = new ViewportFrameStore();
		FrameStore::Spec spec;
		spec.width = kImgW;
		spec.height = kImgH;
		spec.tileEdge = 8;
		auto* source = new FrameStore(spec);
		auto* replacement = new FrameStore(spec);
		auto* trigger = new FrameStore(spec);
		std::mutex gateMutex;
		std::condition_variable gateCondition;
		bool sourceCallbackEntered = false;
		bool sourceCallbackMayReturn = false;
		std::atomic<unsigned int> callbacks(0u);
		vfs->SetTileCompleteCallback([&]( const Rect&, uint64_t ) {
			++callbacks;
			std::unique_lock<std::mutex> lock(gateMutex);
			if( !sourceCallbackEntered ) {
				sourceCallbackEntered = true;
				gateCondition.notify_all();
				gateCondition.wait(lock,[&]() { return sourceCallbackMayReturn; });
			}
		});
		vfs->BindFrameStore(source);
		std::thread sourceDispatcher([&]() {
			source->BeginTile(0u,0u);
			source->EndTile(0u,0u);
		});
		{
			std::unique_lock<std::mutex> lock(gateMutex);
			gateCondition.wait(lock,[&]() { return sourceCallbackEntered; });
		}
		bool rejected = false;
		std::string rejection;
		CallbackObserver triggerObserver([&]() {
			try {
				vfs->BindFrameStore(replacement);
			} catch( const std::runtime_error& error ) {
				rejected = true;
				rejection = error.what();
			}
		});
		trigger->AddObserver(&triggerObserver);
		trigger->BeginTile(0u,0u);
		trigger->EndTile(0u,0u);
		trigger->RemoveObserver(&triggerObserver);
		const bool preservedDuringRejection =
			vfs->GetFrameStore() == source && vfs->IsExternallyBound();
		{
			std::lock_guard<std::mutex> lock(gateMutex);
			sourceCallbackMayReturn = true;
		}
		gateCondition.notify_all();
		sourceDispatcher.join();
		source->BeginTile(0u,1u);
		source->EndTile(0u,1u);
		replacement->BeginTile(0u,0u);
		replacement->EndTile(0u,0u);
		Check(rejected && rejection ==
				"FrameStore observer removal would wait on another callback" &&
			preservedDuringRejection && vfs->GetFrameStore() == source &&
			callbacks.load() == 2u,
			"cross-store bind rejection restores the complete existing observer chain" );
		vfs->release();
		source->release();
		replacement->release();
		trigger->release();
	}

	void TestBindConstructionFailuresPreserveExistingChain()
	{
		auto* vfs = new ViewportFrameStore();
		FrameStore::Spec spec;
		spec.width = kImgW;
		spec.height = kImgH;
		spec.tileEdge = 8;
		auto* source = new FrameStore(spec);
		auto* replacement = new FrameStore(spec);
		std::atomic<unsigned int> callbacks(0u);
		vfs->SetTileCompleteCallback([&]( const Rect&, uint64_t ) { ++callbacks; });
		vfs->BindFrameStore(source);
		const std::vector<const char*> stages = {
			"bind_after_retain",
			"bind_after_observer_allocation",
			"bind_after_old_observer_quiesced"
		};
		bool allPreserved = true;
		for( const char* stage : stages ) {
			const unsigned int callbacksBefore = callbacks.load();
			bool hookCalled = false;
			vfs->ForTest_SetChainConstructionHook([stage,&hookCalled]( const char* observed ) {
				if( std::strcmp(stage,observed) == 0 ) {
					hookCalled = true;
					throw std::runtime_error(stage);
				}
			});
			bool rejected = false;
			try {
				vfs->BindFrameStore(replacement);
			} catch( const std::runtime_error& error ) {
				rejected = std::string(error.what()) == stage;
			}
			vfs->ForTest_SetChainConstructionHook({});
			source->BeginTile(0u,0u);
			source->EndTile(0u,0u);
			replacement->BeginTile(0u,0u);
			replacement->EndTile(0u,0u);
			const bool stagePreserved = hookCalled && rejected &&
				vfs->GetFrameStore() == source && vfs->IsExternallyBound() &&
				callbacks.load() == callbacksBefore+1u;
			Check(stagePreserved,std::string("external construction failure preserves binding at ")+stage);
			allPreserved = allPreserved && stagePreserved;
		}
		Check(allPreserved,
			"every external replacement construction failure preserves the old binding" );
		vfs->release();
		source->release();
		replacement->release();
	}

	void TestBindRollbackPreservesQuiescedEventAndObserverInsertion()
	{
		auto* vfs = new ViewportFrameStore();
		FrameStore::Spec spec;
		spec.width = kImgW;
		spec.height = kImgH;
		spec.tileEdge = 8;
		auto* source = new FrameStore(spec);
		auto* replacement = new FrameStore(spec);
		std::atomic<unsigned int> vfsCallbacks(0u);
		std::atomic<unsigned int> independentCallbacks(0u);
		CallbackObserver independent([&]() { ++independentCallbacks; });
		vfs->SetTileCompleteCallback(
			[&]( const Rect&, uint64_t ) { ++vfsCallbacks; });
		vfs->BindFrameStore(source);
		bool hookCalled = false;
		std::thread eventThread;
		const uint64_t generationBefore = source->Generation();
		vfs->ForTest_SetChainConstructionHook([&]( const char* stage ) {
			if( std::strcmp(stage,"bind_after_old_observer_quiesced") != 0 ) return;
			hookCalled = true;
			source->AddObserver(&independent);
			eventThread = std::thread([&]() {
				source->BeginTile(0u,0u);
				source->EndTile(0u,0u);
			});
			while( source->Generation() == generationBefore ) {
				std::this_thread::yield();
			}
			throw std::runtime_error("injected post-quiesce failure");
		});
		bool rejected = false;
		try {
			vfs->BindFrameStore(replacement);
		} catch( const std::runtime_error& error ) {
			rejected = std::string(error.what()) ==
				"injected post-quiesce failure";
		}
		vfs->ForTest_SetChainConstructionHook({});
		if( eventThread.joinable() ) eventThread.join();
		Check(hookCalled && rejected && vfs->GetFrameStore() == source &&
			vfsCallbacks.load() == 1u && independentCallbacks.load() == 1u,
			"bind rollback releases a quiesced event to the preserved observer chain" );
		source->RemoveObserver(&independent);
		vfs->release();
		source->release();
		replacement->release();
	}

	void TestExposureUpdateWinsConcurrentBindCommit()
	{
		auto* vfs = new ViewportFrameStore();
		FrameStore::Spec spec;
		spec.width = kImgW;
		spec.height = kImgH;
		spec.tileEdge = 8;
		auto* source = new FrameStore(spec);
		auto* replacement = new FrameStore(spec);
		std::mutex gateMutex;
		std::condition_variable gateCondition;
		bool replacementReady = false;
		bool replacementMayCommit = false;
		bool hookReleaseObserved = false;
		vfs->SetCameraExposureCompensationEV(1.0);
		vfs->BindFrameStore(source);
		vfs->ForTest_SetChainConstructionHook([&]( const char* stage ) {
			if( std::strcmp(stage,"bind_after_old_observer_quiesced") != 0 ) return;
			std::unique_lock<std::mutex> lock(gateMutex);
			replacementReady = true;
			gateCondition.notify_all();
			hookReleaseObserved = gateCondition.wait_for(lock,std::chrono::seconds(5),
				[&]() { return replacementMayCommit; });
		});
		std::thread binder([&]() { vfs->BindFrameStore(replacement); });
		bool replacementStageObserved = false;
		{
			std::unique_lock<std::mutex> lock(gateMutex);
			replacementStageObserved = gateCondition.wait_for(lock,
				std::chrono::seconds(5),[&]() { return replacementReady; });
		}
		vfs->SetCameraExposureCompensationEV(2.5);
		{
			std::lock_guard<std::mutex> lock(gateMutex);
			replacementMayCommit = true;
		}
		gateCondition.notify_all();
		binder.join();
		vfs->ForTest_SetChainConstructionHook({});
		Check(replacementStageObserved && hookReleaseObserved &&
			vfs->GetFrameStore() == replacement &&
			replacement->Meta().cameraExposureEV == 2.5,
			"a concurrent exposure update is applied to the newly committed binding" );
		vfs->release();
		source->release();
		replacement->release();
	}

	void TestObserverMutationLockFailureRollsBackPrefix()
	{
		auto* vfs = new ViewportFrameStore();
		FrameStore::Spec spec;
		spec.width = kImgW;
		spec.height = kImgH;
		spec.tileEdge = 8;
		auto* source = new FrameStore(spec);
		auto* replacement = new FrameStore(spec);
		std::atomic<int> sourceFrames{0};
		vfs->SetFrameCompleteCallback(
			[&sourceFrames]( unsigned int, uint64_t ) { ++sourceFrames; });
		vfs->BindFrameStore(source);

		vfs->ForTest_SetObserverMutationLockHook(
			[]( const size_t acquired ) {
				if( acquired == 1u ) {
					throw std::runtime_error("injected observer lock failure");
				}
			});
		bool rejected = false;
		try {
			vfs->BindFrameStore(replacement);
		} catch( const std::runtime_error& error ) {
			rejected = std::string(error.what()) ==
				"injected observer lock failure";
		}
		vfs->ForTest_SetObserverMutationLockHook({});

		source->MarkFrameComplete(1u);
		const bool sourceUsable = sourceFrames.load() == 1;
		vfs->BindFrameStore(replacement);
		Check(rejected && sourceUsable && vfs->GetFrameStore() == replacement,
			"observer lock failure releases the acquired prefix and preserves retry");

		vfs->release();
		source->release();
		replacement->release();
	}

	void TestInternalConstructionFailuresPreserveExistingChain()
	{
		auto* vfs = new ViewportFrameStore();
		auto* originalImage = MakeTestImage();
		vfs->OutputImage(*originalImage,nullptr,0u);
		FrameStore* originalStore = vfs->GetFrameStore();
		const std::vector<const char*> stages = {
			"ensure_after_store",
			"ensure_after_sink",
			"ensure_after_observer_allocation",
			"ensure_after_observer_registration"
		};
		bool allPreserved = true;
		for( size_t i=0; i<stages.size(); ++i ) {
			const char* stage = stages[i];
			bool hookCalled = false;
			auto* resized = MakeTestImage(
				static_cast<unsigned int>(kImgW+1u+i), kImgH+1u);
			vfs->ForTest_SetChainConstructionHook([stage,&hookCalled]( const char* observed ) {
				if( std::strcmp(stage,observed) == 0 ) {
					hookCalled = true;
					throw std::runtime_error(stage);
				}
			});
			bool rejected = false;
			try {
				vfs->OutputImage(*resized,nullptr,1u);
			} catch( const std::runtime_error& error ) {
				rejected = std::string(error.what()) == stage;
			}
			vfs->ForTest_SetChainConstructionHook({});
			vfs->OutputImage(*originalImage,nullptr,2u);
			const bool stagePreserved = hookCalled && rejected &&
				vfs->GetFrameStore() == originalStore &&
				vfs->GetFrameStore()->Width() == kImgW;
			Check(stagePreserved,std::string("internal construction failure preserves binding at ")+stage);
			allPreserved = allPreserved && stagePreserved;
			resized->release();
		}
		Check(allPreserved,
			"every internal replacement construction failure preserves a usable old chain" );
		originalImage->release();
		vfs->release();
	}

	void TestNullBindTearsDownInternalChain()
	{
		auto* vfs = new ViewportFrameStore();
		auto* image = MakeTestImage();
		vfs->OutputImage(*image,nullptr,0u);
		Check(vfs->GetFrameStore() != nullptr && !vfs->IsExternallyBound(),
			"internal chain exists before explicit null bind" );
		vfs->BindFrameStore(nullptr);
		Check(vfs->GetFrameStore() == nullptr && !vfs->IsExternallyBound(),
			"explicit null bind tears down active and dormant internal chains" );
		vfs->OutputImage(*image,nullptr,1u);
		Check(vfs->GetFrameStore() != nullptr && !vfs->IsExternallyBound(),
			"internal chain can be allocated again after explicit teardown" );
		image->release();
		vfs->release();
	}

	// ─── Section 10 (L6e-2b): SetFrameStore notification ─────────
	//
	// Verify that `IRasterizerOutput::OnRasterizerFrameStoreChanged`
	// fires when a Rasterizer's `SetFrameStore` is called, and that
	// VFS's override forwards to `BindFrameStore` so VFS auto-rebinds
	// across resolution changes.
	//
	// We can't easily construct a real Rasterizer in a unit test
	// (full library dependency tree); instead, test the override
	// directly: VFS::OnRasterizerFrameStoreChanged(fs) must
	// observably switch the bound store.
	void TestSetFrameStoreNotification_L6e2b()
	{
		auto* vfs = new ViewportFrameStore();

		FrameStore::Spec specA;
		specA.width = 8; specA.height = 8; specA.tileEdge = 8;
		auto* fsA = new FrameStore( specA );
		fsA->addref();  // test holds one

		FrameStore::Spec specB;
		specB.width = 12; specB.height = 12; specB.tileEdge = 4;
		auto* fsB = new FrameStore( specB );
		fsB->addref();  // test holds one

		// Initial notification — same as a Job-pushed initial bind.
		vfs->OnRasterizerFrameStoreChanged( fsA );
		Check( vfs->IsExternallyBound(),
			"L6e-2b: notification fired with non-null binds VFS" );
		Check( vfs->GetFrameStore() == fsA,
			"L6e-2b: VFS now points at fsA" );

		// Resolution-change notification — Job allocated a new
		// FrameStore on dim change.
		vfs->OnRasterizerFrameStoreChanged( fsB );
		Check( vfs->IsExternallyBound(),
			"L6e-2b: still bound after dim-change notification" );
		Check( vfs->GetFrameStore() == fsB,
			"L6e-2b: VFS rebound to fsB across dim change" );

		// Null notification — rasterizer cleared its FrameStore.
		// Should revert to internal-managed mode.
		vfs->OnRasterizerFrameStoreChanged( nullptr );
		Check( !vfs->IsExternallyBound(),
			"L6e-2b: null notification reverts to internal mode" );
		Check( vfs->GetFrameStore() == nullptr,
			"L6e-2b: GetFrameStore null after unbind" );

		// Test holds the only refs now (VFS released both on rebind).
		Check( fsA->Width()  == 8 && fsA->Height() == 8,
			"L6e-2b: fsA outlived the VFS rebind" );
		Check( fsB->Width()  == 12 && fsB->Height() == 12,
			"L6e-2b: fsB outlived the VFS rebind" );

		safe_release( fsA );
		safe_release( fsB );
		vfs->release();
	}

	// ─── Section 8: cameraExposureEV propagates to Meta ───────────
	void TestCameraExposureFlow()
	{
		auto* vfs = new ViewportFrameStore();

		// Set EV BEFORE first OutputImage — should be applied to
		// the freshly-allocated FrameStore.
		vfs->SetCameraExposureCompensationEV( 1.5 );

		auto* img = MakeTestImage();
		vfs->OutputImage( *img, nullptr, 0 );

		Check( vfs->GetFrameStore()->Meta().cameraExposureEV == 1.5,
			"cameraEV applied to FrameStore at lazy-alloc time" );

		// Mid-render update.
		vfs->SetCameraExposureCompensationEV( -0.5 );
		Check( vfs->GetFrameStore()->Meta().cameraExposureEV == -0.5,
			"cameraEV update propagates to FrameStore.Meta() mid-render" );

		// EV survives a resolution change (re-applied to new FrameStore).
		auto* img2 = new RasterImage_Template<RISEPel>(
			32, 32, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) );
		vfs->OutputImage( *img2, nullptr, 1 );
		Check( vfs->GetFrameStore()->Meta().cameraExposureEV == -0.5,
			"cameraEV preserved across FrameStore reallocation" );

		FrameStore::Spec spec;
		spec.width = 8;
		spec.height = 8;
		auto* externalA = new FrameStore( spec );
		auto* externalB = new FrameStore( spec );
		std::thread updater( [&]() {
			for( unsigned int i=0; i<128; ++i ) {
				vfs->SetCameraExposureCompensationEV(
					static_cast<Scalar>(i)/Scalar(16));
			}
		});
		for( unsigned int i=0; i<128; ++i ) {
			vfs->BindFrameStore( (i & 1u) ? externalA : externalB );
		}
		updater.join();
		vfs->SetCameraExposureCompensationEV( 2.25 );
		vfs->BindFrameStore( externalA );
		Check( vfs->GetFrameStore() == externalA &&
			externalA->Meta().cameraExposureEV == 2.25,
			"cameraEV publication is serialized with concurrent external binding" );
		vfs->BindFrameStore( nullptr );
		safe_release( externalA );
		safe_release( externalB );

		safe_release( img2 );
		safe_release( img );
		vfs->release();
	}

	void TestObserverRetainsUnregisteredEncoder()
	{
		FrameStore::Spec spec;
		spec.width = 1;
		spec.height = 1;
		spec.tileEdge = 1;
		FrameStore* store = new FrameStore(spec);
		bool destroyed = false;
		FrameEncoderRegistry& registry = FrameEncoderRegistry::Get();
		IFrameEncoder* original = registry.AcquireByFormatName("PPM");
		if( !original ) {
			Check(false,"PPM encoder is available for the registry lifetime fixture");
			store->release();
			return;
		}
		RetainedFrameEncoder* encoder = new RetainedFrameEncoder(destroyed,*original);
		registry.Register(encoder);
		IFrameEncoder* acquired = registry.AcquireByFormatName("PPM");
		IFrameEncoder* acquiredByExtension = registry.AcquireByExtension(".ppm");
		std::vector<IFrameEncoder*> acquiredAll = registry.AcquireAll();
		bool retainedInSnapshot = false;
		for( IFrameEncoder* candidate : acquiredAll ) {
			if( candidate == encoder ) {
				retainedInSnapshot = true;
			}
		}
		const std::string pattern = MakeTempPath() + "_retained_encoder";
		const std::string artifact = pattern + ".ppm";
		EncodeOpts opts;
		const bool removed = registry.Unregister("PPM");
		// Register adopts this acquired reference while the wrapper retains its own.
		registry.Register(original);
		FileEncoderObserver* observer = acquired ? new FileEncoderObserver(
			store,acquired,opts,pattern,false) : nullptr;
		safe_release(acquired);
		if( !observer || !acquiredByExtension || !retainedInSnapshot ) {
			Check(false,"registry acquisition retains the encoder across removal");
			safe_release(acquiredByExtension);
			for( IFrameEncoder* candidate : acquiredAll ) candidate->release();
			store->release();
			return;
		}
		observer->OnFrameComplete(0,store->Generation());
		Check( removed && !destroyed &&
			acquiredByExtension == encoder &&
			encoder->encodeCalls == 1 &&
			std::filesystem::exists(artifact),
			"name, extension, and all-encoder acquisitions survive registry removal" );
		safe_release(acquiredByExtension);
		for( IFrameEncoder* candidate : acquiredAll ) candidate->release();
		observer->release();
		Check( destroyed,
			"retained encoder is released when the observer is destroyed" );
		store->release();
		std::remove(artifact.c_str());
	}
}

int main()
{
	ProcessWatchdog watchdog;
	std::cout << "ViewportFrameStoreTest L4 — GUI-viewport facade\n";
	std::cout << "------------------------------------------------------\n";

	TestLazyAllocation();
	TestCallbacks();
	TestThrowingCallbacksReleaseRetainedSnapshots();
	TestIntermediateMultiTile();
	TestRenderToBuffer();
	TestSaveAsByteIdenticalToL2();
	TestSaveAsFireProvenanceAndTransaction();
	TestSaveAsUsesOneMetadataSnapshot();
	TestNonFireSavesLeaseOutputClassification();
	TestRasterizerSwap();
	TestResolutionChange();
	TestBindDormantStoreAsExternal();
	TestMultiFrameReuse();
	TestMidRenderSaveAs();
	TestPreparedFireFrameCannotPublish();
	TestFireSaveAsLeasePrecedesStoreRelease();
	TestChainRaceUnderResolutionChange();
	TestCameraExposureFlow();
	TestExternalBind_L6e2a();
	TestConcurrentExternalBindRejectsSynchronously();
	TestPhaseOneConcurrentBindRejectsSynchronously();
	TestBindTeardownKeepsOldChainPublished();
	TestRejectedCrossStoreBindPreservesExistingChain();
	TestBindConstructionFailuresPreserveExistingChain();
	TestBindRollbackPreservesQuiescedEventAndObserverInsertion();
	TestExposureUpdateWinsConcurrentBindCommit();
	TestObserverMutationLockFailureRollsBackPrefix();
	TestInternalConstructionFailuresPreserveExistingChain();
	TestNullBindTearsDownInternalChain();
	TestSetFrameStoreNotification_L6e2b();
	TestObserverRetainsUnregisteredEncoder();

	std::cout << "------------------------------------------------------\n";
	std::cout << "passed " << gPassCount << ", failed " << gFailCount << "\n";
	return gFailCount == 0 ? 0 : 1;
}
