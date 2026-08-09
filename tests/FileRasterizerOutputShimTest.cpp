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
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
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
#include <ImfChannelList.h>
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

	RISECBOR64::Value ReplaceMapMember( const RISECBOR64::Value& map,
		const std::string& name, const RISECBOR64::Value& replacement )
	{
		RISECBOR64::Value::Members members;
		for( const auto& member : map.GetMap() ) {
			members.push_back(member.first == name ?
				std::make_pair(member.first,replacement) : member);
		}
		return RISECBOR64::Value::MapValue(members);
	}

	std::string SimpleCanonicalJSON( const RISECBOR64::Value& value )
	{
		if( value.GetType() == RISECBOR64::Value::UnsignedInteger ) {
			return std::to_string(value.GetIntegerArgument());
		}
		if( value.GetType() == RISECBOR64::Value::Text ) {
			return "\""+value.GetText()+"\"";
		}
		if( value.GetType() == RISECBOR64::Value::Array ) {
			std::string json = "[";
			for( std::size_t i=0; i<value.GetArray().size(); ++i ) {
				if( i ) json += ',';
				json += SimpleCanonicalJSON(value.GetArray()[i]);
			}
			return json+"]";
		}
		return std::string();
	}

	bool ReplaceBytesAfter( std::vector<unsigned char>& bytes,
		const std::string& marker, const std::string& from, const std::string& to )
	{
		if( from.size() != to.size() ) return false;
		auto markerPosition = std::search(bytes.begin(),bytes.end(),
			marker.begin(),marker.end());
		if( markerPosition == bytes.end() ) return false;
		auto valuePosition = std::search(markerPosition,bytes.end(),from.begin(),from.end());
		if( valuePosition == bytes.end() ) return false;
		std::copy(to.begin(),to.end(),valuePosition);
		return true;
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
		Check( BuildFrameArtifactFilename(longPattern,"_denoised",7u,"exr",true) ==
			longPattern+"_denoised0007.exr",
			"frame artifact filename construction never truncates dynamic patterns" );

		const std::filesystem::path root = MakeTempPathWithoutExt()+"_long_path";
		std::filesystem::path directory = root;
		for( unsigned int i=0u; i<3u; ++i ) {
			directory /= std::string(180u,static_cast<char>('a'+i));
		}
		std::error_code directoryError;
		const bool madeDirectories = std::filesystem::create_directories(
			directory,directoryError) || std::filesystem::exists(directory);
		const std::string actualPattern = (directory/"artifact").string();
		FileRasterizerOutput* output = new FileRasterizerOutput(
			actualPattern.c_str(),false,FileRasterizerOutput::TGA,8,
			eColorSpace_sRGB,0.0,eDisplayTransform_None,
			eExrCompression_Zip,true);
		RasterImage_Template<RISEPel>* image = new RasterImage_Template<RISEPel>(
			1u,1u,RISEColor(RISEPel(0.25,0.5,0.75),1.0));
		if( madeDirectories && output->HasEncoder() ) {
			output->OutputImage(*image,nullptr,0u);
		}
		Check( madeDirectories && output->HasEncoder() &&
			std::filesystem::exists(actualPattern+".tga"),
			"long valid nested output patterns publish through the observer write path" );
		std::filesystem::remove(actualPattern+".tga");
		std::filesystem::remove_all(root,directoryError);
		safe_release(image);
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
		const Value testLinkedDependency = Value::MapValue({
			{ "availability", Value::String("not_linked") },
			{ "linkage", Value::String("not_linked") },
			{ "loaded_binaries", Value::ArrayValue({}) },
			{ "version", Value::String("not_linked") }
		});
		const Value testRuntimeDependency = Value::MapValue({
			{ "availability", Value::String("not_loaded") },
			{ "linkage", Value::String("runtime_optional") },
			{ "loaded_binaries", Value::ArrayValue({}) },
			{ "version", Value::String("not_loaded") }
		});
		RISECBOR64::Encode(Value::MapValue({
			{ "animation", Value::MapValue({
				{ "do_fields", Value::Bool(false) },
				{ "frame_selection", Value::MapValue({
					{ "active", Value::Bool(false) },
					{ "index", Value::Unsigned(0) } }) },
				{ "invert_fields", Value::Bool(false) },
				{ "num_frames", Value::Unsigned(1) },
				{ "time_end", Value::Float(0.0) },
				{ "time_start", Value::Float(0.0) } }) },
			{ "aov", Value::MapValue({
				{ "channels", Value::ArrayValue({ Value::String("beauty") }) } }) },
			{ "camera", Value::MapValue({
				{ "exposure_compensation_ev", Value::Float(0.0) },
				{ "exposure_time", Value::Float(0.0) },
				{ "kind", Value::String("none") },
				{ "location", Value::ArrayValue({}) },
				{ "matrix", Value::ArrayValue({}) },
				{ "pixel_rate", Value::Float(0.0) },
				{ "projection", Value::MapValue({}) },
				{ "scanning_rate", Value::Float(0.0) } }) },
			{ "clamp", Value::MapValue({
				{ "direct", Value::Float(0.0) },
				{ "indirect", Value::Float(0.0) } }) },
			{ "depth", Value::MapValue({
				{ "max_diffuse_bounce", Value::Unsigned(0) },
				{ "max_eye_depth", Value::Unsigned(0) },
				{ "max_glossy_bounce", Value::Unsigned(0) },
				{ "max_light_depth", Value::Unsigned(0) },
				{ "max_recursion", Value::Unsigned(0) },
				{ "max_translucent_bounce", Value::Unsigned(0) },
				{ "max_transmission_bounce", Value::Unsigned(0) },
				{ "max_volume_bounce", Value::Unsigned(0) } }) },
			{ "evaluated_camera_states", Value::ArrayValue({}) },
			{ "execution", Value::MapValue({
				{ "effective_worker_task_count", Value::Unsigned(1) },
				{ "force_number_of_threads", Value::Signed(0) },
				{ "maximum_thread_count", Value::Signed(1) },
				{ "random_stream_policy", Value::String("process_shared_c_rand") },
				{ "render_thread_reserve_count", Value::Signed(0) } }) },
			{ "external_runtime", Value() },
			{ "film", Value::MapValue({
				{ "height", Value::Unsigned(kImgH) },
				{ "pixel_aspect_ratio", Value::Float(1.0) },
				{ "width", Value::Unsigned(kImgW) } }) },
			{ "filter", Value::MapValue({
				{ "height", Value::Float(1.0) },
				{ "name", Value::String("box") },
				{ "param_a", Value::Float(0.0) },
				{ "param_b", Value::Float(0.0) },
				{ "width", Value::Float(1.0) } }) },
			{ "global_render_options", Value::MapValue({
				{ "auto_probe", Value::MapValue({
					{ "activation_spp", Value::Unsigned(1) },
					{ "reach_winsor_percentile", Value::Float(0.99) },
					{ "scale", Value::Unsigned(1) },
					{ "spp", Value::Unsigned(1) },
					{ "tau_bdpt", Value::Float(1.0) },
					{ "tau_caustic", Value::Float(1.0) },
					{ "tau_reach", Value::Float(1.0) },
					{ "variance_renders", Value::Unsigned(2) } }) },
				{ "vcm", Value::MapValue({
					{ "progressive_radius_enabled", Value::Bool(true) },
					{ "throughput_clamp_multiplier", Value::Float(20.0) },
					{ "throughput_clamp_percentile", Value::Float(0.99) } }) } }) },
			{ "integrator", Value::MapValue({
				{ "auto_choice", Value::Unsigned(0) },
				{ "auto_probe_enabled", Value::Bool(false) },
				{ "effective_kind", Value::String("pathtracing_spectral_rasterizer") },
				{ "enable_vertex_connection", Value::Bool(false) },
				{ "enable_vertex_merging", Value::Bool(false) },
				{ "integrate_rgb", Value::Bool(false) },
				{ "kind", Value::String("pathtracing_spectral_rasterizer") },
				{ "merge_radius", Value::Float(0.0) },
				{ "path_guiding", Value::MapValue({
					{ "alpha", Value::Float(0.0) },
					{ "combine_training_iterations", Value::Bool(false) },
					{ "complete_path_guiding", Value::Bool(false) },
					{ "complete_path_strategy_samples", Value::Unsigned(0) },
					{ "complete_path_strategy_selection", Value::Bool(false) },
					{ "enabled", Value::Bool(false) },
					{ "learned_alpha", Value::Bool(false) },
					{ "max_guiding_depth", Value::Unsigned(0) },
					{ "max_light_guiding_depth", Value::Unsigned(0) },
					{ "online", Value::Bool(false) },
					{ "ris_candidates", Value::Unsigned(0) },
					{ "sampling_type", Value::Unsigned(0) },
					{ "training_iterations", Value::Unsigned(0) },
					{ "training_spp", Value::Unsigned(0) },
					{ "warmup_iterations", Value::Unsigned(0) } }) },
				{ "show_luminaires", Value::Bool(false) },
				{ "sms", Value::MapValue({
					{ "bernoulli_trials", Value::Unsigned(0) },
					{ "biased", Value::Bool(false) },
					{ "enabled", Value::Bool(false) },
					{ "max_chain_depth", Value::Unsigned(0) },
					{ "max_iterations", Value::Unsigned(0) },
					{ "max_photon_seeds_per_shading_point", Value::Unsigned(0) },
					{ "multi_trials", Value::Unsigned(0) },
					{ "photon_count", Value::Unsigned(0) },
					{ "seeding_mode", Value::Unsigned(0) },
					{ "target_bounces", Value::Unsigned(0) },
					{ "threshold", Value::Float(0.0) },
					{ "two_stage", Value::Bool(false) },
					{ "use_levenberg_marquardt", Value::Bool(false) } }) } }) },
			{ "light_sampling", Value::MapValue({
				{ "rr_threshold", Value::Float(0.0) } }) },
			{ "raster_sequence", Value::MapValue({
				{ "kind", Value::String("rasterizer_default") } }) },
			{ "record_kind", Value::String("resolved_render_configuration_v1") },
			{ "render_region", Value::MapValue({
				{ "active", Value::Bool(false) },
				{ "bottom", Value::Unsigned(0) },
				{ "left", Value::Unsigned(0) },
				{ "right", Value::Unsigned(0) },
				{ "top", Value::Unsigned(0) } }) },
			{ "sampler", Value::MapValue({
				{ "adaptive", Value::MapValue({
					{ "max_samples", Value::Unsigned(0) },
					{ "show_map", Value::Bool(false) },
					{ "threshold", Value::Float(0.0) } }) },
				{ "blue_noise", Value::Bool(false) },
				{ "large_step_probability", Value::Float(0.0) },
				{ "luminary_sampler", Value::String("none") },
				{ "luminary_sampler_param", Value::Float(0.0) },
				{ "mlt_bootstrap_samples", Value::Unsigned(0) },
				{ "mlt_chains", Value::Unsigned(0) },
				{ "mlt_mutations_per_pixel", Value::Unsigned(0) },
				{ "num_luminary_samples", Value::Unsigned(0) },
				{ "pixel_sampler", Value::String("random") },
				{ "pixel_sampler_param", Value::Float(0.0) },
				{ "pixel_samples", Value::Unsigned(1) },
				{ "progressive", Value::MapValue({
					{ "enabled", Value::Bool(false) },
					{ "samples_per_pass", Value::Unsigned(1) } }) },
				{ "spectral", Value::MapValue({
					{ "hwss", Value::Bool(false) },
					{ "nm_begin", Value::Float(380.0) },
					{ "nm_end", Value::Float(780.0) },
					{ "num_wavelengths", Value::Unsigned(1) },
					{ "spectral_samples", Value::Unsigned(1) } }) } }) },
			{ "schema_version", Value::Unsigned(1) },
			{ "shader", Value::String("none") },
			{ "stability", Value::MapValue({
				{ "filter_glossy", Value::Float(0.0) },
				{ "optimal_mis", Value::Bool(false) },
				{ "optimal_mis_tile_size", Value::Unsigned(1) },
				{ "optimal_mis_training_iterations", Value::Unsigned(0) },
				{ "rr_min_depth", Value::Unsigned(0) },
				{ "rr_threshold", Value::Float(0.0) },
				{ "transparent_shadows", Value::Bool(false) },
				{ "use_light_bvh", Value::Bool(false) } }) },
			{ "transport", Value::MapValue({
				{ "oidn", Value::Bool(false) },
				{ "oidn_device", Value::Unsigned(0) },
				{ "oidn_prefilter", Value::Unsigned(0) },
				{ "oidn_quality", Value::Unsigned(0) },
				{ "radiance_map", Value::MapValue({
					{ "background", Value::Bool(false) },
					{ "name", Value::String("none") },
					{ "orientation", Value::ArrayValue({ Value::Float(0.0),
						Value::Float(0.0),Value::Float(0.0) }) },
					{ "scale", Value::Float(1.0) } }) } }) }
		}),configBytes,&encodeError);
		RISECBOR64::Encode(Value::MapValue({
			{ "compiler", Value::MapValue({
				{ "identity", Value::String("test") },
				{ "language_standard", Value::String("c++17") },
				{ "lto_mode", Value::String("off") },
				{ "optimization_mode", Value::String("disabled") } }) },
			{ "dependency_builds", Value::MapValue({
				{ "avcodec", testRuntimeDependency },
				{ "avfoundation", testRuntimeDependency },
				{ "avformat", testRuntimeDependency },
				{ "avutil", testRuntimeDependency },
				{ "iex", testLinkedDependency },
				{ "ilmthread", testLinkedDependency },
				{ "imath", testLinkedDependency },
				{ "oidn", testLinkedDependency },
				{ "openexr", testLinkedDependency },
				{ "openpgl", testLinkedDependency },
				{ "png", testLinkedDependency },
				{ "swscale", testRuntimeDependency },
				{ "tiff", testLinkedDependency },
				{ "videotoolbox", testRuntimeDependency },
				{ "x265", testRuntimeDependency },
				{ "zlib", testLinkedDependency } }) },
			{ "dirty_state", Value::MapValue({
				{ "diff_sha256", Value::String(std::string(64,'0')) },
				{ "state", Value::String("clean") } }) },
			{ "fp_settings", Value::MapValue({
				{ "contraction_mode", Value::String("off") },
				{ "fast_math", Value::Bool(false) },
				{ "finite_math_only", Value::Bool(false) } }) },
			{ "gate_harness_version", Value::String("phase_a_gate_harness_v1") },
			{ "record_kind", Value::String("renderer_build_v1") },
			{ "renderer_binary", Value::MapValue({
				{ "hash_basis", Value::String("file_bytes") },
				{ "kind", Value::String("executable") },
				{ "path", Value::String("test") },
				{ "sha256", Value::String(std::string(64,'0')) } }) },
			{ "renderer_version", Value::String("test") },
			{ "schema_version", Value::Unsigned(1) },
			{ "solver_schema_versions", Value::ArrayValue({
				Value::String("fire_optics_schema_v3"),
				Value::String("fire_output_provenance_schema_v1") }) },
			{ "source_revision", Value::String("test-build") },
			{ "target", Value::MapValue({
				{ "architecture", Value::String("arm64") },
				{ "platform", Value::String("macos") } }) }
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
		opts.attrs.push_back(std::make_pair("authoringNote","ratchet"));
		IFrameEncoder* exr = FrameEncoderRegistry::Get().ByFormatName("EXR");
		EncodeOpts unsupportedAOVs = opts;
		unsupportedAOVs.includeAOVs = true;
		unsupportedAOVs.aovChannels = { FrameStoreOutput::ChannelId::Albedo };
		const std::string unsupportedAOVFile =
			MakeTempPathWithoutExt()+"_unsupported_aov.exr";
		std::string transactionError;
		Check( !EncodeFrameStoreFileTransaction(*store,*exr,unsupportedAOVs,
				unsupportedAOVFile,transactionError) &&
			!std::filesystem::exists(unsupportedAOVFile) &&
			!std::filesystem::exists(unsupportedAOVFile+".provenance.cbor") &&
			transactionError.find("does not support AOV") != std::string::npos,
			"[fire provenance] unsupported AOV requests fail before artifact publication" );
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
			!exrOutput->Find("include_aovs")->GetBoolean() && exrAOVChannels &&
			exrAOVChannels->GetArray().empty() && exrAttributes &&
			exrAttributes->GetArray().size() == 1u &&
			exrAttributes->GetArray()[0].Find("name") &&
			exrAttributes->GetArray()[0].Find("name")->GetText() == "authoringNote" &&
			exrAttributes->GetArray()[0].Find("value") &&
			exrAttributes->GetArray()[0].Find("value")->GetText() == "ratchet",
			"[fire provenance] output config binds effective channels and caller-authored attributes" );
		std::string verifyError;
		Check( VerifyFireProvenanceEXR(exrBytes,exrSidecarBytes,verifyError),
			"[fire provenance] verifier accepts the authoritative envelope and exact EXR mirrors" );
		auto rejectsSelfConsistentSemanticMutation = [&]( const std::string& key,
			const RISECBOR64::Value& replacement, const std::string& expectedError,
			const std::string& label ) {
			const RISECBOR64::Value* original = exrPayload->Find(key);
			const std::string oldJSON = original ? SimpleCanonicalJSON(*original) : std::string();
			const std::string newJSON = SimpleCanonicalJSON(replacement);
			const RISECBOR64::Value mutatedPayload =
				ReplaceMapMember(*exrPayload,key,replacement);
			RISECBOR64::Bytes mutatedPayloadBytes;
			std::string mutationError;
			const bool payloadOK = RISECBOR64::Encode(
				mutatedPayload,mutatedPayloadBytes,&mutationError);
			const std::string newId = payloadOK ?
				RISECBOR64::SHA256Hex(mutatedPayloadBytes) : std::string();
			const RISECBOR64::Value mutatedEnvelope = RISECBOR64::Value::MapValue({
				{ "payload", mutatedPayload },
				{ "provenance_id", RISECBOR64::Value::String(newId) }
			});
			RISECBOR64::Bytes mutatedSidecar;
			const bool sidecarOK = payloadOK && RISECBOR64::Encode(
				mutatedEnvelope,mutatedSidecar,&mutationError);
			std::vector<unsigned char> mutatedEXR = exrBytes;
			const bool mirrorsOK = original && !oldJSON.empty() && !newJSON.empty() &&
				ReplaceBytesAfter(mutatedEXR,"riseFireProv_"+key,oldJSON,newJSON) &&
				ReplaceBytesAfter(mutatedEXR,"riseFireProv_provenance_id",
					"\""+exrProvenanceId->GetText()+"\"","\""+newId+"\"");
			std::string semanticError;
			Check( sidecarOK && mirrorsOK &&
				!VerifyFireProvenanceEXR(mutatedEXR,mutatedSidecar,semanticError) &&
				semanticError.find(expectedError) != std::string::npos,label );
		};
		rejectsSelfConsistentSemanticMutation("schema_version",
			RISECBOR64::Value::Unsigned(2),"header",
			"[fire provenance] verifier rejects a self-consistent unknown schema version" );
		rejectsSelfConsistentSemanticMutation("artifact_fidelity",
			RISECBOR64::Value::String("unknown_primary"),"artifact_fidelity",
			"[fire provenance] verifier rejects a self-consistent unknown artifact fidelity" );
		RISECBOR64::Value::Values invalidReasons =
			exrPayload->Find("render_reason_codes")->GetArray();
		for( RISECBOR64::Value& reason : invalidReasons ) {
			if( reason.GetText() == "pel_transport" ) {
				reason = RISECBOR64::Value::String("not_a_reasonx");
			}
		}
		rejectsSelfConsistentSemanticMutation("render_reason_codes",
			RISECBOR64::Value::ArrayValue(invalidReasons),"outside the fixed enum",
			"[fire provenance] verifier rejects a self-consistent unknown render reason" );
		std::string invalidBuildId = exrPayload->Find("renderer_build_id")->GetText();
		invalidBuildId[0] = invalidBuildId[0] == 'a' ? 'b' : 'a';
		rejectsSelfConsistentSemanticMutation("renderer_build_id",
			RISECBOR64::Value::String(invalidBuildId),"renderer_build_id",
			"[fire provenance] verifier rejects a self-consistent mismatched build identity" );
		std::string invalidConfigId =
			exrPayload->Find("resolved_render_configuration_id")->GetText();
		invalidConfigId[0] = invalidConfigId[0] == 'a' ? 'b' : 'a';
		rejectsSelfConsistentSemanticMutation("resolved_render_configuration_id",
			RISECBOR64::Value::String(invalidConfigId),"configuration ID",
			"[fire provenance] verifier rejects a self-consistent mismatched config identity" );
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
		bool channelsMatch = false;
		try {
			Imf::InputFile input(exrFile.c_str());
			const Imf::ChannelList& channels = input.header().channels();
			const Imf::Channel* red = channels.findChannel("R");
			const Imf::Channel* green = channels.findChannel("G");
			const Imf::Channel* blue = channels.findChannel("B");
			const Imf::Channel* alpha = channels.findChannel("A");
			channelsMatch = red && green && blue && alpha &&
				red->type == Imf::FLOAT && green->type == Imf::FLOAT &&
				blue->type == Imf::FLOAT && alpha->type == Imf::FLOAT;
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
		Check( channelsMatch && exrOutput && exrOutput->Find("bits_per_channel") &&
			exrOutput->Find("bits_per_channel")->GetIntegerArgument() == 32u,
			"[fire provenance] emitted EXR channel types equal the effective precision claim" );

		const std::string signedZeroFile = MakeTempPathWithoutExt()+"_signed_zero.exr";
		opts.viewTransform.whiteBalance._01 = -0.0;
		Check( EncodeFrameStoreFileTransaction(*store,*exr,opts,signedZeroFile,
				transactionError),
			"[fire provenance] signed-zero EXR transaction succeeds" );
		std::vector<unsigned char> signedZeroBytes, signedZeroSidecar;
		Check( ReadFileAllBytes(signedZeroFile,signedZeroBytes) &&
			ReadFileAllBytes(signedZeroFile+".provenance.cbor",signedZeroSidecar) &&
			VerifyFireProvenanceEXR(signedZeroBytes,signedZeroSidecar,transactionError),
			"[fire provenance] EXR mirror canonicalizes negative zero exactly as CBOR" );

		const std::string halfFile = MakeTempPathWithoutExt()+"_effective_half.exr";
		EncodeOpts halfOpts = opts;
		halfOpts.bpp = 8u;
		const bool halfWritten = EncodeFrameStoreFileTransaction(
			*store,*exr,halfOpts,halfFile,transactionError);
		std::vector<unsigned char> halfSidecar;
		RISECBOR64::Value halfEnvelope;
		const bool halfDecoded = halfWritten &&
			ReadFileAllBytes(halfFile+".provenance.cbor",halfSidecar) &&
			RISECBOR64::DecodeCanonical(halfSidecar,halfEnvelope,&transactionError);
		const RISECBOR64::Value* halfPayload = halfDecoded ?
			halfEnvelope.Find("payload") : nullptr;
		const RISECBOR64::Value* halfConfig = halfPayload ?
			halfPayload->Find("resolved_render_configuration_v1") : nullptr;
		const RISECBOR64::Value* halfOutput = halfConfig ? halfConfig->Find("output") : nullptr;
		bool halfChannels = false;
		try {
			Imf::InputFile input(halfFile.c_str());
			const Imf::ChannelList& channels = input.header().channels();
			const Imf::Channel* red = channels.findChannel("R");
			const Imf::Channel* green = channels.findChannel("G");
			const Imf::Channel* blue = channels.findChannel("B");
			const Imf::Channel* alpha = channels.findChannel("A");
			halfChannels = red && green && blue && alpha &&
				red->type == Imf::HALF && green->type == Imf::HALF &&
				blue->type == Imf::HALF && alpha->type == Imf::HALF;
		} catch( ... ) {
			halfChannels = false;
		}
		Check( halfChannels && halfOutput && halfOutput->Find("bits_per_channel") &&
			halfOutput->Find("bits_per_channel")->GetIntegerArgument() == 16u,
			"[fire provenance] sub-32-bit EXR request resolves to matching FP16 channels" );

		const std::string whiteBalanceFile = MakeTempPathWithoutExt()+"_white_balance.exr";
		opts.viewTransform.whiteBalance._00 = 0.9;
		Check( !EncodeFrameStoreFileTransaction(*store,*exr,opts,whiteBalanceFile,
				transactionError) && !std::filesystem::exists(whiteBalanceFile) &&
			!std::filesystem::exists(whiteBalanceFile+".provenance.cbor") &&
			transactionError.find("does not apply a display transform") != std::string::npos,
			"[fire provenance] ignored EXR white balance fails before publication" );

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
		opts.attrs.clear();
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
		std::remove(halfFile.c_str());
		std::remove((halfFile+".provenance.cbor").c_str());
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
		Check( FrameStoreOutput::ValidateFireOutputMetadata(movieMetadata,movieError),
			"[fire provenance] finalized frame metadata passes the shared semantic validator" );
		FrameStore::Metadata invalidMetadata = movieMetadata;
		invalidMetadata.renderReasonCodes.erase(std::remove(
			invalidMetadata.renderReasonCodes.begin(),invalidMetadata.renderReasonCodes.end(),
			"producer_unqualified"),invalidMetadata.renderReasonCodes.end());
		Check( !FrameStoreOutput::ValidateFireOutputMetadata(invalidMetadata,movieError) &&
			movieError.find("producer_unqualified") != std::string::npos,
			"[fire provenance] static authored media require producer_unqualified" );
		invalidMetadata = movieMetadata;
		FrameStoreOutput::ActiveFireMedium collidingMedium =
			invalidMetadata.activeFireMedia.front();
		collidingMedium.managerName = "other_fire";
		collidingMedium.bindingOwner = "other_scene_slot";
		invalidMetadata.activeFireMedia.push_back(collidingMedium);
		std::sort(invalidMetadata.activeFireMedia.begin(),invalidMetadata.activeFireMedia.end(),
			[]( const FrameStoreOutput::ActiveFireMedium& lhs,
				const FrameStoreOutput::ActiveFireMedium& rhs ) {
				return std::tie(lhs.managerName,lhs.bindingKind,lhs.bindingOwner) <
					std::tie(rhs.managerName,rhs.bindingKind,rhs.bindingOwner);
			});
		Check( !FrameStoreOutput::ValidateFireOutputMetadata(invalidMetadata,movieError) &&
			movieError.find("share one authored_config_digest") != std::string::npos,
			"[fire provenance] distinct static media cannot share an authored digest" );
		invalidMetadata = movieMetadata;
		invalidMetadata.renderReasonCodes[0] = "not_a_fire_reason";
		Check( !FrameStoreOutput::ValidateFireOutputMetadata(invalidMetadata,movieError) &&
			movieError.find("outside the fixed enum") != std::string::npos,
			"[fire provenance] semantic validation rejects an unknown reason code" );
		invalidMetadata = movieMetadata;
		invalidMetadata.renderReasonCodes.push_back(invalidMetadata.renderReasonCodes.back());
		Check( !FrameStoreOutput::ValidateFireOutputMetadata(invalidMetadata,movieError) &&
			movieError.find("duplicated, or unsorted") != std::string::npos,
			"[fire provenance] semantic validation rejects duplicate reason codes" );
		invalidMetadata = movieMetadata;
		invalidMetadata.activeFireOpticsRecordIds[0][0] = 'A';
		Check( !FrameStoreOutput::ValidateFireOutputMetadata(invalidMetadata,movieError) &&
			movieError.find("lowercase SHA-256") != std::string::npos,
			"[fire provenance] semantic validation rejects a malformed aggregate record ID" );
		invalidMetadata = movieMetadata;
		invalidMetadata.activeFireMedia[0].authoredConfigDigest[0] = 'A';
		Check( !FrameStoreOutput::ValidateFireOutputMetadata(invalidMetadata,movieError) &&
			movieError.find("static_authored tagged variant") != std::string::npos,
			"[fire provenance] semantic validation rejects a malformed authored-config digest" );
		invalidMetadata = movieMetadata;
		invalidMetadata.activeFireMedia[0].opticalRecordIds[0] = std::string(64u,'b');
		Check( !FrameStoreOutput::ValidateFireOutputMetadata(invalidMetadata,movieError) &&
			movieError.find("do not equal the aggregate") != std::string::npos,
			"[fire provenance] semantic validation binds component IDs to the aggregate IDs" );
		invalidMetadata = movieMetadata;
		invalidMetadata.rendererBuildId[0] = invalidMetadata.rendererBuildId[0] == 'a' ? 'b' : 'a';
		Check( !FrameStoreOutput::ValidateFireOutputMetadata(invalidMetadata,movieError) &&
			movieError.find("renderer build identity") != std::string::npos,
			"[fire provenance] semantic validation binds renderer build bytes to their exact ID" );
		invalidMetadata = movieMetadata;
		invalidMetadata.primaryArtifactSha256.clear();
		Check( !FrameStoreOutput::ValidateFireOutputMetadata(invalidMetadata,movieError) &&
			movieError.find("primary linkage") != std::string::npos,
			"[fire provenance] semantic validation rejects a partial retained-primary tuple" );

		auto withoutMember = []( const RISECBOR64::Value& map,
			const std::string& name ) {
			RISECBOR64::Value::Members members;
			for( const auto& member : map.GetMap() ) {
				if( member.first != name ) members.push_back(member);
			}
			return RISECBOR64::Value::MapValue(members);
		};
		auto replaceMember = []( const RISECBOR64::Value& map,
			const std::string& name, const RISECBOR64::Value& replacement ) {
			RISECBOR64::Value::Members members;
			for( const auto& member : map.GetMap() ) {
				members.push_back(member.first == name ?
					std::make_pair(member.first,replacement) : member);
			}
			return RISECBOR64::Value::MapValue(members);
		};
		std::function<RISECBOR64::Value(const RISECBOR64::Value&,
			const std::vector<std::string>&,std::size_t)> withoutPath;
		withoutPath = [&]( const RISECBOR64::Value& map,
			const std::vector<std::string>& path, const std::size_t index ) {
			if( index+1u == path.size() ) return withoutMember(map,path[index]);
			const RISECBOR64::Value* child = map.Find(path[index]);
			return child ? replaceMember(map,path[index],withoutPath(*child,path,index+1u)) : map;
		};
		std::function<RISECBOR64::Value(const RISECBOR64::Value&,
			const std::vector<std::string>&,std::size_t,const RISECBOR64::Value&)> replacePath;
		replacePath = [&]( const RISECBOR64::Value& map,
			const std::vector<std::string>& path, const std::size_t index,
			const RISECBOR64::Value& replacement ) {
			if( index+1u == path.size() ) return replaceMember(map,path[index],replacement);
			const RISECBOR64::Value* child = map.Find(path[index]);
			return child ? replaceMember(map,path[index],
				replacePath(*child,path,index+1u,replacement)) : map;
		};
		auto encode = []( const RISECBOR64::Value& value ) {
			RISECBOR64::Bytes bytes;
			std::string error;
			RISECBOR64::Encode(value,bytes,&error);
			return bytes;
		};
		RISECBOR64::Value baseConfig;
		RISECBOR64::Value baseBuild;
		std::string schemaDecodeError;
		Check( RISECBOR64::DecodeCanonical(movieMetadata.resolvedRenderConfigCoreV1,
				baseConfig,&schemaDecodeError) &&
			RISECBOR64::DecodeCanonical(movieMetadata.rendererBuildV1,
				baseBuild,&schemaDecodeError),
			"[fire provenance] schema mutation fixtures decode canonically" );
		for( const auto& member : baseConfig.GetMap() ) {
			invalidMetadata = movieMetadata;
			invalidMetadata.resolvedRenderConfigCoreV1 = encode(
				withoutMember(baseConfig,member.first));
			Check( !FrameStoreOutput::ValidateFireOutputMetadata(invalidMetadata,movieError),
				"[fire provenance] resolved-config schema rejects missing top-level "+
				member.first );
		}
		const std::vector<std::vector<std::string> > configNestedPaths = {
			{ "animation", "do_fields" },
			{ "animation", "frame_selection", "index" },
			{ "aov", "channels" }, { "camera", "kind" },
			{ "clamp", "direct" }, { "depth", "max_eye_depth" },
			{ "execution", "random_stream_policy" }, { "film", "width" },
			{ "filter", "name" },
			{ "global_render_options", "auto_probe", "spp" },
			{ "global_render_options", "vcm", "progressive_radius_enabled" },
			{ "integrator", "kind" },
			{ "integrator", "path_guiding", "enabled" },
			{ "integrator", "sms", "enabled" },
			{ "light_sampling", "rr_threshold" },
			{ "raster_sequence", "kind" }, { "render_region", "active" },
			{ "sampler", "pixel_samples" },
			{ "sampler", "adaptive", "threshold" },
			{ "sampler", "progressive", "enabled" },
			{ "sampler", "spectral", "nm_begin" },
			{ "stability", "rr_threshold" }, { "transport", "oidn" },
			{ "transport", "radiance_map", "name" }
		};
		for( const auto& path : configNestedPaths ) {
			invalidMetadata = movieMetadata;
			invalidMetadata.resolvedRenderConfigCoreV1 = encode(
				withoutPath(baseConfig,path,0u));
			Check( !FrameStoreOutput::ValidateFireOutputMetadata(invalidMetadata,movieError),
				"[fire provenance] resolved-config schema rejects missing nested "+path.back() );
		}
		struct ConfigSemanticMutation {
			std::vector<std::string> path;
			RISECBOR64::Value replacement;
			const char* label;
		};
		const ConfigSemanticMutation configSemanticMutations[] = {
			{ { "evaluated_camera_states" }, RISECBOR64::Value::ArrayValue({
				RISECBOR64::Value::MapValue({
					{ "camera", *baseConfig.Find("camera") },
					{ "field", RISECBOR64::Value::String("middle") },
					{ "frame_index", RISECBOR64::Value::Unsigned(0) },
					{ "time", RISECBOR64::Value::Float(0.0) }
				}) }), "unknown evaluated-camera field" },
			{ { "execution", "random_stream_policy" }, RISECBOR64::Value::String("test"),
				"unknown random-stream policy" },
			{ { "integrator", "auto_choice" }, RISECBOR64::Value::Unsigned(4),
				"out-of-range auto-integrator choice" },
			{ { "integrator", "kind" }, RISECBOR64::Value::String("pathtracing_spectral"),
				"unknown integrator kind" },
			{ { "integrator", "effective_kind" }, RISECBOR64::Value::String("unknown"),
				"unknown effective integrator" },
			{ { "integrator", "effective_kind" }, RISECBOR64::Value::String("bdpt"),
				"incompatible integrator kind/effective-kind pairing" },
			{ { "integrator", "path_guiding", "sampling_type" },
				RISECBOR64::Value::Unsigned(2), "out-of-range path-guiding sampler" },
			{ { "integrator", "sms", "seeding_mode" }, RISECBOR64::Value::Unsigned(2),
				"out-of-range SMS seeding mode" },
			{ { "raster_sequence", "kind" }, RISECBOR64::Value::String("spiral"),
				"unknown raster sequence" },
			{ { "raster_sequence" }, RISECBOR64::Value::MapValue({
				{ "height", RISECBOR64::Value::Unsigned(8) },
				{ "kind", RISECBOR64::Value::String("block") },
				{ "order", RISECBOR64::Value::Unsigned(9) },
				{ "shuffle_seed", RISECBOR64::Value::Unsigned(0) },
				{ "shuffle_seed_active", RISECBOR64::Value::Bool(false) },
				{ "width", RISECBOR64::Value::Unsigned(8) }
			}), "out-of-range block raster order" },
			{ { "raster_sequence" }, RISECBOR64::Value::MapValue({
				{ "height", RISECBOR64::Value::Unsigned(8) },
				{ "kind", RISECBOR64::Value::String("block") },
				{ "order", RISECBOR64::Value::Unsigned(1) },
				{ "shuffle_seed", RISECBOR64::Value::Unsigned(0) },
				{ "shuffle_seed_active", RISECBOR64::Value::Bool(false) },
				{ "width", RISECBOR64::Value::Unsigned(8) }
			}), "inconsistent block raster shuffle activation" },
			{ { "transport", "oidn_device" }, RISECBOR64::Value::Unsigned(3),
				"out-of-range OIDN device" },
			{ { "transport", "oidn_prefilter" }, RISECBOR64::Value::Unsigned(2),
				"out-of-range OIDN prefilter" },
			{ { "transport", "oidn_quality" }, RISECBOR64::Value::Unsigned(4),
				"out-of-range OIDN quality" },
			{ { "aov", "channels" }, RISECBOR64::Value::ArrayValue({
				RISECBOR64::Value::String("unknown") }), "unknown AOV channel" }
		};
		for( const ConfigSemanticMutation& mutation : configSemanticMutations ) {
			invalidMetadata = movieMetadata;
			invalidMetadata.resolvedRenderConfigCoreV1 = encode(replacePath(
				baseConfig,mutation.path,0u,mutation.replacement));
			Check( !FrameStoreOutput::ValidateFireOutputMetadata(invalidMetadata,movieError),
				std::string("[fire provenance] resolved-config schema rejects ")+
				mutation.label );
		}
		for( const auto& member : baseBuild.GetMap() ) {
			invalidMetadata = movieMetadata;
			invalidMetadata.rendererBuildV1 = encode(withoutMember(baseBuild,member.first));
			invalidMetadata.rendererBuildId =
				RISECBOR64::SHA256Hex(invalidMetadata.rendererBuildV1);
			Check( !FrameStoreOutput::ValidateFireOutputMetadata(invalidMetadata,movieError),
				"[fire provenance] renderer-build schema rejects missing top-level "+
				member.first );
		}
		const std::vector<std::vector<std::string> > buildNestedPaths = {
			{ "compiler", "identity" }, { "dirty_state", "state" },
			{ "fp_settings", "fast_math" }, { "renderer_binary", "sha256" },
			{ "target", "platform" }, { "dependency_builds", "avcodec", "version" }
		};
		for( const auto& path : buildNestedPaths ) {
			invalidMetadata = movieMetadata;
			invalidMetadata.rendererBuildV1 = encode(withoutPath(baseBuild,path,0u));
			invalidMetadata.rendererBuildId =
				RISECBOR64::SHA256Hex(invalidMetadata.rendererBuildV1);
			Check( !FrameStoreOutput::ValidateFireOutputMetadata(invalidMetadata,movieError),
				"[fire provenance] renderer-build schema rejects missing nested "+path.back() );
		}
		const std::string runtimeHash(64u,'a');
		const RISECBOR64::Value runtimeBinary = RISECBOR64::Value::MapValue({
			{ "hash_basis", RISECBOR64::Value::String("file_bytes") },
			{ "path", RISECBOR64::Value::String("/test/libavcodec.1.dylib") },
			{ "sha256", RISECBOR64::Value::String(runtimeHash) }
		});
		const RISECBOR64::Value validRuntimeDependency = RISECBOR64::Value::MapValue({
			{ "availability", RISECBOR64::Value::String("loaded") },
			{ "linkage", RISECBOR64::Value::String("runtime_loaded") },
			{ "loaded_binaries", RISECBOR64::Value::ArrayValue({ runtimeBinary }) },
			{ "version", RISECBOR64::Value::String(
				"runtime_binaries_v1:libavcodec.1.dylib@version=mach_o_current_version:1.2.3@sha256="+
				runtimeHash) }
		});
		invalidMetadata = movieMetadata;
		invalidMetadata.rendererBuildV1 = encode(replacePath(baseBuild,
			{ "dependency_builds", "avcodec" },0u,validRuntimeDependency));
		invalidMetadata.rendererBuildId =
			RISECBOR64::SHA256Hex(invalidMetadata.rendererBuildV1);
		Check( FrameStoreOutput::ValidateFireOutputMetadata(invalidMetadata,movieError),
			"[fire provenance] renderer-build schema accepts a bound runtime version/hash" );
		const RISECBOR64::Value mismatchedRuntimeDependency = replaceMember(
			validRuntimeDependency,"version",RISECBOR64::Value::String(
				"runtime_binaries_v1:libavcodec.1.dylib@version=mach_o_current_version:1.2.3@sha256="+
				std::string(64u,'b')));
		invalidMetadata.rendererBuildV1 = encode(replacePath(baseBuild,
			{ "dependency_builds", "avcodec" },0u,mismatchedRuntimeDependency));
		invalidMetadata.rendererBuildId =
			RISECBOR64::SHA256Hex(invalidMetadata.rendererBuildV1);
		Check( !FrameStoreOutput::ValidateFireOutputMetadata(invalidMetadata,movieError),
			"[fire provenance] renderer-build schema rejects a runtime version/hash mismatch" );
		struct BuildSemanticMutation {
			std::vector<std::string> path;
			RISECBOR64::Value replacement;
			const char* label;
		};
		const BuildSemanticMutation buildSemanticMutations[] = {
			{ { "dirty_state", "diff_sha256" }, RISECBOR64::Value::String("not-a-digest"),
				"malformed dirty diff digest" },
			{ { "dirty_state", "state" }, RISECBOR64::Value::String("unknown"),
				"invalid dirty-state enum" },
			{ { "source_revision" }, RISECBOR64::Value::String(""),
				"empty source revision" },
			{ { "compiler", "identity" }, RISECBOR64::Value::String(""),
				"empty compiler identity" },
			{ { "compiler", "language_standard" }, RISECBOR64::Value::String("javascript"),
				"unknown language standard" },
			{ { "compiler", "lto_mode" }, RISECBOR64::Value::String("banana"),
				"unknown LTO mode" },
			{ { "compiler", "optimization_mode" }, RISECBOR64::Value::String("O4"),
				"unknown optimization mode" },
			{ { "fp_settings", "contraction_mode" }, RISECBOR64::Value::String("maybe"),
				"unknown FP contraction mode" },
			{ { "renderer_binary", "path" }, RISECBOR64::Value::String(""),
				"empty renderer-binary path" },
			{ { "renderer_binary", "kind" }, RISECBOR64::Value::String("spreadsheet"),
				"unknown renderer-binary kind" },
			{ { "renderer_binary", "hash_basis" }, RISECBOR64::Value::String("filename"),
				"unknown renderer-binary hash basis" },
			{ { "target", "platform" }, RISECBOR64::Value::String(""),
				"empty target platform" },
			{ { "target", "platform" }, RISECBOR64::Value::String("plan9"),
				"unknown target platform" },
			{ { "target", "architecture" }, RISECBOR64::Value::String("m68k"),
				"unknown target architecture" },
			{ { "gate_harness_version" }, RISECBOR64::Value::String("phase_b"),
				"unknown gate-harness version" },
			{ { "solver_schema_versions" }, RISECBOR64::Value::ArrayValue({
				RISECBOR64::Value::String("") }), "empty solver-schema identity" }
		};
		for( const BuildSemanticMutation& mutation : buildSemanticMutations ) {
			invalidMetadata = movieMetadata;
			invalidMetadata.rendererBuildV1 = encode(replacePath(
				baseBuild,mutation.path,0u,mutation.replacement));
			invalidMetadata.rendererBuildId =
				RISECBOR64::SHA256Hex(invalidMetadata.rendererBuildV1);
			Check( !FrameStoreOutput::ValidateFireOutputMetadata(invalidMetadata,movieError),
				std::string("[fire provenance] renderer-build schema rejects ")+
				mutation.label );
		}
		const bool moviePublished = PublishFireFrameSequenceFileTransaction(
			movieMetadata,FireFrameSequenceEncoding::AppleProRes4444_12Bit,
			movieTemporary,movieFile,16u,16u,30u,2u,
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
		const RISECBOR64::Value* movieEncoding = movieOutput ?
			movieOutput->Find("encoding") : nullptr;
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
			movieOutput->Find("codec") &&
			movieOutput->Find("codec")->GetText() == "apple_prores_4444" &&
			movieOutput->Find("bits_per_channel") &&
			movieOutput->Find("bits_per_channel")->GetIntegerArgument() == 12u &&
			movieOutput->Find("frame_count") &&
			movieOutput->Find("frame_count")->GetIntegerArgument() == 2u,
			"[fire provenance] movie records display/integer/lossy reasons and exact bytes" );
		Check( movieEncoding && movieEncoding->GetType() == RISECBOR64::Value::Map &&
			movieEncoding->GetMap().size() == 34u &&
			movieEncoding->Find("schema_version")->GetIntegerArgument() == 1u &&
			movieEncoding->Find("backend")->GetText() == "avfoundation" &&
			movieEncoding->Find("codec_implementation")->GetText() ==
				"AVVideoCodecTypeAppleProRes4444" &&
			movieEncoding->Find("codec_profile")->GetText() == "4444" &&
			movieEncoding->Find("input_pixel_format")->GetText() ==
				"kCVPixelFormatType_64RGBAHalf" &&
			movieEncoding->Find("output_pixel_format")->GetText() ==
				"prores_4444_12bit" &&
			movieEncoding->Find("chroma_subsampling")->GetText() == "4:4:4" &&
			movieEncoding->Find("alpha_mode")->GetText() == "encoded" &&
			movieEncoding->Find("color_range")->GetText() ==
				"avfoundation_codec_owned" &&
			movieEncoding->Find("color_primaries")->GetText() == "bt2020" &&
			movieEncoding->Find("transfer_function")->GetText() == "smpte_st_2084_pq" &&
			movieEncoding->Find("ycbcr_matrix")->GetText() ==
				"bt2020_nonconstant_luminance" &&
			movieEncoding->Find("display_transform")->GetText() ==
				"rec709_linear_to_rec2020_pq" &&
			movieEncoding->Find("reference_white_nits")->GetIntegerArgument() == 100u &&
			movieEncoding->Find("pq_peak_nits")->GetIntegerArgument() == 10000u &&
			movieEncoding->Find("dimension_rounding")->GetText() == "round_up_to_even" &&
			movieEncoding->Find("max_b_frames")->GetIntegerArgument() == 0u &&
			movieEncoding->Find("gop_frames")->GetIntegerArgument() == 1u &&
			movieEncoding->Find("rate_control")->GetText() == "constant_quality_intra" &&
			movieEncoding->Find("encoder_preset")->GetText() ==
				"not_configurable_by_avfoundation" &&
			movieEncoding->Find("codec_options")->GetText() ==
				"no_compression_properties" &&
			movieEncoding->Find("codec_tag")->GetText() == "ap4h" &&
			movieEncoding->Find("muxer_flags")->GetText() == "none" &&
			movieEncoding->Find("conversion_filter")->GetText() ==
				"avfoundation_managed" &&
			movieEncoding->Find("conversion_matrix")->GetText() ==
				"rec709_to_rec2020_d65" &&
			movieEncoding->Find("conversion_source_range")->GetText() == "full" &&
			movieEncoding->Find("conversion_destination_range")->GetText() ==
				"avfoundation_codec_owned" &&
			movieEncoding->Find("conversion_brightness")->GetIntegerArgument() == 0u &&
			movieEncoding->Find("conversion_contrast")->GetIntegerArgument() == 65536u &&
			movieEncoding->Find("conversion_saturation")->GetIntegerArgument() == 65536u &&
			!movieEncoding->Find("expects_media_data_in_real_time")->GetBoolean(),
			"[fire provenance] macOS movie encoding-v1 ratchets its full parameter surface" );

		FireFrameSequenceEncodingDescriptor windowsProRes;
		std::string descriptorError;
		Check( DescribeFireFrameSequenceEncoding(
				FireFrameSequenceEncoding::AppleProRes4444_10Bit,30u,
				windowsProRes,descriptorError) &&
			windowsProRes.backend == "ffmpeg_libavcodec_libavformat_libswscale" &&
			windowsProRes.containerFormat == "MOV" &&
			windowsProRes.codecImplementation == "prores_ks" &&
			windowsProRes.bitsPerChannel == 10u &&
			windowsProRes.outputPixelFormat == "yuva444p10le" &&
			windowsProRes.colorRange == "full" && windowsProRes.gopFrames == 1u &&
			windowsProRes.rateControl == "qscale_global_quality" &&
			windowsProRes.codecOptions ==
				"profile=4444;global_quality=FF_QP2LAMBDA*5" &&
			windowsProRes.codecTag == "ap4h" &&
			windowsProRes.conversionFilter == "sws_bilinear" &&
			windowsProRes.conversionMatrix == "sws_cs_bt2020" &&
			windowsProRes.conversionSourceRange == "full" &&
			windowsProRes.conversionDestinationRange == "full" &&
			windowsProRes.conversionBrightness == 0 &&
			windowsProRes.conversionContrast == 65536 &&
			windowsProRes.conversionSaturation == 65536,
			"[fire provenance] Windows ProRes encoding-v1 ratchets its full parameter surface" );
		Check( ValidateFireFrameSequenceEncodingDescriptor(
				FireFrameSequenceEncoding::AppleProRes4444_10Bit,30u,
				windowsProRes,descriptorError),
			"[fire provenance] exact Windows ProRes descriptor validates" );
		auto rejectsWindowsDescriptor = [&](
			const FireFrameSequenceEncodingDescriptor& changed, const char* label ) {
			std::string mutationError;
			Check( !ValidateFireFrameSequenceEncodingDescriptor(
				FireFrameSequenceEncoding::AppleProRes4444_10Bit,30u,
				changed,mutationError) && mutationError.find("differs") != std::string::npos,
				label );
		};
		FireFrameSequenceEncodingDescriptor changedWindowsDescriptor = windowsProRes;
		changedWindowsDescriptor.bitsPerChannel = 12u;
		rejectsWindowsDescriptor(changedWindowsDescriptor,
			"[fire provenance] Windows descriptor rejects changed bit depth");
		changedWindowsDescriptor = windowsProRes;
		changedWindowsDescriptor.inputPixelFormat = "rgba32";
		rejectsWindowsDescriptor(changedWindowsDescriptor,
			"[fire provenance] Windows descriptor rejects changed input pixel format");
		changedWindowsDescriptor = windowsProRes;
		changedWindowsDescriptor.chromaSubsampling = "4:2:0";
		rejectsWindowsDescriptor(changedWindowsDescriptor,
			"[fire provenance] Windows descriptor rejects changed chroma subsampling");
		changedWindowsDescriptor = windowsProRes;
		changedWindowsDescriptor.alphaMode = "dropped";
		rejectsWindowsDescriptor(changedWindowsDescriptor,
			"[fire provenance] Windows descriptor rejects changed alpha semantics");
		changedWindowsDescriptor = windowsProRes;
		changedWindowsDescriptor.displayTransform = "identity";
		rejectsWindowsDescriptor(changedWindowsDescriptor,
			"[fire provenance] Windows descriptor rejects changed display transform");
		changedWindowsDescriptor = windowsProRes;
		changedWindowsDescriptor.referenceWhiteNits = 101u;
		rejectsWindowsDescriptor(changedWindowsDescriptor,
			"[fire provenance] Windows descriptor rejects changed reference white");
		changedWindowsDescriptor = windowsProRes;
		changedWindowsDescriptor.pqPeakNits = 9999u;
		rejectsWindowsDescriptor(changedWindowsDescriptor,
			"[fire provenance] Windows descriptor rejects changed PQ peak");

		const std::string hevcTemporary = MakeTempPathWithoutExt()+"_hevc.closed";
		const std::string hevcFile = MakeTempPathWithoutExt()+"_hevc.mp4";
		{
			std::ofstream movie(hevcTemporary,std::ios::binary);
			movie.write("test-hevc-bytes",15);
		}
		const bool hevcPublished = PublishFireFrameSequenceFileTransaction(
			movieMetadata,FireFrameSequenceEncoding::HevcMain10_10Bit,
			hevcTemporary,hevcFile,16u,16u,30u,2u,movieFrames,movieError);
		std::vector<unsigned char> hevcSidecar;
		RISECBOR64::Value hevcEnvelope;
		const bool hevcDecoded = hevcPublished &&
			ReadFileAllBytes(hevcFile+".provenance.cbor",hevcSidecar) &&
			RISECBOR64::DecodeCanonical(hevcSidecar,hevcEnvelope,&movieError);
		const RISECBOR64::Value* hevcPayload = hevcDecoded ?
			hevcEnvelope.Find("payload") : nullptr;
		const RISECBOR64::Value* hevcConfig = hevcPayload ?
			hevcPayload->Find("resolved_render_configuration_v1") : nullptr;
		const RISECBOR64::Value* hevcOutput = hevcConfig ?
			hevcConfig->Find("output") : nullptr;
		const RISECBOR64::Value* hevcEncoding = hevcOutput ?
			hevcOutput->Find("encoding") : nullptr;
		Check( hevcOutput && hevcOutput->Find("format") &&
			hevcOutput->Find("format")->GetText() == "MP4" &&
			hevcOutput->Find("codec") &&
			hevcOutput->Find("codec")->GetText() == "hevc_main10" &&
			hevcOutput->Find("bits_per_channel") &&
			hevcOutput->Find("bits_per_channel")->GetIntegerArgument() == 10u,
			"[fire provenance] HEVC derivative records its actual MP4/Main10/10-bit encoding" );
		Check( hevcEncoding && hevcEncoding->GetMap().size() == 34u &&
			hevcEncoding->Find("backend")->GetText() ==
				"ffmpeg_libavcodec_libavformat_libswscale" &&
			hevcEncoding->Find("codec_implementation")->GetText() == "libx265" &&
			hevcEncoding->Find("codec_profile")->GetText() == "main10" &&
			hevcEncoding->Find("input_pixel_format")->GetText() == "rgba64le" &&
			hevcEncoding->Find("output_pixel_format")->GetText() == "yuv420p10le" &&
			hevcEncoding->Find("chroma_subsampling")->GetText() == "4:2:0" &&
			hevcEncoding->Find("alpha_mode")->GetText() == "dropped" &&
			hevcEncoding->Find("color_range")->GetText() == "limited" &&
			hevcEncoding->Find("gop_frames")->GetIntegerArgument() == 60u &&
			hevcEncoding->Find("rate_control")->GetText() == "crf_20" &&
			hevcEncoding->Find("encoder_preset")->GetText() == "medium" &&
			hevcEncoding->Find("codec_options")->GetText().find(
				"master-display=G(8500,39850)") != std::string::npos &&
			hevcEncoding->Find("codec_tag")->GetText() == "hvc1" &&
			hevcEncoding->Find("muxer_flags")->GetText() == "+faststart" &&
			hevcEncoding->Find("conversion_destination_range")->GetText() == "limited" &&
			hevcEncoding->Find("conversion_brightness")->GetIntegerArgument() == 0u &&
			hevcEncoding->Find("conversion_contrast")->GetIntegerArgument() == 65536u &&
			hevcEncoding->Find("conversion_saturation")->GetIntegerArgument() == 65536u,
			"[fire provenance] HEVC encoding-v1 ratchets rate control, HDR, mux, and conversion settings" );

		const std::string badMovieTemporary = MakeTempPathWithoutExt()+"_bad_movie.closed";
		const std::string badMovieFile = MakeTempPathWithoutExt()+"_bad_movie.mov";
		{
			std::ofstream movie(badMovieTemporary,std::ios::binary);
			movie.write("bad",3);
		}
		movieFrames[1].frameIndex = 7u;
		Check( !PublishFireFrameSequenceFileTransaction(movieMetadata,
				FireFrameSequenceEncoding::AppleProRes4444_12Bit,
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
				FireFrameSequenceEncoding::AppleProRes4444_12Bit,
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
		std::remove(hevcFile.c_str());
		std::remove((hevcFile+".provenance.cbor").c_str());
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
