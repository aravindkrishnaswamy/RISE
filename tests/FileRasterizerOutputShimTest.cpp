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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>  // getpid (POSIX)

#include "../src/Library/Rendering/FileRasterizerOutput.h"
#include "../src/Library/Rendering/FrameEncoders.h"
#include "../src/Library/RasterImages/RasterImage.h"
#include "../src/Library/Utilities/DiskFileWriteBuffer.h"
#include "../src/Library/Utilities/Reference.h"

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
		store->MutableMeta().cameraExposureEV = cameraExposureEV;

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

		// TIFF
		DiffOneCase( FileRasterizerOutput::TIFF, 8, eColorSpace_sRGB,
		             0.0, eDisplayTransform_None, eExrCompression_Piz, true,
		             "tiff", "TIFF default" );

		// TIFF with Reinhard tone curve
		DiffOneCase( FileRasterizerOutput::TIFF, 8, eColorSpace_sRGB,
		             0.0, eDisplayTransform_Reinhard, eExrCompression_Piz, true,
		             "tiff", "TIFF +Reinhard" );

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
}

int main()
{
	std::cout << "FileRasterizerOutputShimTest L3 — shim → file ≡ L2 IFrameEncoder bytes\n";
	std::cout << "----------------------------------------------------------------------\n";

	TestAllFormats();
	TestCameraExposurePropagation();
	TestHDRZerosCameraEV();
	TestDenoiseDualWrite();
	TestAnimationFrameNumbering();
	TestMultiFrameReuse();

	std::cout << "----------------------------------------------------------------------\n";
	std::cout << "passed " << gPassCount << ", failed " << gFailCount << "\n";
	return gFailCount == 0 ? 0 : 1;
}
