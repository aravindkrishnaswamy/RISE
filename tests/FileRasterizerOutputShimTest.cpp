//////////////////////////////////////////////////////////////////////
//
//  FileRasterizerOutputShimTest.cpp - L3 regression gate for the
//  FileRasterizerOutput shim that routes through
//  FrameStore + FrameSink + FileEncoderObserver.
//
//  The L2 byte-identity test (FrameEncoderTest.cpp) already proves
//  IFrameEncoder produces bytes byte-identical to the legacy
//  per-format writer pipeline.  This L3 test proves the additional
//  glue — FrameSink ingest + FileEncoderObserver dispatch — does
//  NOT introduce drift between FrameStore.AsBeautyRasterImage()
//  and the source IRasterImage.
//
//  Strategy:
//    1. Build a known IRasterImage (RasterImage_Template<RISEPel>).
//    2. Pipe it through:
//        - FileRasterizerOutput → file-on-disk → bytes-A
//        - L2 IFrameEncoder + the same IRasterImage → MemoryBuffer-B
//    3. Compare bytes-A == bytes-B byte-for-byte.
//
//  If they match, the L3 shim is byte-identical to L2's encoder
//  output for every format / option combination L2 covers.  Since
//  L2 already proves byte-identity to the legacy
//  WriteImageToFile pipeline, transitivity gives us
//  FileRasterizerOutput-shim ≡ legacy bytes.
//
//////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
	#include <process.h>		// _getpid()
	#define getpid _getpid
#else
	#include <unistd.h>		// getpid() (POSIX)
#endif

#include "../src/Library/Rendering/FileRasterizerOutput.h"
#include "../src/Library/Rendering/FileEncoderObserver.h"
#include "../src/Library/Rendering/FrameEncoders.h"
#include "../src/Library/RasterImages/RasterImage.h"
#include "../src/Library/Utilities/DiskFileWriteBuffer.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/RISECBOR64.h"
#ifndef NO_EXR_SUPPORT
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

	// Same pattern shape as FrameEncoderTest, copied here because
	// the two tests must share the test fixture: any drift between
	// L2 input and L3 input would mask byte-equivalence bugs.
	RISEColor PatternPixel( unsigned int x, unsigned int y )
	{
		const double r = static_cast<double>( x ) / static_cast<double>( kImgW - 1 );
		const double g = static_cast<double>( y ) / static_cast<double>( kImgH - 1 );
		const double b = 0.25 + 0.5 * ( r + g ) / 2.0;
		double rr = r, gg = g, bb = b;
		if ( x == 3 && y == 5 )   { rr = 3.0;  gg = 1.0; bb = 0.5; }
		if ( x == 10 && y == 12 ) { rr = 0.5;  gg = 4.5; bb = 0.5; }
		return RISEColor( RISEPel( rr, gg, bb ), 1.0 );
	}

	void FillLegacyImage( RasterImage_Template<RISEPel>& img )
	{
		for ( unsigned int y = 0; y < kImgH; ++y ) {
			for ( unsigned int x = 0; x < kImgW; ++x ) {
				img.SetPEL( x, y, PatternPixel( x, y ) );
			}
		}
	}

	// Read a file's bytes into a vector.
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

	// Forward declaration — defined later in the namespace.
	std::string MakeTempPathWithoutExt();

	// Run the L2 IFrameEncoder for `format` with `opts` against
	// `img`, capturing the bytes into `out`.
	//
	// IMPORTANT: writes via DiskFileWriteBuffer (NOT MemoryBuffer)
	// so the captured byte stream matches the on-disk byte stream
	// the shim produces.  EXR and TIFF writers `seekp` backwards to
	// patch header offset tables; on a MemoryBuffer the cursor ends
	// at a position BEFORE the highest-written byte, so
	// `getCurPos()`-based capture would miss the trailing bytes.
	// Reading back the file gives the correct full byte stream
	// (the OS preserves all bytes written, regardless of seek
	// pattern).
	void EncodeViaL2( const std::string& format,
	                  const EncodeOpts&  opts,
	                  const std::string& ext,
	                  const RasterImage_Template<RISEPel>& img,
	                  std::vector<unsigned char>& out,
	                  double cameraExposureEV = 0.0 )
	{
		// Build a transient FrameStore + populate from img.
		FrameStore::Spec spec;
		spec.width    = kImgW;
		spec.height   = kImgH;
		spec.tileEdge = 32;
		FrameStore* store = new FrameStore( spec );

		auto* beauty = store->GetChannel<ChannelId::Beauty>();
		auto* alpha  = store->GetChannel<ChannelId::Alpha>();
		store->BeginTile( 0, 0 );
		for ( unsigned int y = 0; y < kImgH; ++y ) {
			for ( unsigned int x = 0; x < kImgW; ++x ) {
				const RISEColor c = img.GetPEL( x, y );
				beauty->At( x, y ) = c.base;
				alpha->At( x, y )  = c.a;
			}
		}
		store->EndTile( 0, 0 );

		// L3 shim feeds cameraExposureEV through FrameStore::Meta().
		// We mirror that wiring so the L2 path sees the same total EV.
		store->SetCameraExposureEV(cameraExposureEV);

		// Write to a temp file (parallel to the shim's path) so byte
		// capture is symmetric: both paths produce on-disk byte
		// streams that we read back into vectors.
		const std::string path = MakeTempPathWithoutExt() + "_l2." + ext;
		DiskFileWriteBuffer* buf = new DiskFileWriteBuffer( path.c_str() );
		IFrameEncoder* enc = FrameEncoderRegistry::Get().ByFormatName( format );
		if ( enc && buf->ReadyToWrite() ) {
			enc->Encode( *store, *buf, opts );
		}
		safe_release( buf );

		ReadFileAllBytes( path, out );
		std::remove( path.c_str() );

		store->release();
	}

	// Build a temp filename in the OS tmp dir.  Use a fixed prefix +
	// random suffix per run to avoid collisions across parallel tests.
	//
	// The path returned here is ABSOLUTE ($TMPDIR on macOS is typically
	// /var/folders/..., not "/tmp/").  EncodeViaShim below feeds this
	// straight to FileRasterizerOutput's constructor, which (per
	// global.options' checked-in `rendered_output_in_rise_media_folder
	// TRUE`) unconditionally does
	// prepends RISE_MEDIA_PATH to the authored pattern, with no check
	// for whether that pattern is already absolute.  On any dev/CI box
	// that has exported RISE_MEDIA_PATH
	// per the Quickstart (`export RISE_MEDIA_PATH="$(pwd)/"`), that
	// turns our absolute temp path into a bogus double-rooted one
	// (e.g. ".../RISE//var/folders/...") that doesn't exist on disk,
	// so the write fails and FileRasterizerOutput falls back to its
	// emergency `fro_temp_*` writer in the CURRENT directory — see
	// main()'s save/restore of RISE_MEDIA_PATH, which neutralizes the
	// concatenation for the lifetime of this test instead.
	std::string MakeTempPathWithoutExt()
	{
		const char* tmpdir = std::getenv( "TMPDIR" );
		if ( !tmpdir ) tmpdir = "/tmp/";
		std::ostringstream os;
		os << tmpdir;
		if ( os.str().back() != '/' ) os << '/';
		os << "rise_l3_shim_" << ::getpid();
		return os.str();
	}

	// Drive the FileRasterizerOutput shim to write a file with the
	// given format + opts, then read the bytes back.  bMultiple=false,
	// frame=0 → "<pattern>.<ext>" filename.
	void EncodeViaShim( FileRasterizerOutput::FRO_TYPE type,
	                    unsigned char bpp,
	                    COLOR_SPACE colorSpace,
	                    Scalar exposureEV,
	                    DISPLAY_TRANSFORM toneCurve,
	                    EXR_COMPRESSION exrCompression,
	                    bool exrWithAlpha,
	                    const std::string& ext,
	                    const RasterImage_Template<RISEPel>& img,
	                    std::vector<unsigned char>& out )
	{
		const std::string pathNoExt = MakeTempPathWithoutExt();
		const std::string fullPath  = pathNoExt + "." + ext;

		// Construct the shim.
		auto* fro = new FileRasterizerOutput(
			pathNoExt.c_str(),
			false,           // bMultiple
			type,
			bpp,
			colorSpace,
			exposureEV,
			toneCurve,
			exrCompression,
			exrWithAlpha );

		// Drive OutputImage (frame=0).  This goes through
		// EnsureChain → FrameSink::OutputImage → FrameStore →
		// FileEncoderObserver::OnFrameComplete → DiskFileWriteBuffer.
		fro->OutputImage( img, nullptr, 0 );

		// Tear down the shim — flushes the disk buffer.
		fro->release();

		// Read the bytes back.
		if ( !ReadFileAllBytes( fullPath, out ) ) {
			std::cerr << "  Could not read back file " << fullPath << "\n";
			out.clear();
		}

		// Best-effort cleanup.
		std::remove( fullPath.c_str() );
	}

	// Format-name string for FRO_TYPE — duplicated from
	// FileRasterizerOutput.cpp's anonymous helper since that one
	// isn't exported.
	const char* FormatName( FileRasterizerOutput::FRO_TYPE t )
	{
		switch ( t ) {
			case FileRasterizerOutput::TGA:   return "TGA";
			case FileRasterizerOutput::PPM:   return "PPM";
			case FileRasterizerOutput::PNG:   return "PNG";
			case FileRasterizerOutput::HDR:   return "HDR";
			case FileRasterizerOutput::TIFF:  return "TIFF";
			case FileRasterizerOutput::RGBEA: return "RGBEA";
			case FileRasterizerOutput::EXR:   return "EXR";
		}
		return "PNG";
	}

	void DiffOneCase(
		FileRasterizerOutput::FRO_TYPE type,
		unsigned char bpp,
		COLOR_SPACE colorSpace,
		Scalar exposureEV,
		DISPLAY_TRANSFORM toneCurve,
		EXR_COMPRESSION exrCompression,
		bool exrWithAlpha,
		const std::string& ext,
		const std::string& label )
	{
		// Build the legacy image once per case.
		auto* img = new RasterImage_Template<RISEPel>(
			kImgW, kImgH, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) );
		FillLegacyImage( *img );

		// Drive the shim → file → bytes.
		std::vector<unsigned char> shimBytes;
		EncodeViaShim( type, bpp, colorSpace, exposureEV, toneCurve,
		               exrCompression, exrWithAlpha, ext, *img, shimBytes );

		// Drive L2 directly → MemoryBuffer → bytes.
		EncodeOpts opts;
		opts.colorSpace     = colorSpace;
		opts.bpp            = bpp;
		opts.exrCompression = exrCompression;
		opts.exrWithAlpha   = exrWithAlpha;
		opts.viewTransform.exposureEV = static_cast<float>( exposureEV );
		opts.viewTransform.toneCurve  = toneCurve;
		std::vector<unsigned char> l2Bytes;
		EncodeViaL2( FormatName( type ), opts, ext, *img, l2Bytes );

		std::ostringstream tag;
		tag << "[" << label << "]";

		Check( !shimBytes.empty(), tag.str() + " shim wrote non-empty file" );
		Check( !l2Bytes.empty(),   tag.str() + " L2 produced non-empty bytes" );

		const bool sizeOk = shimBytes.size() == l2Bytes.size();
		std::ostringstream sizeLabel;
		sizeLabel << tag.str() << " shim size matches L2 (shim=" << shimBytes.size()
		          << " L2=" << l2Bytes.size() << ")";
		Check( sizeOk, sizeLabel.str() );

		if ( sizeOk ) {
			bool eq = true;
			size_t firstDiff = 0;
			for ( size_t i = 0; i < shimBytes.size(); ++i ) {
				if ( shimBytes[i] != l2Bytes[i] ) {
					eq = false;
					firstDiff = i;
					break;
				}
			}
			std::ostringstream eqLabel;
			eqLabel << tag.str() << " shim bytes byte-identical to L2";
			if ( !eq ) eqLabel << " (first diff at offset " << firstDiff << ")";
			Check( eq, eqLabel.str() );
		}

		safe_release( img );
	}

	void TestAllFormats()
	{
		// PNG default
		DiffOneCase( FileRasterizerOutput::PNG, 8, eColorSpace_sRGB,
		             0.0, eDisplayTransform_None, eExrCompression_Piz, true,
		             "png", "PNG default" );

		// PNG with ACES tone curve
		DiffOneCase( FileRasterizerOutput::PNG, 8, eColorSpace_sRGB,
		             0.5, eDisplayTransform_ACES, eExrCompression_Piz, true,
		             "png", "PNG +0.5EV+ACES" );

		// PNG 16bpp
		DiffOneCase( FileRasterizerOutput::PNG, 16, eColorSpace_sRGB,
		             0.0, eDisplayTransform_None, eExrCompression_Piz, true,
		             "png", "PNG 16bpp" );

		// EXR PIZ
		DiffOneCase( FileRasterizerOutput::EXR, 8, eColorSpace_Rec709RGB_Linear,
		             0.0, eDisplayTransform_None, eExrCompression_Piz, true,
		             "exr", "EXR PIZ" );

		// EXR ZIP
		DiffOneCase( FileRasterizerOutput::EXR, 8, eColorSpace_Rec709RGB_Linear,
		             0.0, eDisplayTransform_None, eExrCompression_Zip, true,
		             "exr", "EXR ZIP" );

		// HDR
		DiffOneCase( FileRasterizerOutput::HDR, 8, eColorSpace_Rec709RGB_Linear,
		             0.0, eDisplayTransform_None, eExrCompression_Piz, true,
		             "hdr", "HDR Radiance" );

		// RGBEA
		DiffOneCase( FileRasterizerOutput::RGBEA, 8, eColorSpace_Rec709RGB_Linear,
		             0.0, eDisplayTransform_None, eExrCompression_Piz, true,
		             "rgbea", "RGBEA" );

#ifndef NO_TIFF_SUPPORT
		// TIFF — skipped under NO_TIFF_SUPPORT; the TIFFWriter is a
		// stub that writes 0 bytes, so DiffOneCase's "shim wrote non-empty
		// file" + "L2 produced non-empty bytes" Checks would fail.
		DiffOneCase( FileRasterizerOutput::TIFF, 8, eColorSpace_sRGB,
		             0.0, eDisplayTransform_None, eExrCompression_Piz, true,
		             "tiff", "TIFF default" );

		// TIFF with Reinhard tone curve
		DiffOneCase( FileRasterizerOutput::TIFF, 8, eColorSpace_sRGB,
		             0.0, eDisplayTransform_Reinhard, eExrCompression_Piz, true,
		             "tiff", "TIFF +Reinhard" );
#endif

		// TGA
		DiffOneCase( FileRasterizerOutput::TGA, 8, eColorSpace_sRGB,
		             0.0, eDisplayTransform_None, eExrCompression_Piz, true,
		             "tga", "TGA" );

		// PPM
		DiffOneCase( FileRasterizerOutput::PPM, 8, eColorSpace_sRGB,
		             0.0, eDisplayTransform_None, eExrCompression_Piz, true,
		             "ppm", "PPM" );
	}

	// Sanity: verify SetCameraExposureCompensationEV propagates to
	// the FrameStore so encoders see it on the next frame.
	void TestCameraExposurePropagation()
	{
		auto* img = new RasterImage_Template<RISEPel>(
			kImgW, kImgH, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) );
		FillLegacyImage( *img );

		const std::string pathNoExt = MakeTempPathWithoutExt() + "_camev";
		const std::string fullPath  = pathNoExt + ".png";

		auto* fro = new FileRasterizerOutput(
			pathNoExt.c_str(), false, FileRasterizerOutput::PNG,
			8, eColorSpace_sRGB, /*staticEV=*/0.5, eDisplayTransform_None,
			eExrCompression_Piz, true );

		// Set camera EV BEFORE OutputImage — chain is allocated
		// inside OutputImage's EnsureChain.  cameraEV = 1.0,
		// staticEV = 0.5 → totalEV = 1.5.
		fro->SetCameraExposureCompensationEV( 1.0 );
		fro->OutputImage( *img, nullptr, 0 );
		fro->release();

		std::vector<unsigned char> shimBytes;
		ReadFileAllBytes( fullPath, shimBytes );

		// Compare to L2 path with staticEV=0.5 + cameraEV=1.0.
		// The encoder reads cameraEV from store.Meta() and adds it
		// to opts.viewTransform.exposureEV, so we pass them
		// separately via the cameraExposureEV parameter.
		EncodeOpts opts;
		opts.colorSpace = eColorSpace_sRGB;
		opts.bpp = 8;
		opts.viewTransform.exposureEV = 0.5f;
		opts.viewTransform.toneCurve  = eDisplayTransform_None;
		std::vector<unsigned char> l2Bytes;
		EncodeViaL2( "PNG", opts, "png", *img, l2Bytes, /*cameraEV=*/1.0 );

		Check( shimBytes.size() == l2Bytes.size(),
			"[CameraEV] shim size matches L2 with totalEV=1.5" );
		bool eq = ( shimBytes == l2Bytes );
		Check( eq, "[CameraEV] shim bytes match L2 (camera+static EV summed)" );

		std::remove( fullPath.c_str() );
		safe_release( img );
	}

	// L3 adversarial review M3 — coverage gaps:
	//   (a) OutputPreDenoisedImage / OutputDenoisedImage suffix routing
	//   (b) animation mode (bMultiple=true) frame-numbered filenames
	//   (c) multi-frame reuse — two OutputImage calls on the same
	//       FileRasterizerOutput instance produce two valid files
	//       and the FrameStore correctly refills between them

	void TestDenoiseDualWrite()
	{
		// OutputPreDenoisedImage writes to "<pattern>.png" (no suffix).
		// OutputDenoisedImage writes to "<pattern>_denoised.png".
		// Both files are produced by the same FileRasterizerOutput
		// instance: the suffix discrimination happens via observer
		// callback type (OnPreDenoiseComplete vs OnDenoiseComplete).
		auto* img = new RasterImage_Template<RISEPel>(
			kImgW, kImgH, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) );
		FillLegacyImage( *img );

		// Use a slightly modified second image to verify the denoised
		// file actually carries the SECOND image's content (not the
		// first).
		auto* imgD = new RasterImage_Template<RISEPel>(
			kImgW, kImgH, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) );
		for ( unsigned int y = 0; y < kImgH; ++y ) {
			for ( unsigned int x = 0; x < kImgW; ++x ) {
				const RISEColor c = PatternPixel( x, y );
				// Slight perturbation: scale R by 0.5 so the
				// denoised file is byte-distinguishable from the
				// pre-denoise file.
				imgD->SetPEL( x, y, RISEColor(
					RISEPel( c.base.r * 0.5, c.base.g, c.base.b ), c.a ) );
			}
		}

		const std::string base = MakeTempPathWithoutExt() + "_denoise_dual";
		const std::string preFile  = base + ".png";
		const std::string postFile = base + "_denoised.png";

		auto* fro = new FileRasterizerOutput(
			base.c_str(), false, FileRasterizerOutput::PNG,
			8, eColorSpace_sRGB, 0.0, eDisplayTransform_None,
			eExrCompression_Piz, true );

		fro->OutputPreDenoisedImage( *img,  nullptr, 0 );
		fro->OutputDenoisedImage(    *imgD, nullptr, 0 );
		fro->release();

		std::vector<unsigned char> preBytes, postBytes;
		const bool preOk  = ReadFileAllBytes( preFile,  preBytes  );
		const bool postOk = ReadFileAllBytes( postFile, postBytes );

		Check( preOk  && !preBytes.empty(),
			"[denoise dual] pre-denoise file written to <pattern>.png" );
		Check( postOk && !postBytes.empty(),
			"[denoise dual] denoised file written to <pattern>_denoised.png" );
		Check( preBytes != postBytes,
			"[denoise dual] pre and post files differ (post carries 2nd image)" );

		// Compare each to L2 path.
		EncodeOpts opts;
		opts.colorSpace = eColorSpace_sRGB;
		opts.bpp        = 8;
		opts.viewTransform = ViewTransform::Identity();
		std::vector<unsigned char> l2Pre, l2Post;
		EncodeViaL2( "PNG", opts, "png", *img,  l2Pre  );
		EncodeViaL2( "PNG", opts, "png", *imgD, l2Post );

		Check( preBytes  == l2Pre,
			"[denoise dual] pre-denoise file byte-identical to L2 (img)" );
		Check( postBytes == l2Post,
			"[denoise dual] denoised file byte-identical to L2 (imgD)" );

		std::remove( preFile.c_str() );
		std::remove( postFile.c_str() );
		safe_release( imgD );
		safe_release( img );
	}

	void TestAnimationFrameNumbering()
	{
		// bMultiple = true → filename templated as
		// "<pattern>NNNN.<ext>" (4-digit frame number).
		auto* img = new RasterImage_Template<RISEPel>(
			kImgW, kImgH, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) );
		FillLegacyImage( *img );

		const std::string base = MakeTempPathWithoutExt() + "_anim";
		auto* fro = new FileRasterizerOutput(
			base.c_str(), /*bMultiple=*/true, FileRasterizerOutput::PNG,
			8, eColorSpace_sRGB, 0.0, eDisplayTransform_None,
			eExrCompression_Piz, true );

		// Render frames 0, 1, 7 (non-contiguous to verify the
		// frame number actually drives the filename).
		fro->OutputImage( *img, nullptr, 0 );
		fro->OutputImage( *img, nullptr, 1 );
		fro->OutputImage( *img, nullptr, 7 );
		fro->release();

		const std::string f0 = base + "0000.png";
		const std::string f1 = base + "0001.png";
		const std::string f7 = base + "0007.png";

		std::vector<unsigned char> b0, b1, b7;
		const bool ok0 = ReadFileAllBytes( f0, b0 );
		const bool ok1 = ReadFileAllBytes( f1, b1 );
		const bool ok7 = ReadFileAllBytes( f7, b7 );

		Check( ok0 && !b0.empty(), "[anim] frame 0000 file present" );
		Check( ok1 && !b1.empty(), "[anim] frame 0001 file present" );
		Check( ok7 && !b7.empty(), "[anim] frame 0007 file present" );
		// All three frames have identical input image, so all three
		// files should be byte-identical (PNG is deterministic for
		// identical input + writer state).
		Check( b0 == b1 && b1 == b7,
			"[anim] all three frame files byte-identical (same input)" );

		std::remove( f0.c_str() );
		std::remove( f1.c_str() );
		std::remove( f7.c_str() );
		safe_release( img );
	}

	void TestLongOutputPattern()
	{
		const std::string longPattern(4096u,'x');
		FileRasterizerOutput* output = new FileRasterizerOutput(
			longPattern.c_str(),false,FileRasterizerOutput::TGA,8,
			eColorSpace_sRGB,0.0,eDisplayTransform_None,
			eExrCompression_Zip,true);
		Check( output->HasEncoder(),
			"output patterns longer than the legacy 1024-byte buffer construct safely" );
		safe_release(output);
	}

	void TestMultiFrameReuse()
	{
		// Two OutputImage calls on the same FileRasterizerOutput
		// instance: chain is allocated on the first call, reused
		// on the second.  Verify the FrameStore is correctly
		// refilled (not stale) between calls.
		auto* img1 = new RasterImage_Template<RISEPel>(
			kImgW, kImgH, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) );
		FillLegacyImage( *img1 );

		auto* img2 = new RasterImage_Template<RISEPel>(
			kImgW, kImgH, RISEColor( RISEPel( 1.0, 0.0, 0.0 ), 1.0 ) );
		// img2 is solid red; verify the second-frame file reflects
		// that, not stale img1 pixels.

		const std::string base = MakeTempPathWithoutExt() + "_reuse";
		auto* fro = new FileRasterizerOutput(
			base.c_str(), false, FileRasterizerOutput::PNG,
			8, eColorSpace_sRGB, 0.0, eDisplayTransform_None,
			eExrCompression_Piz, true );

		// First call writes <base>.png with img1's content.
		fro->OutputImage( *img1, nullptr, 0 );
		std::vector<unsigned char> firstWrite;
		ReadFileAllBytes( base + ".png", firstWrite );

		// Second call writes <base>.png with img2's content,
		// overwriting the first.
		fro->OutputImage( *img2, nullptr, 0 );
		std::vector<unsigned char> secondWrite;
		ReadFileAllBytes( base + ".png", secondWrite );

		fro->release();

		Check( !firstWrite.empty(),  "[reuse] first OutputImage wrote a file" );
		Check( !secondWrite.empty(), "[reuse] second OutputImage wrote a file" );
		Check( firstWrite != secondWrite,
			"[reuse] second-frame file differs from first (FrameStore was refilled)" );

		// Verify second matches L2(img2).
		EncodeOpts opts;
		opts.colorSpace = eColorSpace_sRGB;
		opts.bpp        = 8;
		opts.viewTransform = ViewTransform::Identity();
		std::vector<unsigned char> l2Bytes;
		EncodeViaL2( "PNG", opts, "png", *img2, l2Bytes );
		Check( secondWrite == l2Bytes,
			"[reuse] second-frame bytes byte-identical to L2(img2)" );

		std::remove( ( base + ".png" ).c_str() );
		safe_release( img2 );
		safe_release( img1 );
	}

	// Sanity: HDR formats must zero out cameraEV per
	// FileRasterizerOutput.cpp:141 — verify by setting a
	// non-zero camera EV and confirming EXR bytes match L2 with
	// totalEV = 0.
	void TestHDRZerosCameraEV()
	{
		auto* img = new RasterImage_Template<RISEPel>(
			kImgW, kImgH, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) );
		FillLegacyImage( *img );

		const std::string pathNoExt = MakeTempPathWithoutExt() + "_hdr_camev";
		const std::string fullPath  = pathNoExt + ".exr";

		auto* fro = new FileRasterizerOutput(
			pathNoExt.c_str(), false, FileRasterizerOutput::EXR,
			8, eColorSpace_Rec709RGB_Linear, /*staticEV=*/0.0, eDisplayTransform_None,
			eExrCompression_Piz, true );

		// Setting camera EV on an HDR FileRasterizerOutput should
		// be ignored (zeroed inside SetCameraExposureCompensationEV).
		fro->SetCameraExposureCompensationEV( 2.0 );
		fro->OutputImage( *img, nullptr, 0 );
		fro->release();

		std::vector<unsigned char> shimBytes;
		ReadFileAllBytes( fullPath, shimBytes );

		EncodeOpts opts;
		opts.colorSpace     = eColorSpace_Rec709RGB_Linear;
		opts.exrCompression = eExrCompression_Piz;
		opts.exrWithAlpha   = true;
		// totalEV = 0 because HDR zeros the camera EV.
		opts.viewTransform.exposureEV = 0.0f;
		opts.viewTransform.toneCurve  = eDisplayTransform_None;
		std::vector<unsigned char> l2Bytes;
		EncodeViaL2( "EXR", opts, "exr", *img, l2Bytes, /*cameraEV=*/0.0 );

		const bool eq = ( shimBytes == l2Bytes );
		Check( eq, "[HDR] camera EV zeroed for HDR formats (matches L2 totalEV=0)" );

		std::remove( fullPath.c_str() );
		safe_release( img );
	}

	FrameStore* MakeFireFidelityStore()
	{
		FrameStore::Spec spec;
		spec.width = kImgW;
		spec.height = kImgH;
		spec.tileEdge = 32;
		FrameStore* store = new FrameStore(spec);
		auto* beauty = store->GetChannel<ChannelId::Beauty>();
		auto* alpha = store->GetChannel<ChannelId::Alpha>();
		store->BeginTile(0,0);
		for( unsigned int y=0; y<kImgH; ++y ) {
			for( unsigned int x=0; x<kImgW; ++x ) {
				const RISEColor c = PatternPixel(x,y);
				beauty->At(x,y) = c.base;
				alpha->At(x,y) = c.a;
			}
		}
		store->EndTile(0,0);
		using RISECBOR64::Value;
		RISECBOR64::Bytes configBytes;
		RISECBOR64::Bytes buildBytes;
		std::string encodeError;
		RISECBOR64::Encode(Value::MapValue({
			{ "aov", Value::MapValue({}) }, { "camera", Value::MapValue({}) },
			{ "clamp", Value::MapValue({}) }, { "depth", Value::MapValue({}) },
			{ "film", Value::MapValue({}) }, { "filter", Value::MapValue({}) },
			{ "integrator", Value::MapValue({}) },
			{ "record_kind", Value::String("resolved_render_configuration_v1") },
			{ "sampler", Value::MapValue({}) }, { "schema_version", Value::Unsigned(1) }
		}),configBytes,&encodeError);
		RISECBOR64::Encode(Value::MapValue({
			{ "record_kind", Value::String("renderer_build_v1") },
			{ "schema_version", Value::Unsigned(1) },
			{ "source_revision", Value::String("test-build") }
		}),buildBytes,&encodeError);
		FrameStoreOutput::ActiveFireMedium medium;
		medium.mediaKind = "static_authored";
		medium.managerName = "fire";
		medium.bindingKind = "global_medium";
		medium.bindingOwner = "scene";
		medium.authoredConfigDigest =
			"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
		medium.opticalRecordIds = {
			"2cdd00456431fd0c020ee8e28b01bc59e92586beb6ac8f6ea77efa31276ad137" };
		store->SetFireFidelityMetadata("preview",
			{ "pel_transport", "producer_unqualified", "requested_preview" },
			medium.opticalRecordIds,{ medium },configBytes,buildBytes,
			RISECBOR64::SHA256Hex(buildBytes));
		return store;
	}

	void TestFireFidelityProvenanceOutput()
	{
		FrameStore* store = MakeFireFidelityStore();
		EncodeOpts opts;
#ifndef NO_EXR_SUPPORT
		const std::string exrBase = MakeTempPathWithoutExt()+"_fire_provenance";
		const std::string exrFile = exrBase+".exr";
		const std::string exrSidecar = exrFile+".provenance.cbor";
		opts.colorSpace = eColorSpace_Rec709RGB_Linear;
		opts.bpp = 32;
		opts.includeAOVs = true;
		opts.aovChannels = { FrameStoreOutput::ChannelId::Albedo };
		opts.attrs.push_back(std::make_pair("authoringNote","ratchet"));
		IFrameEncoder* exr = FrameEncoderRegistry::Get().ByFormatName("EXR");
		FileEncoderObserver* observer = new FileEncoderObserver(store,exr,opts,exrBase,false);
		store->AddObserver(observer);
		store->MarkFrameComplete(0);
		store->RemoveObserver(observer);
		safe_release(observer);
		std::vector<unsigned char> exrBytes, strippedBytes, exrSidecarBytes;
		RISECBOR64::Value exrEnvelope;
		std::string decodeError;
		const bool exrDecoded = ReadFileAllBytes(exrFile,exrBytes) &&
			ReadFileAllBytes(exrSidecar,exrSidecarBytes) &&
			RISECBOR64::DecodeCanonical(exrSidecarBytes,exrEnvelope,&decodeError);
		const RISECBOR64::Value* exrPayload = exrDecoded ? exrEnvelope.Find("payload") : nullptr;
		const RISECBOR64::Value* exrProvenanceId = exrDecoded ?
			exrEnvelope.Find("provenance_id") : nullptr;
		RISECBOR64::Bytes exrPayloadBytes;
		const bool payloadEncoded = exrPayload &&
			RISECBOR64::Encode(*exrPayload,exrPayloadBytes,&decodeError);
		const RISECBOR64::Value* exrFidelity = exrPayload ?
			exrPayload->Find("artifact_fidelity") : nullptr;
		const RISECBOR64::Value* exrArtifactReasons = exrPayload ?
			exrPayload->Find("artifact_reason_codes") : nullptr;
		const RISECBOR64::Value* exrDigest = exrPayload ?
			exrPayload->Find("artifact_sha256") : nullptr;
		const RISECBOR64::Value* exrConfig = exrPayload ?
			exrPayload->Find("resolved_render_configuration_v1") : nullptr;
		const RISECBOR64::Value* exrOutput = exrConfig ? exrConfig->Find("output") : nullptr;
		const RISECBOR64::Value* exrAOVChannels = exrOutput ?
			exrOutput->Find("aov_channels") : nullptr;
		const RISECBOR64::Value* exrAttributes = exrOutput ?
			exrOutput->Find("attributes") : nullptr;
		const bool stripped = StripFireProvenanceEXRAttributes(
			exrBytes,strippedBytes,decodeError);
		Check( exrDecoded && payloadEncoded && exrProvenanceId &&
			exrProvenanceId->GetText() == RISECBOR64::SHA256Hex(exrPayloadBytes) &&
			exrFidelity && exrFidelity->GetText() == "preview_primary" &&
			exrArtifactReasons && exrArtifactReasons->GetArray().empty(),
			"[fire provenance] primary envelope uses the one-preimage ID and preview_primary" );
		Check( stripped && exrDigest &&
			exrDigest->GetText() == RISECBOR64::SHA256Hex(strippedBytes) &&
			store->Meta().primaryProvenanceId == exrProvenanceId->GetText(),
			"[fire provenance] EXR hash excludes mirrored attributes and binds the retained primary" );
		Check( exrOutput && exrOutput->Find("include_aovs") &&
			exrOutput->Find("include_aovs")->GetBoolean() && exrAOVChannels &&
			exrAOVChannels->GetArray().size() == 1u && exrAttributes &&
			exrAttributes->GetArray().size() == 1u &&
			exrAttributes->GetArray()[0].Find("name") &&
			exrAttributes->GetArray()[0].Find("name")->GetText() == "authoringNote" &&
			exrAttributes->GetArray()[0].Find("value") &&
			exrAttributes->GetArray()[0].Find("value")->GetText() == "ratchet",
			"[fire provenance] output config binds AOV selection and caller-authored attributes" );
		std::string verifyError;
		Check( VerifyFireProvenanceEXR(exrBytes,exrSidecarBytes,verifyError),
			"[fire provenance] verifier accepts the authoritative envelope and exact EXR mirrors" );
		std::vector<unsigned char> mismatchedEXR = exrBytes;
		const std::string statusName = "riseFireProv_render_fidelity_status";
		const std::string previewJSON = "\"preview\"";
		auto statusPos = std::search(mismatchedEXR.begin(),mismatchedEXR.end(),
			statusName.begin(),statusName.end());
		auto statusValue = statusPos == mismatchedEXR.end() ? mismatchedEXR.end() :
			std::search(statusPos,mismatchedEXR.end(),previewJSON.begin(),previewJSON.end());
		if( statusValue != mismatchedEXR.end() ) *(statusValue+1) = 'q';
		Check( statusValue != mismatchedEXR.end() &&
			!VerifyFireProvenanceEXR(mismatchedEXR,exrSidecarBytes,verifyError) &&
			verifyError.find("do not match") != std::string::npos,
			"[fire provenance] verifier rejects an independently mismatched EXR mirror" );
		bool attributesMatch = false;
		try {
			Imf::InputFile input(exrFile.c_str());
			const Imf::StringAttribute* statusAttribute =
				input.header().findTypedAttribute<Imf::StringAttribute>(
					"riseFireProv_render_fidelity_status");
			const Imf::StringAttribute* digestAttribute =
				input.header().findTypedAttribute<Imf::StringAttribute>(
					"riseFireProv_artifact_sha256");
			const Imf::StringAttribute* idAttribute =
				input.header().findTypedAttribute<Imf::StringAttribute>(
					"riseFireProv_provenance_id");
			attributesMatch = statusAttribute && statusAttribute->value() == "\"preview\"" &&
				digestAttribute && exrDigest && digestAttribute->value() ==
					"\""+exrDigest->GetText()+"\"" && idAttribute && exrProvenanceId &&
				idAttribute->value() == "\""+exrProvenanceId->GetText()+"\"";
		} catch( ... ) {
			attributesMatch = false;
		}
		Check( attributesMatch,
			"[fire provenance] EXR mirrors canonical JSON status, digest, and provenance ID" );

		const std::string signedZeroFile = MakeTempPathWithoutExt()+"_signed_zero.exr";
		opts.viewTransform.whiteBalance._01 = -0.0;
		std::string transactionError;
		Check( EncodeFrameStoreFileTransaction(*store,*exr,opts,signedZeroFile,
				transactionError),
			"[fire provenance] signed-zero EXR transaction succeeds" );
		std::vector<unsigned char> signedZeroBytes, signedZeroSidecar;
		Check( ReadFileAllBytes(signedZeroFile,signedZeroBytes) &&
			ReadFileAllBytes(signedZeroFile+".provenance.cbor",signedZeroSidecar) &&
			VerifyFireProvenanceEXR(signedZeroBytes,signedZeroSidecar,transactionError),
			"[fire provenance] EXR mirror canonicalizes negative zero exactly as CBOR" );

		const std::string whiteBalanceFile = MakeTempPathWithoutExt()+"_white_balance.exr";
		opts.viewTransform.whiteBalance._00 = 0.9;
		Check( EncodeFrameStoreFileTransaction(*store,*exr,opts,whiteBalanceFile,
				transactionError),
			"[fire provenance] nonidentity white-balance EXR transaction succeeds" );
		std::vector<unsigned char> whiteBalanceSidecar;
		RISECBOR64::Value whiteBalanceEnvelope;
		const bool whiteBalanceDecoded =
			ReadFileAllBytes(whiteBalanceFile+".provenance.cbor",whiteBalanceSidecar) &&
			RISECBOR64::DecodeCanonical(whiteBalanceSidecar,whiteBalanceEnvelope,
				&transactionError);
		const RISECBOR64::Value* whiteBalancePayload = whiteBalanceDecoded ?
			whiteBalanceEnvelope.Find("payload") : nullptr;
		const RISECBOR64::Value* whiteBalanceReasons = whiteBalancePayload ?
			whiteBalancePayload->Find("artifact_reason_codes") : nullptr;
		const RISECBOR64::Value* whiteBalanceFidelity = whiteBalancePayload ?
			whiteBalancePayload->Find("artifact_fidelity") : nullptr;
		const RISECBOR64::Value* whiteBalanceConfig = whiteBalancePayload ?
			whiteBalancePayload->Find("resolved_render_configuration_v1") : nullptr;
		const RISECBOR64::Value* whiteBalanceOutput = whiteBalanceConfig ?
			whiteBalanceConfig->Find("output") : nullptr;
		const RISECBOR64::Value* whiteBalanceMatrix = whiteBalanceOutput ?
			whiteBalanceOutput->Find("view_white_balance") : nullptr;
		bool hasWhiteBalanceReason = false;
		if( whiteBalanceReasons ) {
			for( const auto& reason : whiteBalanceReasons->GetArray() ) {
				hasWhiteBalanceReason = hasWhiteBalanceReason ||
					reason.GetText() == "white_balance_enabled";
			}
		}
		Check( whiteBalanceFidelity &&
			whiteBalanceFidelity->GetText() == "display_derivative" &&
			hasWhiteBalanceReason && whiteBalanceMatrix &&
			whiteBalanceMatrix->GetArray().size() == 9u &&
			whiteBalanceMatrix->GetArray()[0].GetFloat() == 0.9,
			"[fire provenance] white balance forces and fully records a display derivative" );

		opts.viewTransform.whiteBalance = Matrix3();
		const std::string denoisedBase = MakeTempPathWithoutExt()+"_denoised_frame";
		observer = new FileEncoderObserver(store,exr,opts,denoisedBase,true);
		observer->OnDenoiseComplete(12u,0u);
		safe_release(observer);
		const std::string denoisedFile = denoisedBase+"_denoised0012.exr";
		std::vector<unsigned char> denoisedSidecar;
		RISECBOR64::Value denoisedEnvelope;
		const bool denoisedDecoded =
			ReadFileAllBytes(denoisedFile+".provenance.cbor",denoisedSidecar) &&
			RISECBOR64::DecodeCanonical(denoisedSidecar,denoisedEnvelope,
				&transactionError);
		const RISECBOR64::Value* denoisedPayload = denoisedDecoded ?
			denoisedEnvelope.Find("payload") : nullptr;
		const RISECBOR64::Value* denoisedFidelity = denoisedPayload ?
			denoisedPayload->Find("artifact_fidelity") : nullptr;
		const RISECBOR64::Value* denoisedConfig = denoisedPayload ?
			denoisedPayload->Find("resolved_render_configuration_v1") : nullptr;
		const RISECBOR64::Value* denoisedOutput = denoisedConfig ?
			denoisedConfig->Find("output") : nullptr;
		Check( denoisedFidelity &&
			denoisedFidelity->GetText() == "display_derivative" &&
			denoisedOutput && denoisedOutput->Find("frame_index") &&
			denoisedOutput->Find("frame_index")->GetIntegerArgument() == 12u &&
			denoisedOutput->Find("denoised_derivative") &&
			denoisedOutput->Find("denoised_derivative")->GetBoolean(),
			"[fire provenance] denoised EXR is a frame-indexed display derivative" );

		const std::string concurrentFile = MakeTempPathWithoutExt()+"_concurrent.exr";
		bool concurrentA = false;
		bool concurrentB = false;
		std::string concurrentErrorA;
		std::string concurrentErrorB;
		std::thread writerA([&]() {
			concurrentA = EncodeFrameStoreFileTransaction(*store,*exr,opts,
				concurrentFile,concurrentErrorA);
		});
		std::thread writerB([&]() {
			concurrentB = EncodeFrameStoreFileTransaction(*store,*exr,opts,
				concurrentFile,concurrentErrorB);
		});
		writerA.join();
		writerB.join();
		std::vector<unsigned char> concurrentBytes, concurrentSidecar;
		Check( concurrentA && concurrentB &&
			ReadFileAllBytes(concurrentFile,concurrentBytes) &&
			ReadFileAllBytes(concurrentFile+".provenance.cbor",concurrentSidecar) &&
			VerifyFireProvenanceEXR(concurrentBytes,concurrentSidecar,transactionError),
			"[fire provenance] concurrent same-destination transactions publish one matched pair" );

		const std::string pngBase = MakeTempPathWithoutExt()+"_fire_derivative";
		const std::string pngFile = pngBase+".png";
		const std::string pngSidecar = pngFile+".provenance.cbor";
		opts.colorSpace = eColorSpace_sRGB;
		opts.bpp = 8;
		opts.viewTransform.toneCurve = eDisplayTransform_ACES;
		IFrameEncoder* png = FrameEncoderRegistry::Get().ByFormatName("PNG");
		observer = new FileEncoderObserver(store,png,opts,pngBase,false);
		store->AddObserver(observer);
		store->MarkFrameComplete(0);
		store->RemoveObserver(observer);
		safe_release(observer);
		std::vector<unsigned char> pngBytes, pngSidecarBytes;
		RISECBOR64::Value pngEnvelope;
		const bool pngDecoded = ReadFileAllBytes(pngFile,pngBytes) &&
			ReadFileAllBytes(pngSidecar,pngSidecarBytes) &&
			RISECBOR64::DecodeCanonical(pngSidecarBytes,pngEnvelope,&decodeError);
		const RISECBOR64::Value* pngPayload = pngDecoded ? pngEnvelope.Find("payload") : nullptr;
		const RISECBOR64::Value* pngFidelity = pngPayload ?
			pngPayload->Find("artifact_fidelity") : nullptr;
		const RISECBOR64::Value* derived = pngPayload ?
			pngPayload->Find("derived_from_primary") : nullptr;
		const RISECBOR64::Value* pngDigest = pngPayload ?
			pngPayload->Find("artifact_sha256") : nullptr;
		Check( pngFidelity && pngFidelity->GetText() == "display_derivative" &&
			derived && derived->Find("provenance_id") && exrProvenanceId &&
			derived->Find("provenance_id")->GetText() == exrProvenanceId->GetText() &&
			pngDigest && pngDigest->GetText() == RISECBOR64::SHA256Hex(pngBytes),
			"[fire provenance] display derivative links to the finalized preview primary" );

		const FrameStoreOutput::Metadata primaryBeforeDerivativeFailure = store->Meta();
		const std::string failedDerivativeBase =
			MakeTempPathWithoutExt()+"_failed_derivative";
		const std::string failedDerivativeFile = failedDerivativeBase+".ppm";
		const std::string failedDerivativeSidecar =
			failedDerivativeFile+".provenance.cbor";
		std::filesystem::create_directory(failedDerivativeSidecar);
		IFrameEncoder* ppm = FrameEncoderRegistry::Get().ByFormatName("PPM");
		observer = new FileEncoderObserver(store,ppm,opts,failedDerivativeBase,false);
		bool derivativeFailureThrew = false;
		try {
			observer->OnFrameComplete(0u,0u);
		} catch( ... ) {
			derivativeFailureThrew = true;
		}
		safe_release(observer);
		const FrameStoreOutput::Metadata primaryAfterDerivativeFailure = store->Meta();
		Check( !derivativeFailureThrew && !std::filesystem::exists(failedDerivativeFile) &&
			std::filesystem::is_directory(failedDerivativeSidecar) &&
			primaryAfterDerivativeFailure.primaryProvenanceId ==
				primaryBeforeDerivativeFailure.primaryProvenanceId &&
			primaryAfterDerivativeFailure.primaryArtifactSha256 ==
				primaryBeforeDerivativeFailure.primaryArtifactSha256 &&
			primaryAfterDerivativeFailure.primaryArtifactFidelity ==
				primaryBeforeDerivativeFailure.primaryArtifactFidelity,
			"[fire provenance] failed display derivative preserves the finalized primary" );
		std::filesystem::remove(failedDerivativeSidecar);
		std::remove(pngFile.c_str());
		std::remove(pngSidecar.c_str());
		std::remove(signedZeroFile.c_str());
		std::remove((signedZeroFile+".provenance.cbor").c_str());
		std::remove(whiteBalanceFile.c_str());
		std::remove((whiteBalanceFile+".provenance.cbor").c_str());
		std::remove(denoisedFile.c_str());
		std::remove((denoisedFile+".provenance.cbor").c_str());
		std::remove(concurrentFile.c_str());
		std::remove((concurrentFile+".provenance.cbor").c_str());
		std::remove(exrFile.c_str());
		std::remove(exrSidecar.c_str());
#endif
		const std::string movieTemporary = MakeTempPathWithoutExt()+"_movie.closed";
		const std::string movieFile = MakeTempPathWithoutExt()+"_movie.mov";
		{
			std::ofstream movie(movieTemporary,std::ios::binary);
			movie.write("test-movie-bytes",16);
		}
		std::vector<FireFramePrimary> movieFrames(2u);
		movieFrames[0].frameIndex = 4u;
		movieFrames[0].provenanceId = std::string(64u,'1');
		movieFrames[0].artifactSha256 = std::string(64u,'2');
		movieFrames[1].frameIndex = 5u;
		movieFrames[1].provenanceId = std::string(64u,'3');
		movieFrames[1].artifactSha256 = std::string(64u,'4');
		std::string movieError;
		const FrameStore::Metadata movieMetadata = store->Meta();
		const bool moviePublished = PublishFireFrameSequenceFileTransaction(
			movieMetadata,movieTemporary,movieFile,16u,16u,30u,2u,
			movieFrames,movieError);
		std::vector<unsigned char> movieBytes, movieSidecar;
		RISECBOR64::Value movieEnvelope;
		const bool movieDecoded = moviePublished &&
			ReadFileAllBytes(movieFile,movieBytes) &&
			ReadFileAllBytes(movieFile+".provenance.cbor",movieSidecar) &&
			RISECBOR64::DecodeCanonical(movieSidecar,movieEnvelope,&movieError);
		const RISECBOR64::Value* moviePayload = movieDecoded ?
			movieEnvelope.Find("payload") : nullptr;
		const RISECBOR64::Value* movieLinks = moviePayload ?
			moviePayload->Find("derived_from_frames") : nullptr;
		const RISECBOR64::Value* moviePrimary = moviePayload ?
			moviePayload->Find("derived_from_primary") : nullptr;
		const RISECBOR64::Value* movieFidelity = moviePayload ?
			moviePayload->Find("artifact_fidelity") : nullptr;
		const RISECBOR64::Value* movieReasons = moviePayload ?
			moviePayload->Find("artifact_reason_codes") : nullptr;
		const RISECBOR64::Value* movieDigest = moviePayload ?
			moviePayload->Find("artifact_sha256") : nullptr;
		const RISECBOR64::Value* movieConfig = moviePayload ?
			moviePayload->Find("resolved_render_configuration_v1") : nullptr;
		const RISECBOR64::Value* movieOutput = movieConfig ?
			movieConfig->Find("output") : nullptr;
		RISECBOR64::Bytes moviePayloadBytes;
		const RISECBOR64::Value* movieId = movieDecoded ?
			movieEnvelope.Find("provenance_id") : nullptr;
		const bool moviePayloadEncoded = moviePayload &&
			RISECBOR64::Encode(*moviePayload,moviePayloadBytes,&movieError);
		Check( moviePayloadEncoded && movieId &&
			movieId->GetText() == RISECBOR64::SHA256Hex(moviePayloadBytes) &&
			movieFidelity && movieFidelity->GetText() == "display_derivative" &&
			moviePrimary && moviePrimary->GetType() == RISECBOR64::Value::Null &&
			movieLinks && movieLinks->GetArray().size() == 2u &&
			movieLinks->GetArray()[0].Find("frame_index")->GetIntegerArgument() == 4u &&
			movieLinks->GetArray()[1].Find("frame_index")->GetIntegerArgument() == 5u,
			"[fire provenance] movie envelope hashes its contiguous ordered frame-link array" );
		Check( movieReasons && movieReasons->GetArray().size() == 3u &&
			movieReasons->GetArray()[0].GetText() == "display_transform_enabled" &&
			movieReasons->GetArray()[1].GetText() == "integer_output" &&
			movieReasons->GetArray()[2].GetText() == "lossy_output" &&
			movieDigest && movieDigest->GetText() == RISECBOR64::SHA256Hex(movieBytes) &&
			movieOutput && movieOutput->Find("format") &&
			movieOutput->Find("format")->GetText() == "MOV" &&
			movieOutput->Find("frame_count") &&
			movieOutput->Find("frame_count")->GetIntegerArgument() == 2u,
			"[fire provenance] movie records display/integer/lossy reasons and exact bytes" );

		const std::string badMovieTemporary = MakeTempPathWithoutExt()+"_bad_movie.closed";
		const std::string badMovieFile = MakeTempPathWithoutExt()+"_bad_movie.mov";
		{
			std::ofstream movie(badMovieTemporary,std::ios::binary);
			movie.write("bad",3);
		}
		movieFrames[1].frameIndex = 7u;
		Check( !PublishFireFrameSequenceFileTransaction(movieMetadata,
				badMovieTemporary,badMovieFile,16u,16u,30u,2u,movieFrames,movieError) &&
			!std::filesystem::exists(badMovieFile),
			"[fire provenance] movie transaction rejects a noncontiguous frame-link mutation" );

		const std::string blockedMovieTemporary =
			MakeTempPathWithoutExt()+"_blocked_movie.closed";
		const std::string blockedMovieFile =
			MakeTempPathWithoutExt()+"_blocked_movie.mov";
		{
			std::ofstream movie(blockedMovieTemporary,std::ios::binary);
			movie.write("blocked",7);
		}
		movieFrames[1].frameIndex = 5u;
		std::filesystem::create_directory(blockedMovieFile+".provenance.cbor");
		Check( !PublishFireFrameSequenceFileTransaction(movieMetadata,
				blockedMovieTemporary,blockedMovieFile,16u,16u,30u,2u,
				movieFrames,movieError) && !std::filesystem::exists(blockedMovieFile),
			"[fire provenance] movie sidecar failure leaves no unlabeled MOV artifact" );

		const std::string plainMovieTemporary =
			MakeTempPathWithoutExt()+"_plain_movie.closed";
		const std::string plainMovieFile =
			MakeTempPathWithoutExt()+"_plain_movie.mov";
		{
			std::ofstream priorArtifact(plainMovieFile,std::ios::binary);
			priorArtifact.write("prior-fire-movie",16);
			std::ofstream priorSidecar(plainMovieFile+".provenance.cbor",std::ios::binary);
			priorSidecar.write("prior-fire-sidecar",18);
			std::ofstream replacement(plainMovieTemporary,std::ios::binary);
			replacement.write("new-nonfire-movie",17);
		}
		Check( PublishUnprovenancedFileTransaction(plainMovieTemporary,
				plainMovieFile,movieError),
			"[fire provenance] nonfire movie replacement publishes transactionally" );
		std::vector<unsigned char> plainMovieBytes;
		Check( ReadFileAllBytes(plainMovieFile,plainMovieBytes) &&
			std::string(plainMovieBytes.begin(),plainMovieBytes.end()) ==
				"new-nonfire-movie" &&
			!std::filesystem::exists(plainMovieFile+".provenance.cbor"),
			"[fire provenance] nonfire movie replacement retires a stale fire sidecar" );
		{
			std::ofstream restoredSidecar(plainMovieFile+".provenance.cbor",std::ios::binary);
			restoredSidecar.write("restored-sidecar",16);
		}
		const std::string missingMovieTemporary =
			MakeTempPathWithoutExt()+"_missing_movie.closed";
		Check( !PublishUnprovenancedFileTransaction(missingMovieTemporary,
				plainMovieFile,movieError),
			"[fire provenance] missing nonfire movie is rejected before publication" );
		std::vector<unsigned char> preservedMovieBytes, preservedSidecarBytes;
		Check( ReadFileAllBytes(plainMovieFile,preservedMovieBytes) &&
			ReadFileAllBytes(plainMovieFile+".provenance.cbor",preservedSidecarBytes) &&
			preservedMovieBytes == plainMovieBytes &&
			std::string(preservedSidecarBytes.begin(),preservedSidecarBytes.end()) ==
				"restored-sidecar",
			"[fire provenance] failed nonfire publication preserves the prior pair" );
		std::remove(movieFile.c_str());
		std::remove((movieFile+".provenance.cbor").c_str());
		std::remove(badMovieTemporary.c_str());
		std::remove(badMovieFile.c_str());
		std::remove((badMovieFile+".provenance.cbor").c_str());
		std::remove(blockedMovieTemporary.c_str());
		std::remove(blockedMovieFile.c_str());
		std::filesystem::remove(blockedMovieFile+".provenance.cbor");
		std::remove(plainMovieFile.c_str());
		std::remove((plainMovieFile+".provenance.cbor").c_str());
		const FrameStore::Metadata beforeReset = store->Meta();
		store->SetFireFidelityMetadata("preview",beforeReset.renderReasonCodes,
			beforeReset.activeFireOpticsRecordIds,beforeReset.activeFireMedia,
			beforeReset.resolvedRenderConfigCoreV1,beforeReset.rendererBuildV1,
			beforeReset.rendererBuildId);
		Check( store->Meta().primaryProvenanceId.empty() &&
			store->Meta().primaryArtifactSha256.empty() &&
			store->Meta().primaryArtifactFidelity.empty(),
			"[fire provenance] a new render metadata envelope clears stale primary linkage" );
		safe_release(store);
	}
}

int main()
{
	// This test hands FileRasterizerOutput ABSOLUTE temp paths (see
	// MakeTempPathWithoutExt's comment).  FileRasterizerOutput's
	// constructor unconditionally prepends RISE_MEDIA_PATH onto its
	// pattern when `rendered_output_in_rise_media_folder` is TRUE (the
	// checked-in global.options default) — a real, intentional piece
	// of library behaviour for the SCENE-AUTHORED relative patterns
	// every production caller actually passes it.  No production
	// caller ever hands it an absolute path, so that concatenation
	// never needs to special-case one.  This test is the exception:
	// it exists to exercise the shim in isolation, so it must
	// neutralize the concatenation for its own lifetime rather than
	// asking the library to change that contract.  Save/restore
	// RISE_MEDIA_PATH around the whole run so a dev shell that has it
	// exported (per the Quickstart) doesn't turn our absolute temp
	// paths into a bogus double-rooted path that fails to open and
	// falls back to littering fro_temp_* files in the cwd.
	const char* savedMediaPath = std::getenv( "RISE_MEDIA_PATH" );
	const bool hadMediaPath = ( savedMediaPath != nullptr );
	const std::string savedMediaPathValue = hadMediaPath ? savedMediaPath : std::string();
#ifdef _WIN32
	_putenv_s( "RISE_MEDIA_PATH", "" );
#else
	setenv( "RISE_MEDIA_PATH", "", 1 );
#endif

	std::cout << "FileRasterizerOutputShimTest L3 — shim → file ≡ L2 IFrameEncoder bytes\n";
	std::cout << "----------------------------------------------------------------------\n";

	TestAllFormats();
	TestCameraExposurePropagation();
	TestHDRZerosCameraEV();
	TestDenoiseDualWrite();
	TestAnimationFrameNumbering();
	TestLongOutputPattern();
	TestMultiFrameReuse();
	TestFireFidelityProvenanceOutput();

	std::cout << "----------------------------------------------------------------------\n";
	std::cout << "passed " << gPassCount << ", failed " << gFailCount << "\n";

	if ( hadMediaPath ) {
#ifdef _WIN32
		_putenv_s( "RISE_MEDIA_PATH", savedMediaPathValue.c_str() );
#else
		setenv( "RISE_MEDIA_PATH", savedMediaPathValue.c_str(), 1 );
#endif
	} else {
#ifdef _WIN32
		_putenv_s( "RISE_MEDIA_PATH", "" );
#else
		unsetenv( "RISE_MEDIA_PATH" );
#endif
	}

	return gFailCount == 0 ? 0 : 1;
}
