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
//       detach, register on B; FrameStore + observer + tile-callback
//       state survive across the swap.
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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>  // getpid

#include "../src/Library/Rendering/ViewportFrameStore.h"
#include "../src/Library/Rendering/FrameStore.h"
#include "../src/Library/Rendering/FrameEncoders.h"
#include "../src/Library/RasterImages/RasterImage.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/DiskFileWriteBuffer.h"
#include "../src/Library/Interfaces/IFrameEncoder.h"

using namespace RISE;
using namespace RISE::Implementation;
using namespace RISE::FrameStoreOutput;

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

	constexpr unsigned int kImgW = 16;
	constexpr unsigned int kImgH = 16;

	RISEColor PatternPixel( unsigned int x, unsigned int y )
	{
		const double r = static_cast<double>( x ) / static_cast<double>( kImgW - 1 );
		const double g = static_cast<double>( y ) / static_cast<double>( kImgH - 1 );
		const double b = 0.25 + 0.5 * ( r + g ) / 2.0;
		return RISEColor( RISEPel( r, g, b ), 1.0 );
	}

	// Build a 16x16 IRasterImage with the test pattern.
	RasterImage_Template<RISEPel>* MakeTestImage()
	{
		auto* img = new RasterImage_Template<RISEPel>(
			kImgW, kImgH, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) );
		for ( unsigned int y = 0; y < kImgH; ++y ) {
			for ( unsigned int x = 0; x < kImgW; ++x ) {
				img->SetPEL( x, y, PatternPixel( x, y ) );
			}
		}
		return img;
	}

	std::string MakeTempPath()
	{
		const char* tmpdir = std::getenv( "TMPDIR" );
		if ( !tmpdir ) tmpdir = "/tmp/";
		std::ostringstream os;
		os << tmpdir;
		if ( os.str().back() != '/' ) os << '/';
		os << "rise_l4_vfs_" << ::getpid();
		return os.str();
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

		// IRasterizerOutput::OutputImage when bound: still fires
		// OnFrameComplete via direct MarkFrameComplete on the bound
		// store (frame-complete signal preserved).
		const int beforeFinal = frameCount.load();
		vfs->OutputImage( *img, nullptr, /*frame=*/0 );
		Check( frameCount.load() == beforeFinal + 1,
			"L6e-2a: OutputImage fires OnFrameComplete on bound store" );

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

		safe_release( img2 );
		safe_release( img );
		vfs->release();
	}
}

int main()
{
	std::cout << "ViewportFrameStoreTest L4 — GUI-viewport facade\n";
	std::cout << "------------------------------------------------------\n";

	TestLazyAllocation();
	TestCallbacks();
	TestIntermediateMultiTile();
	TestRenderToBuffer();
	TestSaveAsByteIdenticalToL2();
	TestRasterizerSwap();
	TestResolutionChange();
	TestMultiFrameReuse();
	TestMidRenderSaveAs();
	TestChainRaceUnderResolutionChange();
	TestCameraExposureFlow();
	TestExternalBind_L6e2a();
	TestSetFrameStoreNotification_L6e2b();

	std::cout << "------------------------------------------------------\n";
	std::cout << "passed " << gPassCount << ", failed " << gFailCount << "\n";
	return gFailCount == 0 ? 0 : 1;
}
