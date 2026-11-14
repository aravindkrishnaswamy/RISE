//////////////////////////////////////////////////////////////////////
//
//  ImageReaderOverflowTest.cpp - Regression test for the header-controlled
//    WRITE-side heap overflows in RISE's hand-rolled / delegated image
//    decoders (TGAReader, HDRReader, PNGReader, TIFFReader).
//
//  Distinct from the read-side binary-buffer bounds family (VolumeShortRead
//  Test): those bounded the SOURCE reads.  These decoders size their
//  DESTINATION heap buffer from file-header dimensions with a 32-bit
//  `width*height*channels` multiply that can integer-overflow to a tiny
//  allocation the decode loop then writes past.  HDR additionally had an
//  unbounded Radiance-RLE decode cursor.  PNG/TIFF hand their (untrusted)
//  header dimensions to libpng/libtiff which do NOT validate the caller's
//  destination-buffer size (libpng's default limit is 1e6x1e6; libtiff's
//  TIFFReadRGBAImage writes width*height pixels blind).
//
//  The fix rejects degenerate / oversized dimensions before allocating
//  (64-bit multiply + sane ceiling) and, for HDR, bounds every RLE write to
//  the scanline / buffer, returning false on overrun rather than writing OOB.
//
//  This test crafts in-memory / temp-file "files":
//    TGA : overflow-dim (32768^2 wraps to 0), zero-dim, valid 2x2
//    HDR : new-RLE overflow-run, truncated scanline, valid 4x1 (+pixel),
//          old-RLE valid raw, old-RLE valid run (back-reference), old-RLE
//          crafted run at pixel 0 (back-reference underflow)
//    PNG : IHDR with 100000x100000 dims (valid CRC) -> rejected pre-alloc
//          (valid-PNG decode is covered by the broader scene-test suite,
//          which loads real .png textures)
//    TIFF: valid 2x2 decodes; a 65535^2-patched TIFF is safely rejected
//    EXR : FLOAT RGB/Y/YA range, offset windows, SDK YC/YCA reconstruction
//    All six readers: repeat decode, failed reload, recovery, end, and bounds
//  Under AddressSanitizer the crafted cases trap on the pre-fix code
//  (heap-buffer-overflow); on the fixed code they return false cleanly.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <iterator>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <stdexcept>
#include <cmath>
#include <atomic>
#include <thread>
#include <chrono>

#include <zlib.h>						// crc32 for the crafted PNG chunks

#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/Color/ColorUtils.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/RasterImages/TGAReader.h"
#include "../src/Library/RasterImages/HDRReader.h"
#include "../src/Library/RasterImages/PNGReader.h"
#include "../src/Library/RasterImages/TIFFReader.h"
#include "../src/Library/RasterImages/JPEGReader.h"
#include "../src/Library/RasterImages/EXRReader.h"
#include "../src/Library/RasterImages/EXRWriter.h"
#ifndef NO_EXR_SUPPORT
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfChannelList.h>
#endif
#include "../src/Library/RasterImages/TGAWriter.h"
#include "../src/Library/RasterImages/PPMWriter.h"
#include "../src/Library/RasterImages/RasterImage.h"
#include "../src/Library/Interfaces/IJobRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "TestTempDir.h"

#ifndef NO_TIFF_SUPPORT
	#include <tiffio.h>
#endif

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE { namespace Implementation {
	// Cpp-only delegate to the factory used by Job::AddCallbackRasterizerOutput.
	IRasterizerOutput* ForTest_CreateCallbackRasterizerOutput(IJobRasterizerOutput& receiver);
} }

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const char* name )
{
	if( cond ) {
		++passCount;
		const char* marker = std::getenv("RISE_RED_PROVE_MARKER");
		if( marker && std::strstr(name, marker) ) std::cout << "  PASS: " << name << std::endl;
	}
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

static void TestNormalizedIntegerize()
{
	// Runtime libc parsing keeps nonfinite inputs real under production
	// fast-math flags, following FiniteMathTest's value-boundary convention.
	const double nan=std::strtod("nan",nullptr);
	const double pinf=std::strtod("1e999",nullptr);
	const double ninf=std::strtod("-1e999",nullptr);
	struct Case { double value; unsigned int byte,word; const char* label; };
	const Case cases[]={
		{-0.25,0,0,"negative"},{0,0,0,"zero"},{0.25,63,16383,"quarter truncation"},
		{0.5,127,32767,"half truncation"},{1,255,65535,"one"},{2,255,65535,"above one"},
		{nan,0,0,"runtime NaN"},{pinf,255,65535,"runtime positive infinity"},{ninf,0,0,"runtime negative infinity"}
	};
	for(const auto& item : cases) {
		const ColorRec709RGB color(item.value,item.value,item.value,item.value);
		const auto bytes=color.Integerize<Rec709RGBPel,unsigned char>(255);
		const auto words=color.Integerize<Rec709RGBPel,unsigned short>(65535);
		Check(bytes.r==item.byte && bytes.g==item.byte && bytes.b==item.byte && bytes.a==item.byte,
			(std::string("normalized Integerize 8-bit: all RGBA channels saturate ")+item.label).c_str());
		Check(words.r==item.word && words.g==item.word && words.b==item.word && words.a==item.word,
			(std::string("normalized Integerize 16-bit: all RGBA channels saturate ")+item.label).c_str());
	}
	const ColorRec709RGB mixed(nan,pinf,ninf,0.5);
	const auto bytes=mixed.Integerize<Rec709RGBPel,unsigned char>(255);
	const auto words=mixed.Integerize<Rec709RGBPel,unsigned short>(65535);
	Check(bytes.r==0 && bytes.g==255 && bytes.b==0 && bytes.a==127,
		"normalized Integerize 8-bit: nonfinite RGB channels saturate independently of valid alpha");
	Check(words.r==0 && words.g==65535 && words.b==0 && words.a==32767,
		"normalized Integerize 16-bit: nonfinite RGB channels saturate independently of valid alpha");
	const ColorRec709RGB raw(2,-1,0.25,0.5);Rec709RGBPel pel=raw.base;
	const auto scaled=raw.ScaleRGBPelValuesAndIncludeAlpha<double>(pel,2);
	Check(scaled.r==4 && scaled.g==-2 && scaled.b==0.5 && scaled.a==1,
		"normalized Integerize: public raw scaling helper preserves its unnormalized representable products");
}

static void TestPremultiplyAlphaRGB()
{
	const auto byteHalf=ColorUtils::PremultiplyAlphaRGB<RGBA8,255>({200,100,50,128});
	const bool validated=byteHalf.r==100 && byteHalf.g==50 && byteHalf.b==25 && byteHalf.a==128;
	Check(validated,
		"premultiply alpha: 8-bit fractional RGB multiplies and alpha is preserved");
	// The old helper divides by alpha and overflows signed 16-bit products.
	// First prove its operation is fixed, so RED exits via an ordinary failed
	// assertion instead of executing those known divide-by-zero/overflow sites.
	if(!validated)return;
	const auto byteZero=ColorUtils::PremultiplyAlphaRGB<RGBA8,255>({200,100,50,0});
	Check(byteZero.r==0 && byteZero.g==0 && byteZero.b==0 && byteZero.a==0,
		"premultiply alpha: 8-bit transparent RGB becomes zero without division");
	const auto byteFull=ColorUtils::PremultiplyAlphaRGB<RGBA8,255>({200,100,50,255});
	Check(byteFull.r==200 && byteFull.g==100 && byteFull.b==50 && byteFull.a==255,
		"premultiply alpha: 8-bit opaque RGBA is unchanged");
	const auto wordZero=ColorUtils::PremultiplyAlphaRGB<RGBA16,65535>({65535,50000,32768,0});
	Check(wordZero.r==0 && wordZero.g==0 && wordZero.b==0 && wordZero.a==0,
		"premultiply alpha: 16-bit transparent RGB becomes zero without division");
	const auto wordHalf=ColorUtils::PremultiplyAlphaRGB<RGBA16,65535>({65535,50000,32768,32768});
	Check(wordHalf.r==32768 && wordHalf.g==25000 && wordHalf.b==16384 && wordHalf.a==32768,
		"premultiply alpha: large 16-bit samples use exact wide products and preserve alpha");
	const auto wordFull=ColorUtils::PremultiplyAlphaRGB<RGBA16,65535>({65535,65534,60000,65535});
	Check(wordFull.r==65535 && wordFull.g==65534 && wordFull.b==60000 && wordFull.a==65535,
		"premultiply alpha: maximum 16-bit alpha has no signed product overflow");
	const auto floatZero=ColorUtils::PremultiplyAlphaRGB<RGBA_T<double>,1>({2,-4,.5,0});
	Check(floatZero.r==0 && floatZero.g==0 && floatZero.b==0 && floatZero.a==0,
		"premultiply alpha: floating transparent RGB becomes zero");
	const auto floatHalf=ColorUtils::PremultiplyAlphaRGB<RGBA_T<double>,1>({2,-4,.5,.5});
	Check(floatHalf.r==1 && floatHalf.g==-2 && floatHalf.b==.25 && floatHalf.a==.5,
		"premultiply alpha: floating RGB retains signed HDR values and fractional alpha");
	const auto floatFull=ColorUtils::PremultiplyAlphaRGB<RGBA_T<double>,1>({2,-4,.5,1});
	Check(floatFull.r==2 && floatFull.g==-4 && floatFull.b==.5 && floatFull.a==1,
		"premultiply alpha: floating opaque RGBA is unchanged");
	// Keep RGB and alpha runtime inputs so fast-math/LTO must execute the
	// arithmetic instead of folding this overflow-sensitive oracle away.
	const double hugeRed=std::strtod("0x1p1020",nullptr);
	const double hugeGreen=std::strtod("0x1p1018",nullptr);
	const double halfAlpha=std::strtod("32767.5",nullptr);
	const auto huge=ColorUtils::PremultiplyAlphaRGB<RGBA_T<double>,65535>(
		{hugeRed,hugeGreen,-.5,halfAlpha});
	Check(huge.r==std::ldexp(1.0,1019) && huge.g==std::ldexp(1.0,1017) && huge.b==-.25 && huge.a==32767.5,
		"premultiply alpha: bounded floating mantissas avoid intermediate overflow");
	const auto doubleBits=[](const double& actual,std::uint64_t bits) {
		double expected;std::memcpy(&expected,&bits,sizeof(expected));
		return std::memcmp(&actual,&expected,sizeof(expected))==0;
	};
	const auto floatBits=[](const float& actual,std::uint32_t bits) {
		float expected;std::memcpy(&expected,&bits,sizeof(expected));
		return std::memcmp(&actual,&expected,sizeof(expected))==0;
	};
	const double smallestAlpha=std::strtod("0x1p-1074",nullptr);
	const auto tinyProduct=ColorUtils::PremultiplyAlphaRGB<RGBA_T<double>,65535>(
		{hugeRed,hugeRed,hugeRed,smallestAlpha});
	// 2^1020 * 2^-1074 / 65535 rounds to 0x1.0001000100010p-70.
	Check(doubleBits(tinyProduct.r,0x3b90001000100010ULL) && doubleBits(tinyProduct.g,0x3b90001000100010ULL) &&
		doubleBits(tinyProduct.b,0x3b90001000100010ULL) && doubleBits(tinyProduct.a,1),
		"premultiply alpha range: huge runtime RGB and subnormal alpha retain a normal result and exact alpha bits");
	const double opaqueAlpha=std::strtod("65535",nullptr);
	const double minimumNormal=std::strtod("0x1p-1022",nullptr);
	const auto opaqueNormal=ColorUtils::PremultiplyAlphaRGB<RGBA_T<double>,65535>(
		{minimumNormal,minimumNormal,minimumNormal,opaqueAlpha});
	Check(doubleBits(opaqueNormal.r,0x0010000000000000ULL) && doubleBits(opaqueNormal.g,0x0010000000000000ULL) &&
		doubleBits(opaqueNormal.b,0x0010000000000000ULL) && std::memcmp(&opaqueNormal.a,&opaqueAlpha,sizeof(opaqueAlpha))==0,
		"premultiply alpha range: opaque runtime minimum-normal RGB preserves every bit");
	const double minimumSubnormal=std::strtod("0x1p-1074",nullptr);
	const auto opaqueSubnormal=ColorUtils::PremultiplyAlphaRGB<RGBA_T<double>,65535>(
		{minimumSubnormal,minimumSubnormal,minimumSubnormal,opaqueAlpha});
	Check(doubleBits(opaqueSubnormal.r,1) && doubleBits(opaqueSubnormal.g,1) && doubleBits(opaqueSubnormal.b,1) &&
		std::memcmp(&opaqueSubnormal.a,&opaqueAlpha,sizeof(opaqueAlpha))==0,
		"premultiply alpha range: opaque runtime minimum-subnormal RGB preserves every bit");
	const float floatRGB=std::strtof("0x1p120",nullptr),floatAlpha=std::strtof("0x1p-149",nullptr);
	const auto floatTiny=ColorUtils::PremultiplyAlphaRGB<RGBA_T<float>,65536>({floatRGB,floatRGB,floatRGB,floatAlpha});
	// 2^120 * 2^-149 / 2^16 is exactly 2^-45 (binary32 0x29000000).
	Check(floatBits(floatTiny.r,0x29000000U) && floatBits(floatTiny.g,0x29000000U) && floatBits(floatTiny.b,0x29000000U) &&
		floatBits(floatTiny.a,1),"premultiply alpha range: runtime float32 subnormal alpha retains the exact normal product");
}

static void TestCallbackExportBounds()
{
	struct Receiver final : IJobRasterizerOutput {
		bool floats=false,premultiplied=true;
		unsigned int calls16=0,calls32=0,policies=0,w=0,h=0;
		Rect region{0,0,0,0};
		std::vector<unsigned short> words;std::vector<float> values;
		bool PremultipliedAlpha() override{++policies;return premultiplied;}
		int GetColorSpace() override{++policies;return 0;}
		bool WantsFloat32() override{++policies;return floats;}
		void OutputImageRGBA16(const unsigned short* pixels,unsigned int width,unsigned int height,
			unsigned int top,unsigned int left,unsigned int bottom,unsigned int right) override {
			++calls16;w=width;h=height;region=Rect(top,left,bottom,right);
			words.assign(pixels,pixels+static_cast<size_t>(width)*height*4);
		}
		void OutputImageRGBA32F(const float* pixels,unsigned int width,unsigned int height,
			unsigned int top,unsigned int left,unsigned int bottom,unsigned int right) override {
			++calls32;w=width;h=height;region=Rect(top,left,bottom,right);
			values.assign(pixels,pixels+static_cast<size_t>(width)*height*4);
		}
	} receiver;
	struct Image final : RISERasterImage {
		unsigned int w=1,h=1;bool hdr=false,stop=false;
		// Instrumentation on this fake does not mutate its image semantics.
		mutable size_t reads=0;mutable bool outside=false;
		Image():RISERasterImage(1,1,RISEColor(0,0,0,1)){}
		unsigned int GetWidth() const override{return w;}
		unsigned int GetHeight() const override{return h;}
		RISEColor GetPEL(unsigned int x,unsigned int y) const override {
			++reads;outside |= x>=w || y>=h;
			if(stop)throw std::runtime_error("callback image read canary");
			const double alpha[]={0,.5,1};const auto i=(static_cast<size_t>(y)*w+x)%3;
			return hdr?RISEColor(4,-2,.5,alpha[i]):RISEColor(1,.5,.25,alpha[i]);
		}
		void Reset(unsigned int width,unsigned int height,bool floating,bool canary=false) {
			w=width;h=height;hdr=floating;stop=canary;reads=0;outside=false;
		}
	};
	const auto release=[](auto* p){if(p)p->release();};
	std::unique_ptr<IRasterizerOutput,decltype(release)> output(ForTest_CreateCallbackRasterizerOutput(receiver),release);
	std::unique_ptr<Image,decltype(release)> image(new Image(),release);
	Check(output && output->refcount()==1,"callback export: production factory delegate returns one owned dispatcher reference");
	if(!output)return;
	const unsigned short premul16[][4]={{0,0,0,0},{32767,16383,8191,32767},{65535,32767,16383,65535}};
	const unsigned short straight16[][4]={{65535,32767,16383,0},{65535,32767,16383,32767},{65535,32767,16383,65535}};
	const float premul32[][4]={{0,0,0,0},{2,-1,.25f,.5f},{4,-2,.5f,1}};
	const float straight32[][4]={{4,-2,.5f,0},{4,-2,.5f,.5f},{4,-2,.5f,1}};
	const auto pixelMatches=[&](size_t index) {
		for(unsigned int c=0;c<4;++c) {
			if(receiver.floats) {
				if(receiver.values[index*4+c]!=(receiver.premultiplied?premul32:straight32)[index%3][c])return false;
			} else if(receiver.words[index*4+c]!=(receiver.premultiplied?premul16:straight16)[index%3][c])return false;
		}
		return true;
	};
	const auto healthy=[&](unsigned int width,unsigned int height,const char* label) {
		image->Reset(width,height,receiver.floats);
		const auto old16=receiver.calls16,old32=receiver.calls32;bool escaped=false;
		try{output->OutputIntermediateImage(*image,nullptr);}catch(...){escaped=true;}
		const size_t count=static_cast<size_t>(width)*height;
		bool exact=!escaped && image->reads==count && !image->outside && receiver.w==width && receiver.h==height &&
			receiver.calls16==old16+(receiver.floats?0:1) && receiver.calls32==old32+(receiver.floats?1:0) &&
			(receiver.floats?receiver.values.size():receiver.words.size())==count*4;
		if(exact)for(size_t i=0;i<count;++i)exact &= pixelMatches(i);
		Check(exact,(std::string("callback export ")+(receiver.floats?"32F: ":"16-bit: ")+label).c_str());
	};
	bool dimensionsGuarded=true;
	for(bool floating : {false,true}) {
		// The old float path allocates 68 GiB before GetPEL on the overflow
		// fixture. Establish its shared dimension gate using the 16-bit path.
		if(floating && !dimensionsGuarded)break;
		receiver.floats=floating;receiver.premultiplied=true;
		healthy(3,1,"zero fractional and opaque alpha match independent premultiplied pixels");
		receiver.premultiplied=false;
		healthy(3,1,"same-size policy change preserves independent straight RGBA pixels");
		receiver.premultiplied=true;
		healthy(2,2,"growth preserves every pixel and alpha");
		healthy(1,1,"shrink preserves every pixel and alpha");
		healthy(2,2,"replacement restores the complete coordinate fixture");
		image->Reset(2,2,floating);
		unsigned int before=receiver.calls16+receiver.calls32;bool escaped=false;
		const Rect outside(0,2,0,2);
		try{output->OutputIntermediateImage(*image,&outside);}catch(...){escaped=true;}
		const bool regionsGuarded=!escaped && image->reads==0 && receiver.calls16+receiver.calls32==before;
		Check(regionsGuarded,"callback export: wholly outside region performs no source reads or receiver output");
		// Old x=2,y=0 aliases pixel2 inside the allocated 2x2 buffer. Only
		// after that safe canary succeeds do we try formerly unsafe endpoints.
		if(regionsGuarded) {
			image->Reset(2,2,floating);before=receiver.calls16+receiver.calls32;
			const Rect clipped(0,1,std::numeric_limits<unsigned int>::max(),std::numeric_limits<unsigned int>::max());
			output->OutputIntermediateImage(*image,&clipped);
			const bool sizeOK=(floating?receiver.values.size():receiver.words.size())==16;
			Check(image->reads==2 && !image->outside && receiver.calls16+receiver.calls32==before+1 &&
				receiver.region.top==0 && receiver.region.left==1 && receiver.region.bottom==1 && receiver.region.right==1 &&
				sizeOK && pixelMatches(1) && pixelMatches(3),"callback export: region endpoints clip to image bounds before indexing");
			image->Reset(2,2,floating);before=receiver.calls16+receiver.calls32;
			const Rect inverted(1,1,0,0);output->OutputIntermediateImage(*image,&inverted);
			Check(image->reads==0 && receiver.calls16+receiver.calls32==before,
				"callback export: inverted region performs no source reads or receiver output");
		}
		const unsigned int invalid[][2]={{0,1},{1,0},{65536,65536},
			{std::numeric_limits<unsigned int>::max(),std::numeric_limits<unsigned int>::max()}};
		for(const auto& dims:invalid) {
			image->Reset(dims[0],dims[1],floating,true);before=receiver.calls16+receiver.calls32;
			const auto policies=receiver.policies;escaped=false;
			// If old 16-bit allocation wraps to zero, GetPEL throws before its
			// result can be written into that zero-sized allocation.
			try{output->OutputIntermediateImage(*image,nullptr);}catch(...){escaped=true;}
			const bool rejected=!escaped && image->reads==0 && receiver.calls16+receiver.calls32==before && receiver.policies==policies;
			Check(rejected,"callback export: zero and overflowing dimensions reject before allocation source reads or receiver calls");
			dimensionsGuarded &= rejected;
		}
		healthy(3,1,"healthy retry recovers after invalid dimensions and regions");
	}
	// Old normal RED must stop before exercising its known unsynchronized
	// shared buffer. The new preflight canary establishes this guard first.
	if(!dimensionsGuarded)return;
	struct ConcurrentReceiver final : IJobRasterizerOutput {
		const bool floats;
		const unsigned short (*expected16)[4];const float (*expected32)[4];
		std::atomic<unsigned int> active{0},maximum{0},calls{0},badPixels{0};
		ConcurrentReceiver(bool floating,const unsigned short (&words)[3][4],const float (&values)[3][4]):
			floats(floating),expected16(words),expected32(values){}
		bool PremultipliedAlpha() override{return true;}
		int GetColorSpace() override{return 0;}
		bool WantsFloat32() override{return floats;}
		void Enter() {
			const auto current=active.fetch_add(1)+1;auto prior=maximum.load();
			while(prior<current && !maximum.compare_exchange_weak(prior,current)){}
		}
		void Leave() {
			// A bounded scheduling window makes concurrent receiver entry
			// observable without any actual rendering or frame-store access.
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			++calls;--active;
		}
		void OutputImageRGBA16(const unsigned short* data,unsigned int w,unsigned int h,
			unsigned int top,unsigned int left,unsigned int bottom,unsigned int right) override {
			Enter();bool exact=!floats && w==2 && h==2 && top==0 && left==0 && bottom==1 && right==1;
			if(exact)for(unsigned int i=0;i<4;++i)for(unsigned int c=0;c<4;++c)exact &= data[i*4+c]==expected16[i%3][c];
			if(!exact)++badPixels;
			Leave();
		}
		void OutputImageRGBA32F(const float* data,unsigned int w,unsigned int h,
			unsigned int top,unsigned int left,unsigned int bottom,unsigned int right) override {
			Enter();bool exact=floats && w==2 && h==2 && top==0 && left==0 && bottom==1 && right==1;
			if(exact)for(unsigned int i=0;i<4;++i)for(unsigned int c=0;c<4;++c)exact &= data[i*4+c]==expected32[i%3][c];
			if(!exact)++badPixels;
			Leave();
		}
	};
	for(bool floating : {false,true}) {
		ConcurrentReceiver concurrent(floating,premul16,premul32);
		std::unique_ptr<IRasterizerOutput,decltype(release)> dispatcher(ForTest_CreateCallbackRasterizerOutput(concurrent),release);
		std::atomic<bool> start{false};std::atomic<unsigned int> escaped{0},reads{0};
		std::vector<std::thread> workers;
		for(unsigned int worker=0;worker<3;++worker)workers.emplace_back([&]() {
			Image local;local.Reset(2,2,floating);
			while(!start.load())std::this_thread::yield();
			try{for(unsigned int iteration=0;iteration<4;++iteration)dispatcher->OutputIntermediateImage(local,nullptr);}
			catch(...){++escaped;}
			reads.fetch_add(static_cast<unsigned int>(local.reads));
		});
		start.store(true);for(auto& worker:workers)worker.join();
		Check(escaped.load()==0 && concurrent.calls.load()==12 && concurrent.badPixels.load()==0 && reads.load()==48,
			floating?"callback export concurrent 32F: independent source images deliver exact pixels and call count":
			"callback export concurrent 16-bit: independent source images deliver exact pixels and call count");
		Check(concurrent.maximum.load()==1 && concurrent.active.load()==0,
			floating?"callback export concurrent 32F: receiver entry is serialized per adapter":
			"callback export concurrent 16-bit: receiver entry is serialized per adapter");
	}
}

static void TestRasterStorage()
{
	struct Reader final : IRasterImageReader, Reference
	{
		bool ended = false;
		unsigned int side = 1;
		bool BeginRead( unsigned int& w, unsigned int& h ) override { w = h = side; return true; }
		void ReadColor( RISEColor& color, unsigned int, unsigned int ) override
		{
			color = RISEColor( 0.125, 0.25, 0.5, 1.0 );
		}
		void EndRead() override { ended = true; }
	};
	const RISEColor blank( 0.0, 0.0, 0.0, 1.0 );
	const RISEColor marker( 0.5, 0.25, 0.125, 1.0 );
	auto* parent = new RISERasterImage( 6, 4, blank );
	parent->SetPEL( 4, 3, marker );
	auto* view = new RISERasterImage( parent, Rect( 1, 2, 4, 5 ) );
	auto* copy = new RISERasterImage( *view );
	Check( copy->GetStride() == 6 && copy->GetPEL( 2, 2 ).base.r == 0.5,
		"strided image copies retain the last row and its stride" );
	view->SetPEL( 2, 2, blank );
	Check( copy->GetPEL( 2, 2 ).base.r == 0.5 && parent->GetPEL( 4, 3 ).base.r == 0,
		"image copies are independent and views write through" );
	view->SetPEL( 2, 2, marker );
	Reader reader;
	parent->LoadImage( &reader );
	Check( reader.ended && parent->GetWidth() == 1 && parent->GetPEL( 0, 0 ).base.r == 0.125,
		"raster reload replaces dimensions and closes the reader" );
	parent->release();
	Check( view->GetPEL( 2, 2 ).base.r == 0.5, "view retains the old pixel allocation after source reload" );
	view->release();
	copy->release();

	auto* adopted = new RISERasterImage( new RISEColor[1]{marker}, 1, 1, true );
	auto* retained = new RISERasterImage( adopted );
	adopted->release();
	Check( retained->GetPEL( 0, 0 ).base.r == 0.5, "view retains adopted pixels" );
	retained->release();

	RISEColor external[] = {marker};
	auto* borrowed = new RISERasterImage( external, 1, 1, false );
	borrowed->SetPEL( 0, 0, blank );
	borrowed->release();
	Check( external[0].base.r == 0, "borrowed pixels remain caller-owned and writable" );

	struct OwningSource final : RISERasterImage
	{
		std::unique_ptr<RISEColor[]> owner;
		bool& destroyed;
		OwningSource( RISEColor* pixels, bool& ended ) :
			RISERasterImage( pixels, 1, 1, false ), owner( pixels ), destroyed( ended ) {}
		~OwningSource() override { destroyed = true; }
	};
	bool destroyed = false;
	auto* source = new OwningSource( new RISEColor[1]{marker}, destroyed );
	auto* borrowedView = new RISERasterImage( source );
	auto* nested = new RISERasterImage( borrowedView );
	source->release();
	Check( !destroyed && borrowedView->GetPEL( 0, 0 ).base.r == 0.5,
		"borrowed views retain a source that owns its external storage" );
	reader.side = 2;
	borrowedView->LoadImage( &reader );
	borrowedView->release();
	Check( !destroyed && nested->GetPEL( 0, 0 ).base.r == 0.5,
		"nested borrowed view retains its allocation after an intermediate view reloads" );
	nested->release();
	Check( destroyed, "the last borrowed view releases its source" );
}

static void TestHDRWriterBounds()
{
	auto* buffer = new MemoryBuffer();
	IRasterImageWriter* writer = nullptr;
	const bool made = RISE_API_CreateHDRWriter( &writer, *buffer, eColorSpace_Rec709RGB_Linear );
	if( !made || !writer ) {
		Check( false, "create HDR writer" );
		buffer->release();
		return;
	}
	writer->BeginWrite( 65536, 65536 );
	writer->WriteColor( RISEColor(1, 0, 0, 1), 0, 0 );
	writer->EndWrite();
	Check( buffer->getCurPos() == 0, "invalid HDR dimensions emit no partial header or pixels" );
	writer->BeginWrite( 1, 1 );
	writer->WriteColor( RISEColor(1, 0, 0, 1), 0, 0 );
	writer->EndWrite();
	const std::string bytes( buffer->Pointer(), buffer->getCurPos() );
	const std::string dimensions = "-Y 1 +X 1\n";
	const auto offset = bytes.find( dimensions );
	// Exact binary RGBE scaling stores 1.0 as mantissa128 at exponent129.
	const unsigned char expected[] = { 128, 0, 0, 129 };
	Check( offset != std::string::npos && bytes.size() == offset + dimensions.size() + sizeof(expected) &&
		std::memcmp( bytes.data() + offset + dimensions.size(), expected, sizeof(expected) ) == 0,
		"HDR writer recovers with one independently specified RGBE pixel" );
	writer->release();
	buffer->release();
}

static void TestSharedExponentPacking()
{
	const double nan=std::strtod("nan",nullptr),pinf=std::strtod("1e999",nullptr),ninf=std::strtod("-1e999",nullptr);
	const double ceiling=std::ldexp(255.0,119);
	const double below=std::ldexp(1.0,-107),above=std::ldexp(1.0,-106);
	struct Case {const char* name;RISEColor color;std::vector<unsigned char> expected;};
	const std::vector<Case> hdr={
		{"ordinary truncation",RISEColor(.6,.3,.15,1),{153,76,38,128}},
		{"fractional alpha composites over black",RISEColor(.6,.3,.15,.5),{153,76,38,127}},
		{"above-one alpha retains HDR range",RISEColor(.6,.3,.15,2),{153,76,38,129}},
		{"mixed negative clips only that channel",RISEColor(-1,.5,1,1),{0,64,128,129}},
		{"mixed NaN clips only that channel",RISEColor(nan,.5,1,1),{0,64,128,129}},
		{"positive infinity reaches format endpoint",RISEColor(pinf,.5,1,1),{255,0,0,255}},
		{"NaN RGB becomes black",RISEColor(nan,nan,nan,1),{0,0,0,0}},
		{"negative infinity RGB becomes black",RISEColor(ninf,ninf,ninf,1),{0,0,0,0}},
		{"huge finite RGB saturates exponent",RISEColor(1e300,1e300,1e300,1),{255,255,255,255}},
		{"derived upper endpoint is exact",RISEColor(ceiling,0,0,1),{255,0,0,255}},
		{"legacy below-floor black",RISEColor(below,0,0,1),{0,0,0,0}},
		{"above-floor power of two",RISEColor(above,0,0,1),{128,0,0,23}},
		{"NaN alpha composites to black",RISEColor(.6,.3,.15,nan),{0,0,0,0}},
		{"positive infinity alpha saturates after compositing",RISEColor(.6,.3,.15,pinf),{255,255,255,255}},
		{"negative infinity alpha composites to black",RISEColor(.6,.3,.15,ninf),{0,0,0,0}}
	};
	// These RGBEA bytes independently include Rec709->ROMM conversion:
	// (.6,.3,.15) -> (.437778673333857,.325255616690958,.175295367053433),
	// giving mantissas E024/A687/59C0 at exponent127. RGB is not alpha-scaled.
	const std::vector<Case> rgbea={
		{"ROMM conversion and independent fractional alpha",RISEColor(.6,.3,.15,.5),{0x24,0xe0,0x87,0xa6,0xc0,0x59,127,127}},
		{"opaque black preserves opacity",RISEColor(0,0,0,1),{0,0,0,0,0,0,0,255}},
		{"mixed negatives clip after ROMM conversion",RISEColor(1,-2,.5,.5),{0,0,0,0,0x49,0xdb,126,127}},
		{"NaN propagates through ROMM then clips",RISEColor(nan,.3,.15,.5),{0,0,0,0,0,0,0,127}},
		{"positive infinity ROMM channels saturate",RISEColor(pinf,.3,.15,.5),{255,255,255,255,255,255,255,127}},
		{"negative infinity ROMM channels clip",RISEColor(ninf,ninf,ninf,.5),{0,0,0,0,0,0,0,127}},
		{"huge finite RGB saturates exponent",RISEColor(1e300,1e300,1e300,.5),{255,255,255,255,255,255,255,127}},
		{"below-floor black retains alpha",RISEColor(below,below,below,.5),{0,0,0,0,0,0,0,127}},
		{"negative alpha clips independently",RISEColor(.6,.3,.15,-.25),{0x24,0xe0,0x87,0xa6,0xc0,0x59,127,0}},
		{"above-one alpha clips independently",RISEColor(.6,.3,.15,2),{0x24,0xe0,0x87,0xa6,0xc0,0x59,127,255}},
		{"NaN alpha clips independently",RISEColor(.6,.3,.15,nan),{0x24,0xe0,0x87,0xa6,0xc0,0x59,127,0}},
		{"positive infinity alpha saturates independently",RISEColor(.6,.3,.15,pinf),{0x24,0xe0,0x87,0xa6,0xc0,0x59,127,255}},
		{"negative infinity alpha clips independently",RISEColor(.6,.3,.15,ninf),{0x24,0xe0,0x87,0xa6,0xc0,0x59,127,0}},
		{"zero alpha retains unassociated HDR RGB",RISEColor(.6,.3,.15,0),{0x24,0xe0,0x87,0xa6,0xc0,0x59,127,0}}
	};
	for(bool alpha : {false,true}) for(const auto& item : alpha?rgbea:hdr) {
		auto* buffer=new MemoryBuffer();IRasterImageWriter* writer=nullptr;
		if(alpha)RISE_API_CreateRGBEAWriter(&writer,*buffer);
		else RISE_API_CreateHDRWriter(&writer,*buffer,eColorSpace_Rec709RGB_Linear);
		bool exact=writer!=nullptr;
		if(writer){writer->BeginWrite(1,1);writer->WriteColor(item.color,0,0);writer->EndWrite();writer->release();}
		const std::string bytes(buffer->Pointer(),buffer->getCurPos());
		const std::string resolution="-Y 1 +X 1\n";
		const auto header=bytes.find(resolution);
		exact &= header!=std::string::npos && bytes.size()==header+resolution.size()+item.expected.size();
		if(exact) {
			const auto* pixel=reinterpret_cast<const unsigned char*>(bytes.data()+header+resolution.size());
			exact=std::memcmp(pixel,item.expected.data(),item.expected.size())==0;
			// Independently decode wire mantissas at their quantization-bin
			// lower edge; do not call either production reader/packing helper.
			const unsigned int bits=alpha?16:8,exponent=pixel[alpha?6:3];
			for(unsigned int channel=0;channel<3;++channel) {
				const unsigned int m=alpha?unsigned(pixel[channel*2])+(unsigned(pixel[channel*2+1])<<8):pixel[channel];
				const double decoded=exponent?std::ldexp(double(m),int(exponent)-128-int(bits)):0;
				const double largest=std::ldexp(double((1u<<bits)-1u),127-int(bits));
				exact &= IsFiniteDouble(decoded) && decoded>=0 && decoded<=largest;
			}
			if(alpha)exact &= pixel[7]/255.0==item.expected[7]/255.0;
		}
		Check(exact,(std::string("shared exponent packing ")+(alpha?"RGBEA: ":"HDR: ")+item.name).c_str());
		buffer->release();
	}
}

static void TestHDRRawScanlineMarker()
{
	auto* buffer=new MemoryBuffer();IRasterImageWriter* writer=nullptr;
	RISE_API_CreateHDRWriter(&writer,*buffer,eColorSpace_Rec709RGB_Linear);
	if(!writer){Check(false,"HDR raw scanline: exact scale avoids false RLE marker and matches independent pixel expectations");buffer->release();return;}
	writer->BeginWrite(8,1);
	for(unsigned int x=0;x<8;++x)writer->WriteColor(RISEColor(.02,.02,1,1),x,0);
	writer->EndWrite();writer->release();
	const std::string bytes(buffer->Pointer(),buffer->getCurPos());
	const std::string resolution="-Y 1 +X 8\n";
	const auto offset=bytes.find(resolution);
	const unsigned char expected[]={2,2,128,129};
	bool exact=offset!=std::string::npos && bytes.size()==offset+resolution.size()+8*sizeof(expected);
	if(exact)for(unsigned int x=0;x<8;++x)
		exact &= std::memcmp(bytes.data()+offset+resolution.size()+x*sizeof(expected),expected,sizeof(expected))==0;
	buffer->seek(IBuffer::START,0);auto* reader=new HDRReader(*buffer);
	unsigned int width=0,height=0;const bool decoded=reader->BeginRead(width,height);
	exact &= decoded && width==8 && height==1;
	if(decoded && width==8 && height==1)for(unsigned int x=0;x<8;++x) {
		RISEColor actual;reader->ReadColor(actual,x,0);
		// HDRReader reconstructs bin centers; these expectations are
		// independently specified as (m+.5)*2^-7.
		exact &= actual.base.r==0.01953125 && actual.base.g==0.01953125 && actual.base.b==1.00390625 && actual.a==1;
	}
	Check(exact,"HDR raw scanline: exact scale avoids false RLE marker and matches independent pixel expectations");
	reader->release();buffer->release();
}

static void TestPPMWriterBounds()
{
	const auto reject=[](unsigned int width,unsigned int height) {
		auto* output=new MemoryBuffer();output->setBytes("keep",4);
		auto* writer=new PPMWriter(*output,eColorSpace_Rec709RGB_Linear);
		bool rejected=false;
		try{writer->BeginWrite(width,height);}
		catch(const std::invalid_argument&){rejected=true;}
		catch(const std::length_error&){rejected=true;}
		Check(rejected && output->getCurPos()==4 && output->Size()==4 && std::memcmp(output->Pointer(),"keep",4)==0,
			"PPM writer bounds: invalid dimensions reject before any output mutation");
		writer->release();output->release();return rejected;
	};
	const bool validated=reject(65536,65536);
	reject(0,1);reject(1,0);
	// This product fits UINT_MAX, but its P6 header does not. Avoid the old
	// multi-GiB allocation until ordinary overflow rejection is established.
	if(validated)reject(std::numeric_limits<unsigned int>::max()/3,1);
	const auto wire=[](const MemoryBuffer& output,unsigned int width,unsigned int height,
		const std::vector<unsigned char>& pixels) {
		const std::string header="P6\n"+std::to_string(width)+" "+std::to_string(height)+"\n255\n";
		return output.getCurPos()==header.size()+pixels.size() && output.Size()==output.getCurPos() &&
			std::memcmp(output.Pointer(),header.data(),header.size())==0 &&
			std::memcmp(output.Pointer()+header.size(),pixels.data(),pixels.size())==0;
	};
	auto* output=new MemoryBuffer();auto* writer=new PPMWriter(*output,eColorSpace_Rec709RGB_Linear);
	if(validated) {
		writer->EndWrite();writer->WriteColor(RISEColor(1,1,1,1),0,0);
		Check(output->getCurPos()==0,"PPM writer bounds: no active image emits no bytes");
	}
	const std::vector<unsigned char> expected={255,0,0, 0,255,0, 0,0,255, 63,127,191};
	for(unsigned int mode=0;mode<3;++mode) {
		output->Resize(0,true);output->seek(IBuffer::START,0);
		writer->BeginWrite(2,2);
		writer->WriteColor(RISEColor(1,0,0,.5),0,0);
		writer->WriteColor(RISEColor(0,1,0,1),1,0);
		// The old ignored helper call also divides by alpha. Its zero-alpha
		// case is exercised only after the new bounds gate is established.
		writer->WriteColor(RISEColor(0,0,1,validated?0:1),0,1);
		writer->WriteColor(RISEColor(.25,.5,.75,.25),1,1);
		// Both old-code writes alias initialized in-bounds pixels; neither
		// deliberately performs an out-of-allocation write during RED proof.
		if(mode==1)writer->WriteColor(RISEColor(1,1,1,1),2,0);
		if(mode==2)writer->WriteColor(RISEColor(1,1,1,1),0,0x80000000U);
		writer->EndWrite();
		Check(wire(*output,2,2,expected),mode==0?
			"PPM writer bounds: independent P6 bytes preserve RGB truncation and ignore zero fractional opaque alpha":mode==1?
			"PPM writer bounds: x at width cannot alias a pixel on the following row":
			"PPM writer bounds: huge y cannot wrap the index onto an existing pixel");
	}
	output->Resize(0,true);output->seek(IBuffer::START,0);
	writer->BeginWrite(1,1);writer->WriteColor(RISEColor(.5,.25,1,.5),0,0);writer->EndWrite();
	Check(wire(*output,1,1,{127,63,255}),"PPM writer bounds: repeated BeginWrite replaces dimensions and pixels");
	const auto before=output->getCurPos();bool rejected=false;
	try{writer->BeginWrite(0,1);}catch(const std::invalid_argument&){rejected=true;}
	if(rejected){writer->WriteColor(RISEColor(1,1,1,1),0,0);writer->EndWrite();}
	Check(rejected && output->getCurPos()==before,"PPM writer bounds: rejected replacement deactivates previous pixels without output");
	output->Resize(0,true);output->seek(IBuffer::START,0);
	writer->BeginWrite(1,1);writer->WriteColor(RISEColor(.5,.25,1,validated?0:1),0,0);writer->EndWrite();
	Check(wire(*output,1,1,{127,63,255}),"PPM writer bounds: healthy retry recovers after an invalid replacement");
	writer->release();output->release();
	for(unsigned int capacity : {10u,12u}) {
		char storage[12]={};auto* borrowed=new MemoryBuffer(storage,capacity,false);
		auto* failed=new PPMWriter(*borrowed,eColorSpace_Rec709RGB_Linear);bool reported=false;
		try{failed->BeginWrite(1,1);failed->WriteColor(RISEColor(1,0,0,1),0,0);failed->EndWrite();}
		catch(const std::runtime_error&){reported=true;}
		Check(reported && borrowed->getCurPos()==(capacity==10?0u:11u),
			"PPM writer bounds: header and final pixel failures surface as exceptions");
		failed->release();borrowed->release();
	}
}

static void TestTGAWriterBounds()
{
	bool supportedBudget = true;
	try { TGAWriter::ValidateDimensions(65535, 16384); }
	catch( const std::exception& ) { supportedBudget = false; }
	bool oversizedBudget = false;
	try { TGAWriter::ValidateDimensions(65535, 16385); }
	catch( const std::length_error& ) { oversizedBudget = true; }
	Check(supportedBudget && oversizedBudget,
		"TGA preflight checks the complete 32-bit output budget without allocating pixel storage");
	auto rejectDimensions = []( unsigned int width, unsigned int height ) {
		auto* buffer = new MemoryBuffer();
		auto* writer = new TGAWriter( *buffer, eColorSpace_Rec709RGB_Linear );
		bool rejected = false;
		try { writer->BeginWrite(width, height); }
		catch( const std::invalid_argument& ) { rejected = true; }
		catch( const std::length_error& ) { rejected = true; }
		Check( rejected && buffer->getCurPos() == 0,
			"TGA writer: invalid dimensions throw before emitting any header" );
		writer->release();
		buffer->release();
		return rejected;
	};
	const bool widthRejected = rejectDimensions(65536, 1);
	const bool heightRejected = rejectDimensions(1, 65536);
	rejectDimensions(0, 1);
	rejectDimensions(1, 0);
	// The old writer allocates gigabytes for this overflow case. First prove
	// that dimension validation exists, keeping the baseline RED run bounded.
	const bool validated = widthRejected && heightRejected;
	if( validated ) rejectDimensions(65535, 65535);

	for( bool wide : { false, true } ) {
		const unsigned int width = wide ? 65535 : 1;
		const unsigned int height = wide ? 1 : 65535;
		auto* buffer = new MemoryBuffer();
		auto* writer = new TGAWriter( *buffer, eColorSpace_Rec709RGB_Linear );
		writer->BeginWrite(width, height);
		const RISEColor black(0, 0, 0, 1);
		for( unsigned int y = 0; y < height; ++y ) {
			for( unsigned int x = 0; x < width; ++x ) writer->WriteColor(black, x, y);
		}
		writer->EndWrite();
		const unsigned char expectedHeader[] = {
			0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			static_cast<unsigned char>(wide ? 255 : 1), static_cast<unsigned char>(wide ? 255 : 0),
			static_cast<unsigned char>(wide ? 1 : 255), static_cast<unsigned char>(wide ? 0 : 255), 32, 8
		};
		bool exact = buffer->getCurPos() == 18 + 4 * 65535 &&
			std::memcmp(buffer->Pointer(), expectedHeader, sizeof(expectedHeader)) == 0;
		const auto* bytes = reinterpret_cast<const unsigned char*>(buffer->Pointer());
		for( unsigned int offset = 18; exact && offset < buffer->getCurPos(); offset += 4 ) {
			exact = bytes[offset] == 0 && bytes[offset+1] == 0 && bytes[offset+2] == 0 && bytes[offset+3] == 255;
		}
		Check( exact, "TGA writer: 65535-axis image preserves exact header, byte count, and BGRA pixels" );
		writer->release();
		buffer->release();
	}

	if( !validated ) return; // Avoid exercising known unsafe baseline paths.
	auto* buffer = new MemoryBuffer();
	auto* writer = new TGAWriter( *buffer, eColorSpace_Rec709RGB_Linear );
	writer->EndWrite();
	Check( buffer->getCurPos() == 0, "TGA writer: EndWrite before BeginWrite emits no bytes" );
	writer->BeginWrite(2, 1);
	writer->WriteColor(RISEColor(1, 0, 0, 1), 0, 0);
	buffer->seek(IBuffer::START, 0);
	writer->BeginWrite(1, 1);
	writer->WriteColor(RISEColor(0, 0, 0, 1), 0, 0);
	writer->WriteColor(RISEColor(1, 1, 1, 1), 1, 0);
	writer->WriteColor(RISEColor(1, 1, 1, 1), 0, 1);
	writer->EndWrite();
	const unsigned char expected[] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 32, 8, 0, 0, 0, 255 };
	Check( buffer->getCurPos() == sizeof(expected) && std::memcmp(buffer->Pointer(), expected, sizeof(expected)) == 0,
		"TGA writer: repeated BeginWrite resets pixels and invalid coordinates cannot alter output" );
	writer->BeginWrite(1, 1);
	const auto before = buffer->getCurPos();
	bool rejected = false;
	try { writer->BeginWrite(0, 1); }
	catch( const std::invalid_argument& ) { rejected = true; }
	writer->WriteColor(RISEColor(1, 1, 1, 1), 0, 0);
	writer->EndWrite();
	Check( rejected && buffer->getCurPos() == before,
		"TGA writer: invalid replacement clears previous active image" );
	writer->release();
	buffer->release();

	for( unsigned int capacity : { 17U, 20U } ) {
		char storage[20] = {};
		auto* borrowed = new MemoryBuffer(storage, capacity, false);
		auto* failed = new TGAWriter( *borrowed, eColorSpace_Rec709RGB_Linear );
		bool reported = false;
		try {
			failed->BeginWrite(1, 1);
			failed->WriteColor(RISEColor(0, 0, 0, 1), 0, 0);
			failed->EndWrite();
		} catch( const std::runtime_error& ) { reported = true; }
		if( capacity == 17 ) failed->EndWrite();
		Check( reported && borrowed->getCurPos() == (capacity == 17 ? 0U : 18U),
			"TGA writer: header and pixel write failures surface as exceptions without an active failed header" );
		failed->release();
		borrowed->release();
	}
}

//----------------------------------------------------------------------
// Byte-buffer helpers.
//----------------------------------------------------------------------
static void AppendStr( std::vector<char>& v, const char* s )
{
	for( const char* p = s; *p; ++p ) v.push_back( *p );
}

static void AppendByte( std::vector<char>& v, unsigned char b )
{
	v.push_back( static_cast<char>( b ) );
}

static std::string TmpDir()
{
	const char* tmp = getenv( "TMPDIR" );
	std::string dir = tmp ? std::string( tmp ) : ( RISE::TestSupport::ScratchBaseDir() + "/" );
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	return dir;
}

static std::vector<char> ReadFileBytes( const std::string& path )
{
	std::ifstream f( path.c_str(), std::ios::binary );
	return std::vector<char>( (std::istreambuf_iterator<char>( f )), std::istreambuf_iterator<char>() );
}

static std::filesystem::path JPEGFixture()
{
#ifdef RISE_TEST_SOURCE_ROOT
	const std::filesystem::path root(RISE_TEST_SOURCE_ROOT);
#else
	// Direct compiler invocations can provide an absolute source filename;
	// repository builds supply the absolute root even with relative __FILE__.
	const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
	if( !root.is_absolute() ) throw std::runtime_error("ImageReaderOverflowTest requires RISE_TEST_SOURCE_ROOT or absolute __FILE__");
#endif
	return root / "scenes/Tests/Painters/textures/test_pattern.jpg";
}

static void TestJPEGReload()
{
	const auto sameColor = [](const RISEColor& a, const RISEColor& b) {
		return a.base.r == b.base.r && a.base.g == b.base.g && a.base.b == b.base.b && a.a == b.a;
	};
	const auto fixture = JPEGFixture();
	Check( std::filesystem::is_regular_file(fixture), "JPEG reload: locate shipped JPEG fixture" );
	if( !std::filesystem::is_regular_file(fixture) ) return;

	auto* buffer = new MemoryBuffer( fixture.string().c_str() );
	auto* reader = new JPEGReader( *buffer, eColorSpace_Rec709RGB_Linear );
	unsigned int width = 0, height = 0;
	const bool decoded = reader->BeginRead( width, height );
	Check( decoded && width == 16 && height == 16, "JPEG reload: decode the shipped 16x16 fixture" );
	if( decoded && width == 16 && height == 16 ) {
		std::vector<RISEColor> expected( width * height );
		for( unsigned int y = 0; y < height; ++y ) {
			for( unsigned int x = 0; x < width; ++x ) reader->ReadColor( expected[y*width+x], x, y );
		}
		for( unsigned int reload = 0; reload < 4; ++reload ) {
			const bool reloaded = reader->BeginRead(width, height);
			bool identical = reloaded && width == 16 && height == 16;
			if( identical ) {
				for( unsigned int y = 0; y < height; ++y ) {
					for( unsigned int x = 0; x < width; ++x ) {
						RISEColor pixel;
						reader->ReadColor( pixel, x, y );
						identical &= sameColor(pixel, expected[y*width+x]);
					}
				}
			}
			Check( identical, "JPEG reload: repeated BeginRead preserves every pixel and dimensions without caller rewind" );
		}

		Check( buffer->Resize(0, true), "JPEG reload: replace the source with an empty input" );
		width = 123;
		height = 456;
		Check( !reader->BeginRead(width, height) && width == 0 && height == 0,
			"JPEG reload: failed decode clears output dimensions" );
		const RISEColor poison( -1000, -2000, -3000, -4000 );
		RISEColor pixel = poison;
		reader->ReadColor( pixel, 0, 0 );
		Check( sameColor(pixel, poison), "JPEG reload: failed decode cannot read the previous image" );
	}
	reader->release();
	buffer->release();
}


static void TestJPEGCompleteStream()
{
	const auto encoded=ReadFileBytes(JPEGFixture().string());
	Check(encoded.size()>200,"JPEG complete stream: shipped fixture has entropy for tail truncation");
	if(encoded.size()<=200) return;
	auto* input=new MemoryBuffer(static_cast<unsigned int>(encoded.size()));
	input->setBytes(encoded.data(),static_cast<unsigned int>(encoded.size()));input->seek(IBuffer::START,0);
	auto* reader=new JPEGReader(*input,eColorSpace_Rec709RGB_Linear);
	unsigned int width=0,height=0;
	const bool opened=reader->BeginRead(width,height) && width==16 && height==16;
	Check(opened,"JPEG complete stream: complete baseline JPEG decodes");
	const RISEColor poison(-1000,-2000,-3000,-4000);
	std::vector<RISEColor> expected(256,poison);
	if(opened) for(unsigned int i=0;i<256;++i) reader->ReadColor(expected[i],i%16,i/16);
	const auto same=[](const RISEColor& a,const RISEColor& b) {
		return a.base.r==b.base.r && a.base.g==b.base.g && a.base.b==b.base.b && a.a==b.a;
	};
	const auto install=[&](const std::vector<char>& bytes) {
		input->Resize(static_cast<unsigned int>(bytes.size()),true);input->seek(IBuffer::START,0);
		input->setBytes(bytes.data(),static_cast<unsigned int>(bytes.size()));
	};
	const auto matches=[&]() {
		bool exact=reader->BeginRead(width,height) && width==16 && height==16;
		if(exact) for(unsigned int i=0;i<256;++i) {
			RISEColor actual=poison;reader->ReadColor(actual,i%16,i/16);exact &= same(actual,expected[i]);
		}
		return opened && exact;
	};
	for(unsigned int missing : {1,2,120,200}) {
		auto truncated=encoded;truncated.resize(truncated.size()-missing);install(truncated);
		width=123;height=456;
		const bool rejected=!reader->BeginRead(width,height);
		RISEColor stale=poison;reader->ReadColor(stale,0,0);
		Check(rejected && width==0 && height==0 && same(stale,poison),
			"JPEG complete stream: truncated entropy clears dimensions and previous pixels");
		install(encoded);
		Check(matches(),"JPEG complete stream: healthy input recovers every pixel after entropy truncation");
	}
	// An apparent EOI inside a length-delimited APP segment must not conceal
	// the missing entropy tail. The same segment is legal in a complete image.
	auto fakeEnd=encoded;
	const char app[] = {char(0xff),char(0xe1),0,6,'x',char(0xff),char(0xd9),'y'};
	fakeEnd.insert(fakeEnd.begin()+2,std::begin(app),std::end(app));
	install(fakeEnd);
	Check(matches(),"JPEG complete stream: APP payload containing an EOI byte pair preserves pixels");
	fakeEnd.resize(fakeEnd.size()-120);install(fakeEnd);
	Check(!reader->BeginRead(width,height) && width==0 && height==0,
		"JPEG complete stream: APP payload EOI cannot hide a truncated entropy tail");
	auto trailing=encoded;
	for(unsigned char byte : {0x00,0xff,0xd8,0x12}) trailing.push_back(static_cast<char>(byte));
	install(trailing);
	Check(matches(),"JPEG complete stream: legal bytes after EOI preserve every pixel");
	auto malformed=encoded;malformed[4]=char(0xff);malformed[5]=char(0xff);install(malformed);
	Check(!reader->BeginRead(width,height) && width==0 && height==0,
		"JPEG complete stream: segment extending past physical input is rejected");
	reader->release();input->release();

	// Independent solid-color fixtures generated with Pillow 12.3/libjpeg:
	// quality=90, subsampling=0; progressive=True or restart_marker_blocks=1.
	// Their expected decoded RGB is exactly (64,128,192), alpha opaque.
	static const unsigned char progressive[] = {
		0xff,0xd8,0xff,0xe0,0x00,0x10,0x4a,0x46,0x49,0x46,0x00,0x01,0x01,0x00,0x00,0x01,0x00,0x01,0x00,0x00,
		0xff,0xdb,0x00,0x43,0x00,0x03,0x02,0x02,0x03,0x02,0x02,0x03,0x03,0x03,0x03,0x04,0x03,0x03,0x04,0x05,
		0x08,0x05,0x05,0x04,0x04,0x05,0x0a,0x07,0x07,0x06,0x08,0x0c,0x0a,0x0c,0x0c,0x0b,0x0a,0x0b,0x0b,0x0d,
		0x0e,0x12,0x10,0x0d,0x0e,0x11,0x0e,0x0b,0x0b,0x10,0x16,0x10,0x11,0x13,0x14,0x15,0x15,0x15,0x0c,0x0f,
		0x17,0x18,0x16,0x14,0x18,0x12,0x14,0x15,0x14,0xff,0xdb,0x00,0x43,0x01,0x03,0x04,0x04,0x05,0x04,0x05,
		0x09,0x05,0x05,0x09,0x14,0x0d,0x0b,0x0d,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,
		0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,
		0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0xff,0xc2,
		0x00,0x11,0x08,0x00,0x10,0x00,0x10,0x03,0x01,0x11,0x00,0x02,0x11,0x01,0x03,0x11,0x01,0xff,0xc4,0x00,
		0x15,0x00,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x05,
		0xff,0xc4,0x00,0x15,0x01,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x00,0x06,0xff,0xda,0x00,0x0c,0x03,0x01,0x00,0x02,0x10,0x03,0x10,0x00,0x00,0x01,0x9f,0x73,0x1a,
		0x00,0xff,0xc4,0x00,0x14,0x10,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x00,0x20,0xff,0xda,0x00,0x08,0x01,0x01,0x00,0x01,0x05,0x02,0x1f,0xff,0xc4,0x00,0x14,0x11,0x01,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x20,0xff,0xda,0x00,0x08,
		0x01,0x03,0x01,0x01,0x3f,0x01,0x1f,0xff,0xc4,0x00,0x14,0x11,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x20,0xff,0xda,0x00,0x08,0x01,0x02,0x01,0x01,0x3f,0x01,0x1f,
		0xff,0xc4,0x00,0x14,0x10,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x20,0xff,0xda,0x00,0x08,0x01,0x01,0x00,0x06,0x3f,0x02,0x1f,0xff,0xc4,0x00,0x14,0x10,0x01,0x00,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x20,0xff,0xda,0x00,0x08,0x01,
		0x01,0x00,0x01,0x3f,0x21,0x1f,0xff,0xda,0x00,0x0c,0x03,0x01,0x00,0x02,0x00,0x03,0x00,0x00,0x00,0x10,
		0x6d,0xbf,0xff,0xc4,0x00,0x14,0x11,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,0x20,0xff,0xda,0x00,0x08,0x01,0x03,0x01,0x01,0x3f,0x10,0x1f,0xff,0xc4,0x00,0x14,0x11,
		0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x20,0xff,0xda,0x00,
		0x08,0x01,0x02,0x01,0x01,0x3f,0x10,0x1f,0xff,0xc4,0x00,0x14,0x10,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x20,0xff,0xda,0x00,0x08,0x01,0x01,0x00,0x01,0x3f,0x10,
		0x1f,0xff,0xd9,
	};
	static const unsigned char restart[] = {
		0xff,0xd8,0xff,0xe0,0x00,0x10,0x4a,0x46,0x49,0x46,0x00,0x01,0x01,0x00,0x00,0x01,0x00,0x01,0x00,0x00,
		0xff,0xdb,0x00,0x43,0x00,0x03,0x02,0x02,0x03,0x02,0x02,0x03,0x03,0x03,0x03,0x04,0x03,0x03,0x04,0x05,
		0x08,0x05,0x05,0x04,0x04,0x05,0x0a,0x07,0x07,0x06,0x08,0x0c,0x0a,0x0c,0x0c,0x0b,0x0a,0x0b,0x0b,0x0d,
		0x0e,0x12,0x10,0x0d,0x0e,0x11,0x0e,0x0b,0x0b,0x10,0x16,0x10,0x11,0x13,0x14,0x15,0x15,0x15,0x0c,0x0f,
		0x17,0x18,0x16,0x14,0x18,0x12,0x14,0x15,0x14,0xff,0xdb,0x00,0x43,0x01,0x03,0x04,0x04,0x05,0x04,0x05,
		0x09,0x05,0x05,0x09,0x14,0x0d,0x0b,0x0d,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,
		0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,
		0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0xff,0xc0,
		0x00,0x11,0x08,0x00,0x10,0x00,0x10,0x03,0x01,0x11,0x00,0x02,0x11,0x01,0x03,0x11,0x01,0xff,0xc4,0x00,
		0x1f,0x00,0x00,0x01,0x05,0x01,0x01,0x01,0x01,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,
		0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0xff,0xc4,0x00,0xb5,0x10,0x00,0x02,0x01,0x03,0x03,
		0x02,0x04,0x03,0x05,0x05,0x04,0x04,0x00,0x00,0x01,0x7d,0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,
		0x31,0x41,0x06,0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,
		0x52,0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,0x29,
		0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,
		0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,
		0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,
		0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,
		0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,
		0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,0xff,0xc4,0x00,0x1f,0x01,0x00,0x03,
		0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x02,0x03,0x04,0x05,
		0x06,0x07,0x08,0x09,0x0a,0x0b,0xff,0xc4,0x00,0xb5,0x11,0x00,0x02,0x01,0x02,0x04,0x04,0x03,0x04,0x07,
		0x05,0x04,0x04,0x00,0x01,0x02,0x77,0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,
		0x07,0x61,0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,0x15,
		0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,0x27,0x28,0x29,0x2a,0x35,
		0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,
		0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,
		0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,
		0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,
		0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,
		0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,0xff,0xdd,0x00,0x04,0x00,0x01,0xff,0xda,0x00,0x0c,0x03,
		0x01,0x00,0x02,0x11,0x03,0x11,0x00,0x3f,0x00,0xe7,0xeb,0xf7,0x33,0xf1,0xc3,0xff,0xd0,0xe7,0xeb,0xf7,
		0x33,0xf1,0xc3,0xff,0xd1,0xe7,0xeb,0xf7,0x33,0xf1,0xc3,0xff,0xd2,0xe7,0xeb,0xf7,0x33,0xf1,0xc3,0xff,
		0xd9,
	};
	for(const auto& fixture : {std::vector<unsigned char>(std::begin(progressive),std::end(progressive)),
		std::vector<unsigned char>(std::begin(restart),std::end(restart))}) {
		auto bytes=std::vector<char>(fixture.begin(),fixture.end());
		auto* buffer=new MemoryBuffer(bytes.data(),static_cast<unsigned int>(bytes.size()),false);
		auto* decoder=new JPEGReader(*buffer,eColorSpace_Rec709RGB_Linear);
		RISEColor solid;solid.SetFromIntegerized<Rec709RGBPel,unsigned char>({64,128,192,255},255.0);
		bool exact=decoder->BeginRead(width,height) && width==16 && height==16;
		if(exact) for(unsigned int i=0;i<256;++i) {
			RISEColor actual=poison;decoder->ReadColor(actual,i%16,i/16);exact &= same(actual,solid);
		}
		Check(exact,"JPEG complete stream: progressive and restart-coded fixtures preserve every expected pixel");
		decoder->release();buffer->release();
	}
}

//----------------------------------------------------------------------
// Reader drivers.  Each builds a MemoryBuffer over the crafted bytes (no
// ownership; the caller's vector outlives the reader), runs BeginRead, and
// tears everything down so a leak / double-free would surface here.
//----------------------------------------------------------------------
static bool RunTGA( std::vector<char>& bytes, unsigned int& w, unsigned int& h,
					RISEColor* px0 = 0, RISEColor* pxCorner = 0 )
{
	w = h = 0;
	MemoryBuffer* mb = new MemoryBuffer( bytes.data(), static_cast<unsigned int>( bytes.size() ), false );
	TGAReader* reader = new TGAReader( *mb, eColorSpace_Rec709RGB_Linear );
	const bool ok = reader->BeginRead( w, h );
	if( ok && px0 && w >= 1 && h >= 1 )           reader->ReadColor( *px0, 0, 0 );
	if( ok && pxCorner && w >= 1 && h >= 1 )       reader->ReadColor( *pxCorner, w-1, h-1 );
	reader->release();
	mb->release();
	return ok;
}

static bool RunHDR( std::vector<char>& bytes, unsigned int& w, unsigned int& h,
					RISEColor* px0 = 0, RISEColor* pxLast = 0 )
{
	w = h = 0;
	MemoryBuffer* mb = new MemoryBuffer( bytes.data(), static_cast<unsigned int>( bytes.size() ), false );
	HDRReader* reader = new HDRReader( *mb );
	const bool ok = reader->BeginRead( w, h );
	if( ok && px0 && w >= 1 )        reader->ReadColor( *px0,   0,   0 );
	if( ok && pxLast && w >= 1 )     reader->ReadColor( *pxLast, w-1, 0 );
	reader->release();
	mb->release();
	return ok;
}

#ifndef NO_PNG_SUPPORT
static bool RunPNG( std::vector<char>& bytes, unsigned int& w, unsigned int& h, RISEColor* pixel = nullptr )
{
	w = h = 0;
	MemoryBuffer* mb = new MemoryBuffer( bytes.data(), static_cast<unsigned int>( bytes.size() ), false );
	PNGReader* reader = new PNGReader( *mb, eColorSpace_Rec709RGB_Linear );
	const bool ok = reader->BeginRead( w, h );
	if( ok && pixel ) reader->ReadColor( *pixel, 0, 0 );
	reader->release();
	mb->release();
	return ok;
}
#endif

#ifndef NO_TIFF_SUPPORT
static bool RunTIFF( std::vector<char>& bytes, unsigned int& w, unsigned int& h,
					 RISEColor* px0 = 0 )
{
	w = h = 0;
	struct DriverBuffer final : MemoryBuffer {
		bool& destroyed;
		DriverBuffer(std::vector<char>& data, bool& flag) :
			MemoryBuffer(data.data(), static_cast<unsigned int>(data.size()), false), destroyed(flag) {}
		~DriverBuffer() override { destroyed = true; }
	};
	struct DriverReader final : TIFFReader {
		bool& destroyed;
		DriverReader(IReadBuffer& input, bool& flag) :
			TIFFReader(input, eColorSpace_Rec709RGB_Linear), destroyed(flag) {}
		~DriverReader() override { destroyed = true; }
	};
	bool bufferDestroyed = false, readerDestroyed = false;
	auto* mb = new DriverBuffer(bytes, bufferDestroyed);
	auto* reader = new DriverReader(*mb, readerDestroyed);
	const bool ok = reader->BeginRead( w, h );
	if( ok && px0 && w >= 1 && h >= 1 )           reader->ReadColor( *px0, 0, 0 );
	reader->release();
	Check(readerDestroyed && mb->refcount() == 1,
		"TIFF driver teardown: reader destruction releases its buffer reference");
	mb->release();
	Check(bufferDestroyed, "TIFF driver teardown: borrowed buffer wrapper is destroyed");
	return ok;
}
#endif

//======================================================================
// TGA
//======================================================================
static std::vector<char> MakeTGAHeader( unsigned short w, unsigned short h, unsigned char bpp )
{
	std::vector<char> v;
	for( int i = 0; i < 12; ++i ) AppendByte( v, 0 );		// 12-byte preheader
	v[2] = 2; // Uncompressed truecolor image.
	AppendByte( v, (unsigned char)( w & 0xFF ) );
	AppendByte( v, (unsigned char)( (w >> 8) & 0xFF ) );
	AppendByte( v, (unsigned char)( h & 0xFF ) );
	AppendByte( v, (unsigned char)( (h >> 8) & 0xFF ) );
	AppendByte( v, bpp );
	AppendByte( v, 8 ); // Eight alpha bits; bottom-left origin.
	return v;
}

static void TestTGA()
{
	std::cout << "A: TGA overflow / zero / valid dimensions" << std::endl;
	unsigned int w, h;

	// 32768*32768*4 == 2^32 -> wraps the 32-bit product to 0 -> every write OOB.
	std::vector<char> over = MakeTGAHeader( 0x8000, 0x8000, 32 );
	Check( !RunTGA( over, w, h ), "A: overflow-dimension TGA returns false" );

	std::vector<char> zw = MakeTGAHeader( 0, 4, 32 );
	Check( !RunTGA( zw, w, h ), "A: zero-width TGA returns false" );
	std::vector<char> zh = MakeTGAHeader( 4, 0, 32 );
	Check( !RunTGA( zh, w, h ), "A: zero-height TGA returns false" );

	// Independently specified BGRA pixels follow the complete 18-byte header.
	// Both rows have R>G>B and full alpha.
	std::vector<char> good = MakeTGAHeader( 2, 2, 32 );
	for( int p = 0; p < 4; ++p ) { AppendByte( good, 10 ); AppendByte( good, 100 ); AppendByte( good, 200 ); AppendByte( good, 255 ); }
	RISEColor tga0, tgaCorner;
	const bool tgaOk = RunTGA( good, w, h, &tga0, &tgaCorner );
	Check( tgaOk && w == 2 && h == 2, "A: valid 2x2 TGA decodes" );
	Check( tgaOk && tga0.base.r > tga0.base.g && tga0.base.g > tga0.base.b && tga0.base.b > 0.0,
		"A: TGA pixel (0,0) decodes R>G>B (channel order)" );
	Check( tgaOk && tgaCorner.base.r > tgaCorner.base.g && tgaCorner.base.g > tgaCorner.base.b,
		"A: TGA pixel (1,1) decodes R>G>B (backwards-y row offset)" );
}

static void TestTGAFormat()
{
	const RGBA8 samples[] = { {200, 100, 10, 64}, {20, 210, 50, 128},
		{60, 70, 220, 192}, {230, 90, 80, 255} };
	// Explicit file-order tables for bottom-left, bottom-right, top-left,
	// and top-right origins. Samples themselves are always in top-left order.
	const unsigned int orders[4][4] = { {2, 3, 0, 1}, {3, 2, 1, 0}, {0, 1, 2, 3}, {1, 0, 3, 2} };
	auto sameColor = []( const RISEColor& actual, const RISEColor& expected ) {
		return std::fabs(actual.base.r - expected.base.r) < 1e-12 &&
			std::fabs(actual.base.g - expected.base.g) < 1e-12 &&
			std::fabs(actual.base.b - expected.base.b) < 1e-12 && std::fabs(actual.a - expected.a) < 1e-12;
	};
	std::vector<char> valid;
	bool checkedLayout = false;
	for( unsigned int origin = 0; origin < 4; ++origin ) {
		for( unsigned char alphaBits : { 0, 8 } ) {
			auto bytes = MakeTGAHeader(2, 2, 32);
			bytes[0] = 3;
			bytes[17] = static_cast<char>(origin * 0x10 + alphaBits);
			for( unsigned char idByte : { 'I', 'D', '!' } ) AppendByte(bytes, idByte);
			for( const auto index : orders[origin] ) {
				const auto& pixel = samples[index];
				for( unsigned char channel : { pixel.b, pixel.g, pixel.r, pixel.a } ) AppendByte(bytes, channel);
			}
			auto* buffer = new MemoryBuffer(bytes.data(), static_cast<unsigned int>(bytes.size()), false);
			auto* reader = new TGAReader(*buffer, eColorSpace_Rec709RGB_Linear);
			unsigned int width = 0, height = 0;
			bool exact = reader->BeginRead(width, height) && width == 2 && height == 2;
			if( exact ) {
				for( unsigned int i = 0; i < 4; ++i ) {
					RISEColor actual, expected;
					reader->ReadColor(actual, i % 2, i / 2);
					auto sample = samples[i];
					if( alphaBits == 0 ) sample.a = 255;
					expected.SetFromIntegerized<Rec709RGBPel, unsigned char>(sample, 255.0);
					exact &= sameColor(actual, expected);
				}
			}
			Check( exact, "TGA reader: 18-byte BGRA header, ID, both origin axes, and descriptor alpha decode exactly" );
			if( exact ) {
				const RISEColor poison(-1000, -2000, -3000, -4000);
				RISEColor outside = poison;
				reader->ReadColor(outside, 2, 0);
				reader->ReadColor(outside, 0, 2);
				Check( sameColor(outside, poison), "TGA reader: out-of-range coordinates preserve caller color" );
			}
			checkedLayout |= exact;
			reader->release();
			buffer->release();
			valid = bytes;
		}
	}
	unsigned int width = 123, height = 456;
	for( unsigned char mapDepth : {15, 16, 24, 32} ) {
		auto mapped = MakeTGAHeader(1, 1, 32);
		mapped[0] = 3; mapped[1] = 1; mapped[3] = 4; mapped[5] = 2; mapped[7] = mapDepth;
		mapped[17] = 0x28;
		for( unsigned char id : {'I','D','!'} ) AppendByte(mapped,id);
		const unsigned int mapBytes = 2 * ((mapDepth + 7) / 8);
		for( unsigned int i=0; i<mapBytes; ++i ) AppendByte(mapped,static_cast<unsigned char>(90+i));
		for( unsigned char channel : {samples[0].b,samples[0].g,samples[0].r,samples[0].a} ) AppendByte(mapped,channel);
		RISEColor actual,expected;
		expected.SetFromIntegerized<Rec709RGBPel,unsigned char>(samples[0],255.0);
		const bool decoded = RunTGA(mapped,width,height,&actual);
		Check(decoded && width==1 && height==1 && sameColor(actual,expected),
			"TGA reader: truecolor skips image ID and an unused color map without changing pixels");
		mapped.resize(18+3+mapBytes-1);
		Check(!RunTGA(mapped,width,height) && width==0 && height==0,
			"TGA reader: truncated unused color map is rejected with empty dimensions");
	}
	for( size_t size : { size_t(17), size_t(20), valid.size() - 1 } ) {
		auto truncated = valid;
		truncated.resize(size);
		Check( !RunTGA(truncated, width, height) && width == 0 && height == 0,
			"TGA reader: truncated header, ID, or final pixel fails with empty dimensions" );
	}
	for( unsigned int mode = 0; mode < 5; ++mode ) {
		auto unsupported = valid;
		if( mode == 0 ) unsupported[2] = 10; // RLE.
		if( mode == 1 ) unsupported[1] = 2; // Invalid color-map type.
		if( mode == 2 ) unsupported[16] = 24;
		if( mode == 3 ) unsupported[17] = 4; // Unsupported attribute depth.
		if( mode == 4 ) unsupported[17] = 0x48; // Interleaved rows.
		Check( !RunTGA(unsupported, width, height), "TGA reader: unsupported encodings are rejected explicitly" );
	}
	if( !checkedLayout ) return; // The baseline reader cannot safely run bounds/reset probes.
	auto* buffer = new MemoryBuffer(static_cast<unsigned int>(valid.size()));
	buffer->setBytes(valid.data(), static_cast<unsigned int>(valid.size()));
	auto* reader = new TGAReader(*buffer, eColorSpace_Rec709RGB_Linear);
	Check( reader->BeginRead(width, height), "TGA reader: initial decode for failed-reload regression" );
	Check( buffer->Resize(0, true), "TGA reader: shrink input for failed-reload regression" );
	const bool failedReload = !reader->BeginRead(width, height);
	const RISEColor poison(-1000, -2000, -3000, -4000);
	RISEColor stale = poison;
	reader->ReadColor(stale, 0, 0);
	Check( failedReload && width == 0 && height == 0 && sameColor(stale, poison),
		"TGA reader: failed reload clears dimensions and previous pixels" );
	reader->EndRead();
	reader->EndRead();
	reader->release();
	buffer->release();

	auto* encoded = new MemoryBuffer();
	auto* writer = new TGAWriter(*encoded, eColorSpace_Rec709RGB_Linear);
	writer->BeginWrite(2, 2);
	for( unsigned int i = 0; i < 4; ++i ) {
		RISEColor color;
		color.SetFromIntegerized<Rec709RGBPel, unsigned char>(samples[i], 255.0);
		writer->WriteColor(color, i % 2, i / 2);
	}
	writer->EndWrite();
	writer->release();
	auto* decoded = new TGAReader(*encoded, eColorSpace_Rec709RGB_Linear);
	bool roundTrip = decoded->BeginRead(width, height) && width == 2 && height == 2;
	if( roundTrip ) {
		for( unsigned int i = 0; i < 4; ++i ) {
			RISEColor color;
			decoded->ReadColor(color, i % 2, i / 2);
			const auto actual = color.Integerize<Rec709RGBPel, unsigned char>(255.0);
			roundTrip &= std::abs(int(actual.r) - samples[i].r) <= 1 &&
				std::abs(int(actual.g) - samples[i].g) <= 1 &&
				std::abs(int(actual.b) - samples[i].b) <= 1 && actual.a == samples[i].a;
		}
	}
	Check( roundTrip, "TGA reader: writer roundtrip preserves all RGBA pixels and image orientation" );
	decoded->release();
	encoded->release();
}

//======================================================================
// HDR (Radiance RGBE)
//======================================================================
static std::vector<char> MakeHDRHeader( const char* resolutionLine )
{
	std::vector<char> v;
	AppendStr( v, "#?RADIANCE\n" );
	AppendStr( v, "FORMAT=32-bit_rle_rgbe\n" );
	AppendStr( v, resolutionLine );							// caller supplies trailing '\n'
	return v;
}

static void TestHDRNewRLE()
{
	std::cout << "B: HDR new-RLE overflow-dims / over-long run / truncated / valid" << std::endl;
	unsigned int w, h;

	// Overflow dimensions rejected before allocation.
	std::vector<char> over = MakeHDRHeader( "-Y 100000 +X 100000\n" );
	Check( !RunHDR( over, w, h ), "B: overflow-dimension HDR returns false" );

	std::vector<char> wrapped = MakeHDRHeader( "-Y 2147483648 +X 2147483648\n" );
	Check( !RunHDR( wrapped, w, h ), "B: HDR rejects dimensions whose byte count wraps 64 bits" );

	std::vector<char> missingAxis = MakeHDRHeader( "-Y 1\n" );
	Check( !RunHDR( missingAxis, w, h ), "B: HDR rejects a resolution line without an X axis" );

	std::vector<char> wrongAxis = MakeHDRHeader( "-Y 1 AX 1\n" );
	AppendByte( wrongAxis, 10 ); AppendByte( wrongAxis, 20 );
	AppendByte( wrongAxis, 30 ); AppendByte( wrongAxis, 130 );
	Check( !RunHDR( wrongAxis, w, h ), "B: HDR rejects an invalid X axis even with a complete pixel" );

	// Over-long RLE run (num=200 -> length 72) into a 8-wide scanline.
	std::vector<char> run = MakeHDRHeader( "-Y 2 +X 8\n" );
	AppendByte( run, 2 ); AppendByte( run, 2 ); AppendByte( run, 0 ); AppendByte( run, 8 );
	AppendByte( run, 200 ); AppendByte( run, 0xAB );
	Check( !RunHDR( run, w, h ), "B: over-long-RLE HDR returns false" );

	// Truncated scanline (col header, no run codes) -> zero-length run -> reject.
	std::vector<char> trunc = MakeHDRHeader( "-Y 2 +X 8\n" );
	AppendByte( trunc, 2 ); AppendByte( trunc, 2 ); AppendByte( trunc, 0 ); AppendByte( trunc, 8 );
	Check( !RunHDR( trunc, w, h ), "B: truncated-scanline HDR returns false" );

	// Valid 8x1 new-RLE: four raw runs of 8 values; R<G<B so we can assert
	// the decode order wasn't transposed.
	std::vector<char> good = MakeHDRHeader( "-Y 1 +X 8\n" );
	AppendByte( good, 2 ); AppendByte( good, 2 ); AppendByte( good, 0 ); AppendByte( good, 8 );
	const unsigned char comp[4][8] = {
		{ 10, 20, 30, 40, 50, 60, 70, 80 }, { 11, 21, 31, 41, 51, 61, 71, 81 },
		{ 12, 22, 32, 42, 52, 62, 72, 82 }, { 130, 130, 130, 130, 130, 130, 130, 130 }
	};
	for( int c = 0; c < 4; ++c ) {
		AppendByte( good, 8 );
		for( int p = 0; p < 8; ++p ) AppendByte( good, comp[c][p] );
	}
	RISEColor px0;
	const bool ok = RunHDR( good, w, h, &px0 );
	Check( ok && w == 8 && h == 1, "B: valid 8x1 new-RLE HDR decodes" );
	Check( ok && px0.a == 1.0, "B: decoded pixel alpha == 1.0" );
	Check( ok && px0.base.r > 0.0 && px0.base.r < px0.base.g && px0.base.g < px0.base.b,
		"B: pixel channels decode in R<G<B order (no transposition)" );
	auto wrongWidth = good;
	wrongWidth[MakeHDRHeader( "-Y 1 +X 8\n" ).size()+3] = 5;
	Check( !RunHDR( wrongWidth, w, h ), "B: HDR rejects an RLE scanline with the wrong encoded width" );
	good.pop_back();
	Check( !RunHDR( good, w, h ), "B: HDR rejects a missing final RLE literal" );
}

static void TestHDROldRLE()
{
	std::cout << "C: HDR old-RLE valid raw / valid run / crafted run-at-pixel-0" << std::endl;
	unsigned int w, h;

	std::vector<char> headerOnly = MakeHDRHeader( "-Y 1 +X 1\n" );
	Check( !RunHDR( headerOnly, w, h ), "C: HDR rejects a header without the first pixel" );

	std::vector<char> longComment;
	AppendStr( longComment, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n#" );
	longComment.insert( longComment.end(), 1023, ' ' );
	AppendStr( longComment, "-Y 1 +X 2\n-Y 1 +X 1\n" );
	AppendByte( longComment, 2 ); AppendByte( longComment, 2 );
	AppendByte( longComment, 1 ); AppendByte( longComment, 130 );
	Check( RunHDR( longComment, w, h ) && w == 1 && h == 1,
		"C: HDR skips long comment continuations and accepts a flat pixel with an RLE-like prefix" );

	std::vector<char> highBitBlue = MakeHDRHeader( "-Y 1 +X 8\n" );
	for( int i=0; i<8; ++i ) {
		AppendByte( highBitBlue, 2 ); AppendByte( highBitBlue, 2 );
		AppendByte( highBitBlue, 128 ); AppendByte( highBitBlue, 130 );
	}
	Check( RunHDR( highBitBlue, w, h ) && w == 8 && h == 1,
		"C: HDR accepts a flat scanline with a high-bit blue sample" );

	std::vector<char> xyze;
	AppendStr( xyze, "#?RADIANCE\nFORMAT=32-bit_rle_xyze\n-Y 1 +X 1\n" );
	AppendByte( xyze, 10 ); AppendByte( xyze, 20 ); AppendByte( xyze, 30 ); AppendByte( xyze, 130 );
	Check( !RunHDR( xyze, w, h ), "C: HDR rejects unsupported XYZE encoding" );

	// Valid old-RLE raw scanline (first byte != 2 selects old scheme; != 1,1,1
	// selects the raw branch).  Exercises the `col`-not-`buf` store fix.
	std::vector<char> raw = MakeHDRHeader( "-Y 1 +X 2\n" );
	AppendByte( raw, 10 ); AppendByte( raw, 20 ); AppendByte( raw, 30 ); AppendByte( raw, 130 );	// px0
	AppendByte( raw, 11 ); AppendByte( raw, 21 ); AppendByte( raw, 31 ); AppendByte( raw, 131 );	// px1
	RISEColor px0, px1;
	bool ok = RunHDR( raw, w, h, &px0, &px1 );
	Check( ok && w == 2 && h == 1, "C: valid old-RLE raw HDR decodes" );
	Check( ok && px0.base.r > 0.0 && px0.base.r < px0.base.g && px0.base.g < px0.base.b,
		"C: old-RLE raw pixel channels decode in order (col, not header buf)" );

	// Valid old-RLE with a run: px0 raw, then a (1,1,1,2) header repeats px0
	// twice.  Exercises the bounds-checked run writes + the back-reference read.
	std::vector<char> withrun = MakeHDRHeader( "-Y 1 +X 3\n" );
	AppendByte( withrun, 10 ); AppendByte( withrun, 20 ); AppendByte( withrun, 30 ); AppendByte( withrun, 130 ); // px0
	AppendByte( withrun, 1 ); AppendByte( withrun, 1 ); AppendByte( withrun, 1 ); AppendByte( withrun, 2 );      // run x2
	RISEColor last;
	ok = RunHDR( withrun, w, h, 0, &last );
	Check( ok && w == 3 && h == 1, "C: valid old-RLE run HDR decodes" );
	Check( ok && last.base.r > 0.0 && last.base.r < last.base.g && last.base.g < last.base.b,
		"C: old-RLE run copies the previous pixel (back-reference)" );

	// Reference Radiance old-RLE counts are little-endian byte chains.
	const auto chained = [&](unsigned int width, const std::vector<unsigned char>& packets,
		const RISEColor& expectedLast, const char* label) {
		auto bytes = MakeHDRHeader(("-Y 1 +X " + std::to_string(width) + "\n").c_str());
		for(auto byte : packets) AppendByte(bytes,byte);
		RISEColor first, final;
		const bool decoded=RunHDR(bytes,w,h,&first,&final);
		Check(decoded && w==width && h==1 && first.base.r==px0.base.r &&
			final.base.r==expectedLast.base.r && final.base.g==expectedLast.base.g &&
			final.base.b==expectedLast.base.b,label);
	};
	chained(257,{10,20,30,130, 1,1,1,0, 1,1,1,1},px0,
		"C: old-RLE chained count accepts zero low byte and emits 256 repeats");
	chained(258,{10,20,30,130, 1,1,1,1, 1,1,1,1},px0,
		"C: old-RLE chained count emits 257 repeats");
	chained(259,{10,20,30,130, 1,1,1,1, 11,21,31,131, 1,1,1,0, 1,1,1,1},px1,
		"C: old-RLE chained count shift resets after a literal");
	chained(2,{10,20,30,130, 1,1,1,0, 11,21,31,131},px1,
		"C: old-RLE zero count can precede a literal");
	for(const auto& packets : std::vector<std::vector<unsigned char>>{
		{10,20,30,130, 1,1,1,0},
		{10,20,30,130, 1,1,1,0, 1,1,1,0, 1,1,1,1},
		{10,20,30,130, 1,1,1,0, 1,1,1,0, 1,1,1,0, 1,1,1,0, 1,1,1,0}}) {
		auto bytes=MakeHDRHeader("-Y 1 +X 258\n");
		for(auto byte : packets) AppendByte(bytes,byte);
		Check(!RunHDR(bytes,w,h) && w==0 && h==0,
			"C: old-RLE chained count rejects truncated oversized and over-shifted packets");
	}

	// Crafted old-RLE run at pixel 0: (1,1,1,5) at scanline start references
	// pos-step == -1 -> the back-reference bounds guard must reject.
	std::vector<char> badrun = MakeHDRHeader( "-Y 1 +X 4\n" );
	AppendByte( badrun, 1 ); AppendByte( badrun, 1 ); AppendByte( badrun, 1 ); AppendByte( badrun, 5 );
	Check( !RunHDR( badrun, w, h ), "C: old-RLE run at pixel 0 (back-ref underflow) returns false" );

	std::vector<char> secondRowRun = MakeHDRHeader( "-Y 2 +X 1\n" );
	AppendByte( secondRowRun, 10 ); AppendByte( secondRowRun, 20 );
	AppendByte( secondRowRun, 30 ); AppendByte( secondRowRun, 130 );
	AppendByte( secondRowRun, 1 ); AppendByte( secondRowRun, 1 );
	AppendByte( secondRowRun, 1 ); AppendByte( secondRowRun, 1 );
	Check( !RunHDR( secondRowRun, w, h ), "C: old-RLE run cannot reference the previous scanline" );
}

//======================================================================
// PNG — crafted IHDR with overflow dimensions (valid chunk CRCs).
//======================================================================
#ifndef NO_PNG_SUPPORT
static void AppendU32BE( std::vector<char>& v, unsigned int x )
{
	AppendByte( v, (x >> 24) & 0xFF ); AppendByte( v, (x >> 16) & 0xFF );
	AppendByte( v, (x >> 8) & 0xFF );  AppendByte( v, x & 0xFF );
}

static void AppendPNGChunk( std::vector<char>& v, const char type[4], const std::vector<unsigned char>& data )
{
	AppendU32BE( v, (unsigned int)data.size() );
	std::vector<unsigned char> tc;
	tc.insert( tc.end(), type, type + 4 );
	tc.insert( tc.end(), data.begin(), data.end() );
	for( int i = 0; i < 4; ++i ) v.push_back( type[i] );
	for( size_t i = 0; i < data.size(); ++i ) v.push_back( (char)data[i] );
	unsigned long crc = crc32( 0L, Z_NULL, 0 );
	crc = crc32( crc, (const Bytef*)tc.data(), (uInt)tc.size() );
	AppendU32BE( v, (unsigned int)crc );
}

static void TestPNG()
{
	std::cout << "D: PNG with 100000x100000 IHDR is rejected before allocation" << std::endl;

	std::vector<char> png;
	const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	for( int i = 0; i < 8; ++i ) AppendByte( png, sig[i] );

	// IHDR: 100000x100000, 8-bit, colour type 2 (RGB).  Both dims are < the
	// libpng default limit (1e6) so png_read_info accepts them; the 32-bit
	// height*stride (1e5 * (1e5*3)) = 3e10 wraps.  The fix's 64-bit ceiling
	// check rejects it.
	std::vector<unsigned char> ihdr;
	unsigned int dim = 100000;
	for( int s = 24; s >= 0; s -= 8 ) ihdr.push_back( (dim >> s) & 0xFF ); // width  BE
	for( int s = 24; s >= 0; s -= 8 ) ihdr.push_back( (dim >> s) & 0xFF ); // height BE
	ihdr.push_back( 8 );	// bit depth
	ihdr.push_back( 2 );	// colour type RGB
	ihdr.push_back( 0 );	// compression
	ihdr.push_back( 0 );	// filter
	ihdr.push_back( 0 );	// interlace
	AppendPNGChunk( png, "IHDR", ihdr );

	// A structurally-valid IDAT (content never decompressed — we reject before
	// png_read_row) so png_read_info has a chunk to stop at, then IEND.
	std::vector<unsigned char> idat; idat.push_back( 0x78 );
	AppendPNGChunk( png, "IDAT", idat );
	AppendPNGChunk( png, "IEND", std::vector<unsigned char>() );

	unsigned int w, h;
	Check( !RunPNG( png, w, h ), "D: overflow-dimension PNG returns false (no OOB alloc)" );

	std::vector<char> rgba16;
	for( unsigned char byte : sig ) AppendByte( rgba16, byte );
	AppendPNGChunk( rgba16, "IHDR", { 0,0,0,1, 0,0,0,1, 16,6,0,0,0 } );
	const unsigned char row[] = { 0, 0x12,0x34, 0x56,0x78, 0x9a,0xbc, 0xde,0xf0 };
	std::vector<unsigned char> compressed( compressBound( sizeof(row) ) );
	uLongf length = static_cast<uLongf>( compressed.size() );
	const int compressedOk = compress( compressed.data(), &length, row, sizeof(row) );
	Check( compressedOk == Z_OK, "D: create an independent 16-bit PNG scanline fixture" );
	if( compressedOk == Z_OK ) {
		compressed.resize( length );
		AppendPNGChunk( rgba16, "IDAT", compressed );
		AppendPNGChunk( rgba16, "IEND", {} );
		RISEColor pixel;
		const bool ok = RunPNG( rgba16, w, h, &pixel );
		const RISEColor expected( RISEPel( Rec709RGBPel( 0x1234 / 65535.0,
			0x5678 / 65535.0, 0x9abc / 65535.0 ) ), 0xdef0 / 65535.0 );
		Check( ok && w == 1 && h == 1 && std::fabs( pixel.a - expected.a ) < 1e-12 &&
			std::fabs( pixel.base.r - expected.base.r ) < 1e-12 &&
			std::fabs( pixel.base.g - expected.base.g ) < 1e-12 &&
			std::fabs( pixel.base.b - expected.base.b ) < 1e-12,
			"D: 16-bit PNG decodes independently specified network-order RGBA samples" );
		rgba16.pop_back();
		Check( !RunPNG( rgba16, w, h ), "D: short PNG read returns failure" );
	}

	for( unsigned char colorType : { 0, 4 } ) {
		std::vector<char> grayscale;
		for( unsigned char byte : sig ) AppendByte( grayscale, byte );
		AppendPNGChunk( grayscale, "IHDR", { 0,0,0,1, 0,0,0,1, 16,colorType,0,0,0 } );
		const unsigned char grayRow[] = { 0, 0x12,0x34, 0x56,0x78 };
		compressed.resize( compressBound( sizeof(grayRow) ) );
		length = static_cast<uLongf>( compressed.size() );
		if( compress( compressed.data(), &length, grayRow, colorType == 4 ? 5 : 3 ) != Z_OK ) {
			Check( false, "D: compress grayscale fixture" );
			continue;
		}
		compressed.resize( length );
		AppendPNGChunk( grayscale, "IDAT", compressed );
		AppendPNGChunk( grayscale, "IEND", {} );
		RISEColor pixel;
		const bool ok = RunPNG( grayscale, w, h, &pixel );
		const double gray = 0x1234 / 65535.0;
		const RISEPel expected( Rec709RGBPel( gray, gray, gray ) );
		const double alpha = colorType == 4 ? 0x5678 / 65535.0 : 1.0;
		Check( ok && w == 1 && h == 1 && std::fabs( pixel.a-alpha ) < 1e-12 &&
			std::fabs( pixel.base.r-expected.r ) < 1e-12 &&
			std::fabs( pixel.base.g-expected.g ) < 1e-12 &&
			std::fabs( pixel.base.b-expected.b ) < 1e-12,
			colorType == 4 ? "D: expanded grayscale preserves alpha" : "D: expanded grayscale has opaque alpha" );
	}
}
#endif

//======================================================================
// TIFF — real 2x2 via libtiff (happy path) + a dimension-patched copy
// (safely rejected).  TIFF is not exercised by any scene test, so this is
// the only coverage the reader has.
//======================================================================
#ifndef NO_TIFF_SUPPORT
// Patch the little-endian IFD: set the value field of ImageWidth (256),
// ImageLength (257) and RowsPerStrip (278) to 65535 so the resulting
// 65535*65535*4 == 1.7e10 exceeds the 2 GiB ceiling.  RowsPerStrip is patched
// too so the strip count stays 1 (matching the single strip actually present),
// keeping TIFFClientOpen happy so our size guard — not a strip-count error — is
// what rejects the file.
static void PatchTIFFDims( std::vector<char>& t )
{
	if( t.size() < 8 || t[0] != 'I' || t[1] != 'I' ) return;	// little-endian only
	unsigned char* b = (unsigned char*)t.data();
	auto rd32 = [&]( size_t o ) { return (unsigned int)b[o] | (b[o+1]<<8) | (b[o+2]<<16) | ((unsigned int)b[o+3]<<24); };
	auto rd16 = [&]( size_t o ) { return (unsigned int)b[o] | (b[o+1]<<8); };
	size_t ifd = rd32( 4 );
	if( ifd + 2 > t.size() ) return;
	unsigned int n = rd16( ifd );
	for( unsigned int i = 0; i < n; ++i ) {
		size_t e = ifd + 2 + (size_t)i * 12;
		if( e + 12 > t.size() ) break;
		unsigned int tag = rd16( e );
		if( tag == 256 || tag == 257 || tag == 278 ) {
			// Value field at e+8: write 65535 (low 2 bytes; valid for SHORT and LONG).
			b[e+8] = 0xFF; b[e+9] = 0xFF; b[e+10] = 0x00; b[e+11] = 0x00;
		}
	}
}

static void TestDamagedTIFFStrip()
{
	const std::string path = TmpDir() + "rise_bad_strip_" +
		std::to_string(RISE::TestSupport::CurrentProcessId()) + ".tif";
	TIFF* output = TIFFOpen( path.c_str(), "w" );
	if( !output ) { Check(false, "create damaged TIFF strip fixture"); return; }
	TIFFSetField( output, TIFFTAG_IMAGEWIDTH, 4 );
	TIFFSetField( output, TIFFTAG_IMAGELENGTH, 4 );
	TIFFSetField( output, TIFFTAG_SAMPLESPERPIXEL, 3 );
	TIFFSetField( output, TIFFTAG_BITSPERSAMPLE, 8 );
	TIFFSetField( output, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT );
	TIFFSetField( output, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG );
	TIFFSetField( output, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB );
	TIFFSetField( output, TIFFTAG_ROWSPERSTRIP, 1 );
	TIFFSetField( output, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE );
	bool written = true;
	for( unsigned int y = 0; y < 4; ++y ) {
		unsigned char row[12];
		for( unsigned int x = 0; x < 4; ++x ) {
			row[x*3] = static_cast<unsigned char>(20 + y*40);
			row[x*3+1] = 40;
			row[x*3+2] = 80;
		}
		written &= TIFFWriteScanline( output, row, y, 0 ) >= 0;
	}
	TIFFClose( output );
	TIFF* input = TIFFOpen( path.c_str(), "r" );
	std::uint64_t* offsets = nullptr;
	std::uint64_t* counts = nullptr;
	const bool indexed = input && TIFFGetField(input, TIFFTAG_STRIPOFFSETS, &offsets) &&
		TIFFGetField(input, TIFFTAG_STRIPBYTECOUNTS, &counts) && TIFFNumberOfStrips(input) == 4;
	if( !written || !indexed ) {
		Check(false, "write and locate TIFF strips");
		if( input ) TIFFClose(input);
		std::remove(path.c_str());
		return;
	}
	const auto offset = offsets[1];
	const auto count = counts[1];
	TIFFClose( input );
	std::fstream corrupt( path, std::ios::binary | std::ios::in | std::ios::out );
	corrupt.seekp( static_cast<std::streamoff>(offset) );
	for( std::uint64_t i = 0; i < count; ++i ) corrupt.put( static_cast<char>(0xff) );
	corrupt.close();
	input = TIFFOpen( path.c_str(), "r" );
	std::uint32_t pixels[16] = {};
	const bool partial = input && TIFFReadRGBAImageOriented(input, 4, 4, pixels, ORIENTATION_TOPLEFT, 0);
	if( input ) TIFFClose(input);
	Check( partial && TIFFGetR(pixels[0]) == 20 && TIFFGetR(pixels[4]) == 0 &&
		TIFFGetA(pixels[4]) == 255 && TIFFGetR(pixels[8]) == 100,
		"permissive TIFF decoding reports success with an opaque-black damaged strip" );
	unsigned int width = 0, height = 0;
	auto damaged = ReadFileBytes(path);
	Check( !RunTIFF(damaged, width, height), "RISE rejects a TIFF with a damaged compressed strip" );
	std::remove( path.c_str() );
}

static void TestTIFF()
{
	std::cout << "E: TIFF valid 2x2 decodes; 65535^2-patched TIFF safely rejected" << std::endl;

	const std::string path = TmpDir() + "rise_imgoverflow_test.tif";
	std::remove( path.c_str() );

	TIFF* tif = TIFFOpen( path.c_str(), "w" );
	if( !tif ) {
		Check( false, "E: could not create temp TIFF (libtiff)" );
		return;
	}
	TIFFSetField( tif, TIFFTAG_IMAGEWIDTH, 2 );
	TIFFSetField( tif, TIFFTAG_IMAGELENGTH, 2 );
	TIFFSetField( tif, TIFFTAG_SAMPLESPERPIXEL, 4 );
	TIFFSetField( tif, TIFFTAG_BITSPERSAMPLE, 8 );
	TIFFSetField( tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT );
	TIFFSetField( tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG );
	TIFFSetField( tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB );
	TIFFSetField( tif, TIFFTAG_ROWSPERSTRIP, 2 );			// single strip
	unsigned char row[2*4];
	for( int y = 0; y < 2; ++y ) {
		for( int x = 0; x < 2; ++x ) {
			row[x*4+0] = 200; row[x*4+1] = 100; row[x*4+2] = 50; row[x*4+3] = 255;
		}
		TIFFWriteScanline( tif, row, y, 0 );
	}
	TIFFClose( tif );

	std::vector<char> bytes = ReadFileBytes( path );
	std::remove( path.c_str() );
	Check( !bytes.empty(), "E: temp TIFF written and read back" );

	unsigned int w, h;
	RISEColor tpx0;
	const bool tOk = RunTIFF( bytes, w, h, &tpx0 );
	Check( tOk && w == 2 && h == 2, "E: valid 2x2 TIFF decodes" );
	// Assert the decoded pixel is non-empty (buffer actually populated by
	// TIFFReadRGBAImage, not left memset-zero).  We don't assert an exact
	// channel order here to stay robust to libtiff's RGBA byte packing.
	Check( tOk && (tpx0.base.r + tpx0.base.g + tpx0.base.b) > 0.0,
		"E: valid TIFF pixel (0,0) is non-empty (decode populated the buffer)" );

	auto* output = new MemoryBuffer();
	IRasterImageWriter* writer = nullptr;
	if( RISE_API_CreateTIFFWriter( &writer, *output, eColorSpace_Rec709RGB_Linear ) && writer ) {
		writer->BeginWrite( 1, 1 );
		RISEColor red;
		red.SetFromIntegerized<Rec709RGBPel, unsigned char>( {200, 100, 50, 128}, 255.0 );
		writer->WriteColor( red, 0, 0 );
		writer->EndWrite();
		writer->release();
		Check( output->DumpToFile( path.c_str() ), "E: RISE writes a TIFF via a memory buffer" );
	} else {
		Check( false, "E: RISE creates a TIFF writer" );
	}
	output->release();
	TIFF* decoded = TIFFOpen( path.c_str(), "r" );
	std::uint32_t pixel = 0;
	const bool read = decoded && TIFFReadRGBAImage( decoded, 1, 1, &pixel, 1 );
	Check( read && TIFFGetR(pixel) == 100 && TIFFGetG(pixel) == 50 &&
		TIFFGetB(pixel) == 25 && TIFFGetA(pixel) == 128,
		"E: libtiff independently decodes the RISE-written pixel" );
	if( decoded ) TIFFClose( decoded );
	RISEColor roundTrip;
	auto encoded = ReadFileBytes( path );
	const bool readBack = RunTIFF( encoded, w, h, &roundTrip );
	const auto rgba = roundTrip.Integerize<Rec709RGBPel, unsigned char>( 255.0 );
	Check( readBack && std::abs( int(rgba.r) - 200 ) <= 2 && std::abs( int(rgba.g) - 100 ) <= 2 &&
		std::abs( int(rgba.b) - 50 ) <= 2 && rgba.a == 128,
		"E: RISE reads TIFF alpha as straight color" );
	std::remove( path.c_str() );

	std::vector<char> patched = bytes;
	PatchTIFFDims( patched );
	Check( !RunTIFF( patched, w, h ), "E: 65535^2-dimension TIFF safely rejected (no OOB)" );
}
#endif

static void TestReaderFamilyReload()
{
	using Factory = std::function<IRasterImageReader*(IReadBuffer&)>;
	struct Case { const char* name; std::vector<char> bytes; Factory create; };
	const auto encoded = [](const char* format) {
		auto* output = new MemoryBuffer();
		IRasterImageWriter* writer = nullptr;
		if( std::strcmp(format, "tga") == 0 ) RISE_API_CreateTGAWriter(&writer, *output, eColorSpace_Rec709RGB_Linear);
		if( std::strcmp(format, "png") == 0 ) RISE_API_CreatePNGWriter(&writer, *output, 8, eColorSpace_Rec709RGB_Linear);
		if( std::strcmp(format, "tiff") == 0 ) RISE_API_CreateTIFFWriter(&writer, *output, eColorSpace_Rec709RGB_Linear);
		if( std::strcmp(format, "exr") == 0 ) RISE_API_CreateEXRWriter(&writer, *output, eColorSpace_Rec709RGB_Linear, eExrCompression_Piz, true, true);
		std::vector<char> result;
		if( writer ) {
			writer->BeginWrite(2, 2);
			for( unsigned int y = 0; y < 2; ++y ) for( unsigned int x = 0; x < 2; ++x )
				writer->WriteColor(RISEColor(0.25 + 0.25*x, 0.5, 0.75 - 0.25*y, 1), x, y);
			writer->EndWrite();
			result.assign(reinterpret_cast<const char*>(output->Pointer()),
				reinterpret_cast<const char*>(output->Pointer()) + output->Size());
			writer->release();
		}
		output->release();
		return result;
	};
	std::vector<Case> cases;
	cases.push_back({"TGA", encoded("tga"), [](IReadBuffer& b) { IRasterImageReader* r=nullptr; RISE_API_CreateTGAReader(&r,b,eColorSpace_Rec709RGB_Linear); return r; }});
	auto hdr = MakeHDRHeader("-Y 2 +X 2\n");
	for( int i=0; i<4; ++i ) for( unsigned char v : {128,64,32,129} ) AppendByte(hdr,v);
	cases.push_back({"HDR", hdr, [](IReadBuffer& b) { IRasterImageReader* r=nullptr; RISE_API_CreateHDRReader(&r,b); return r; }});
	cases.push_back({"JPEG", ReadFileBytes(JPEGFixture().string()), [](IReadBuffer& b) { IRasterImageReader* r=nullptr; RISE_API_CreateJPEGReader(&r,b,eColorSpace_Rec709RGB_Linear); return r; }});
#ifndef NO_PNG_SUPPORT
	cases.push_back({"PNG", encoded("png"), [](IReadBuffer& b) { IRasterImageReader* r=nullptr; RISE_API_CreatePNGReader(&r,b,eColorSpace_Rec709RGB_Linear); return r; }});
#endif
#ifndef NO_TIFF_SUPPORT
	cases.push_back({"TIFF", encoded("tiff"), [](IReadBuffer& b) { IRasterImageReader* r=nullptr; RISE_API_CreateTIFFReader(&r,b,eColorSpace_Rec709RGB_Linear); return r; }});
#endif
#ifndef NO_EXR_SUPPORT
	cases.push_back({"EXR", encoded("exr"), [](IReadBuffer& b) { IRasterImageReader* r=nullptr; RISE_API_CreateEXRReader(&r,b,eColorSpace_Rec709RGB_Linear); return r; }});
#endif
	const auto same = [](const RISEColor& a,const RISEColor& b) {
		return a.base.r==b.base.r && a.base.g==b.base.g && a.base.b==b.base.b && a.a==b.a;
	};
	const RISEColor poison(-1000,-2000,-3000,-4000);
	bool allReset = true;
	for( auto& item : cases ) {
		const auto check = [&](bool ok,const char* suffix) { Check(ok,(std::string("reader family ")+item.name+": "+suffix).c_str()); };
		check(!item.bytes.empty(), "valid fixture exists");
		if( item.bytes.empty() ) { allReset=false; continue; }
		// These blob formats start at the construction cursor. A prefix also
		// catches accidental rewinds to byte zero and full-buffer JPEG slurps.
		auto source = item.bytes;
		const unsigned int start = std::strcmp(item.name,"PNG") == 0 || std::strcmp(item.name,"JPEG") == 0 ||
			std::strcmp(item.name,"EXR") == 0 ? 7 : 0;
		source.insert(source.begin(), start, '!');
		auto* input = new MemoryBuffer(static_cast<unsigned int>(source.size()));
		input->setBytes(source.data(),static_cast<unsigned int>(source.size())); input->seek(IBuffer::START,start);
		auto* reader = item.create(*input);
		unsigned int width=0,height=0;
		bool opened = reader && reader->BeginRead(width,height);
		check(opened && width>=2 && height>=2, "initial decode has complete dimensions");
		if( !opened ) { allReset=false; if(reader)reader->release();input->release();continue; }
		const auto expectedWidth=width,expectedHeight=height;
		std::vector<RISEColor> expected(width*height);
		for(unsigned int y=0;y<height;++y) for(unsigned int x=0;x<width;++x) reader->ReadColor(expected[y*width+x],x,y);
		const auto pixelsMatch = [&]() {
			bool match=width==expectedWidth && height==expectedHeight;
			if(match) for(unsigned int y=0;y<height;++y) for(unsigned int x=0;x<width;++x) {
				RISEColor pixel=poison;reader->ReadColor(pixel,x,y);match &= same(pixel,expected[y*width+x]);
			}
			return match;
		};
		for( int i=0; i<3; ++i ) {
			const bool reloaded=reader->BeginRead(width,height);
			check(reloaded && pixelsMatch(), "repeated BeginRead preserves every pixel without caller rewind");
		}
		check(input->Resize(0,true), "empty replacement input installed");
		width=123;height=456;bool threw=false,failed=false;
		try { failed=!reader->BeginRead(width,height); } catch(const std::exception&) { threw=true; }
		RISEColor stale=poison;reader->ReadColor(stale,0,0);
		const bool reset=!threw && failed && width==0 && height==0 && same(stale,poison);
		check(reset, "failed reload clears dimensions and previous pixels without throwing");
		allReset &= reset;
		input->Resize(static_cast<unsigned int>(source.size()),true);input->seek(IBuffer::START,0);
		input->setBytes(source.data(),static_cast<unsigned int>(source.size()));
		const bool recovered=reader->BeginRead(width,height);
		check(recovered && pixelsMatch(), "healthy input recovers every pixel after failed reload without caller rewind");
		reader->EndRead();reader->EndRead();stale=poison;reader->ReadColor(stale,0,0);
		check(same(stale,poison), "repeated EndRead leaves no active image");
		reader->release();check(input->refcount()==1, "reader teardown releases its input");input->release();
	}
	// The unfixed readers fail the reset contract above; avoid invoking their
	// old unchecked pixel methods out of range during a normal-assertion RED run.
	if( !allReset ) return;
	for( auto& item : cases ) {
		auto* input=new MemoryBuffer(item.bytes.data(),static_cast<unsigned int>(item.bytes.size()),false);
		auto* reader=item.create(*input);unsigned int width=0,height=0;
		const bool opened=reader->BeginRead(width,height);
		RISEColor pixel=poison;
		if(opened){reader->ReadColor(pixel,width,0);reader->ReadColor(pixel,0,height);}
		Check(opened && same(pixel,poison),(std::string("reader family ")+item.name+": invalid coordinates preserve caller color").c_str());
		reader->release();input->release();
	}
}

#ifndef NO_EXR_SUPPORT
static void WriteSDKAssociatedEXR( MemoryBuffer& buffer, const std::vector<float>& rgba,
	Imf::PixelType storedType = Imf::FLOAT, bool alpha = true )
{
	const auto width=static_cast<unsigned int>(rgba.size()/4);
	OStreamWrapper stream(buffer);Imf::Header header(width,1);
	const char* names[]={"R","G","B","A"};
	Imf::FrameBuffer frame;
	std::vector<Imath::half> halfPixels;
	if(storedType==Imf::HALF) {
		halfPixels.reserve(rgba.size());
		for(float value : rgba)halfPixels.emplace_back(value);
	}
	for(unsigned int channel=0;channel<(alpha?4u:3u);++channel) {
		header.channels().insert(names[channel],Imf::Channel(storedType));
		if(storedType==Imf::HALF) {
			frame.insert(names[channel],Imf::Slice::Make(Imf::HALF,halfPixels.data()+channel,
				header.dataWindow(),4*sizeof(Imath::half),width*4*sizeof(Imath::half)));
		} else {
			frame.insert(names[channel],Imf::Slice::Make(Imf::FLOAT,rgba.data()+channel,
				header.dataWindow(),4*sizeof(float),width*4*sizeof(float)));
		}
	}
	Imf::OutputFile file(stream,header);file.setFrameBuffer(frame);file.writePixels(1);
}

static bool SameStraightEXRColor( const RISEColor& actual, const RISEPel& expected, double alpha )
{
	const auto close=[](double a,double b){return std::fabs(a-b)<=1e-12*std::max(1.0,std::fabs(b));};
	return close(actual.base.r,expected.r) && close(actual.base.g,expected.g) &&
		close(actual.base.b,expected.b) && actual.a==alpha;
}

static void TestEXRStraightAlpha()
{
	for(bool floats : {false,true}) {
		const float tiny=std::ldexp(1.0f,floats?-100:-14);
		const float large=floats?std::ldexp(1.0f,100):1.0f;
		const std::vector<float> associated={
			7,8,9,0, 0.25f,0.125f,0.5f,0.5f, 1,0.5f,2,2,
			-0.125f,0.0625f,-0.25f,-0.25f, large,1,tiny,tiny, 0.125f,0.25f,0.5f,1
		};
		// Power-of-two cases have exact quotients, including a FLOAT input
		// whose straight result is 2^200 and therefore requires double division.
		const double expected[][4]={
			{7,8,9,0},{0.5,0.25,1,0.5},{0.5,0.25,1,2},{0.5,-0.25,1,-0.25},
			{std::ldexp(1.0,floats?200:14),std::ldexp(1.0,floats?100:14),1,tiny},{0.125,0.25,0.5,1}
		};
		auto* input=new MemoryBuffer();WriteSDKAssociatedEXR(*input,associated,floats?Imf::FLOAT:Imf::HALF);
		for(auto space : {eColorSpace_Rec709RGB_Linear,eColorSpace_sRGB}) {
			input->seek(IBuffer::START,0);auto* reader=new EXRReader(*input,space);
			unsigned int width=0,height=0;
			bool exact=reader->BeginRead(width,height) && width==6 && height==1;
			RISEColor decoded[6];
			if(exact) for(unsigned int i=0;i<6;++i) {
				RISEColor actual;reader->ReadColor(actual,i,0);
				decoded[i]=actual;
				const auto& p=expected[i];
				const RISEPel straight=space==eColorSpace_sRGB?RISEPel(sRGBPel(p[0],p[1],p[2])):
					RISEPel(Rec709RGBPel(p[0],p[1],p[2]));
				exact &= SameStraightEXRColor(actual,straight,p[3]);
			}
			Check(exact,space==eColorSpace_sRGB?
				"EXR straight alpha: SDK FLOAT/HALF associated RGB unassociates before nonlinear color conversion":
				"EXR straight alpha: SDK FLOAT/HALF retains zero-alpha RGB and handles negative above-one tiny alpha without losing range");
			reader->release();
			if(exact && space==eColorSpace_Rec709RGB_Linear) {
				auto* output=new MemoryBuffer();
				auto* writer=new EXRWriter(*output,space,eExrCompression_None,true,floats);
				writer->BeginWrite(6,1);for(unsigned int i=0;i<6;++i)writer->WriteColor(decoded[i],i,0);
				writer->EndWrite();writer->release();output->seek(IBuffer::START,0);
				std::vector<float> stored(24);
				{
					IStreamWrapper stream(*output);Imf::InputFile file(stream);Imf::FrameBuffer frame;
					const char* names[]={"R","G","B","A"};
					for(unsigned int c=0;c<4;++c)frame.insert(names[c],Imf::Slice::Make(Imf::FLOAT,stored.data()+c,
						file.header().dataWindow(),4*sizeof(float),24*sizeof(float)));
					file.setFrameBuffer(frame);file.readPixels(0,0);exact=!stream.Failed();
				}
				for(unsigned int i=0;i<24;++i)exact &= stored[i]==(i<3?0.0f:associated[i]);
				Check(exact,"EXR straight alpha: repeated EXR read/write preserves associated pixels and existing zero-alpha premultiplication");
				output->release();
			}
		}
		input->release();
	}
	auto* opaque=new MemoryBuffer();WriteSDKAssociatedEXR(*opaque,{0.125f,0.25f,0.5f,0},Imf::FLOAT,false);
	opaque->seek(IBuffer::START,0);auto* reader=new EXRReader(*opaque,eColorSpace_Rec709RGB_Linear);
	unsigned int width=0,height=0;RISEColor actual;
	const bool opened=reader->BeginRead(width,height);if(opened)reader->ReadColor(actual,0,0);
	Check(opened && width==1 && height==1 && SameStraightEXRColor(actual,RISEPel(Rec709RGBPel(0.125,0.25,0.5)),1),
		"EXR straight alpha: missing alpha remains opaque with unchanged RGB");
	reader->release();opaque->release();
}

static void TestEXRAlphaAcrossFormats()
{
	auto* source=new MemoryBuffer();
	const float nan=static_cast<float>(std::strtod("nan",nullptr));
	const float pinf=static_cast<float>(std::strtod("1e999",nullptr));
	const float ninf=static_cast<float>(std::strtod("-1e999",nullptr));
	WriteSDKAssociatedEXR(*source,{0.25f,0.125f,0.5f,0.5f, 0.0625f,0.125f,0.1875f,0.25f, 0.125f,0.5f,1,0,
		-0.0625f,-0.125f,-0.1875f,-0.25f, 0.5f,1,1.5f,2,
		0.25f,0.5f,0.75f,nan, 0.25f,0.5f,0.75f,pinf, 0.25f,0.5f,0.75f,ninf});
	constexpr unsigned int pixelCount=8;
	source->seek(IBuffer::START,0);auto* sourceReader=new EXRReader(*source,eColorSpace_Rec709RGB_Linear);
	unsigned int width=0,height=0;const bool opened=sourceReader->BeginRead(width,height);
	Check(opened && width==pixelCount && height==1,"EXR straight alpha: independent associated fixture opens for format conversion");
	if(!opened || width!=pixelCount || height!=1) {sourceReader->release();source->release();return;}
	RISEColor straight[pixelCount];for(unsigned int i=0;i<pixelCount;++i)sourceReader->ReadColor(straight[i],i,0);
	Check(straight[3].a==-0.25 && straight[4].a==2 &&
		!IsFiniteDouble(straight[5].a) && !IsPositiveInfinityDouble(straight[5].a) && !IsPositiveInfinityDouble(-straight[5].a) &&
		IsPositiveInfinityDouble(straight[6].a) && IsPositiveInfinityDouble(-straight[7].a),
		"normalized Integerize: EXR input retains negative above-one and nonfinite alpha before LDR conversion");
	sourceReader->release();source->release();
	// The established 8-bit writers truncate quantization: .5->127, .25->63.
	const RGBA8 expected[]={{127,63,255,127},{63,127,191,63},{31,127,255,0},
		{63,127,191,0},{63,127,191,255},{0,0,0,0},{0,0,0,255},{0,0,0,0}};
	std::vector<const char*> formats={"tga"};
#ifndef NO_PNG_SUPPORT
	formats.push_back("png");
#endif
	for(const char* format : formats) {
		const bool png=std::strcmp(format,"png")==0;
		auto* encoded=new MemoryBuffer();IRasterImageWriter* writer=nullptr;
		if(png) RISE_API_CreatePNGWriter(&writer,*encoded,8,eColorSpace_Rec709RGB_Linear);
		else RISE_API_CreateTGAWriter(&writer,*encoded,eColorSpace_Rec709RGB_Linear);
		Check(writer!=nullptr,"EXR straight alpha: target straight-RGBA writer exists");
		if(!writer){encoded->release();continue;}
		writer->BeginWrite(pixelCount,1);for(unsigned int i=0;i<pixelCount;++i)writer->WriteColor(straight[i],i,0);
		writer->EndWrite();writer->release();encoded->seek(IBuffer::START,0);
		IRasterImageReader* decoder=nullptr;
		if(png) RISE_API_CreatePNGReader(&decoder,*encoded,eColorSpace_Rec709RGB_Linear);
		else RISE_API_CreateTGAReader(&decoder,*encoded,eColorSpace_Rec709RGB_Linear);
		bool exact=decoder && decoder->BeginRead(width,height) && width==pixelCount && height==1;
		RISEColor recovered[pixelCount];
		if(exact) for(unsigned int i=0;i<pixelCount;++i) {
			decoder->ReadColor(recovered[i],i,0);RISEColor wanted;
			wanted.SetFromIntegerized<Rec709RGBPel,unsigned char>(expected[i],255.0);
			exact &= SameStraightEXRColor(recovered[i],wanted.base,wanted.a);
		}
		Check(exact,png?"EXR straight alpha: normalized Integerize PNG saturates invalid alpha and preserves expected RGBA bytes":
			"EXR straight alpha: normalized Integerize TGA saturates invalid alpha and preserves expected RGBA bytes");
		if(decoder)decoder->release();encoded->release();
		if(!exact)continue;
		auto* roundTrip=new MemoryBuffer();auto* exr=new EXRWriter(*roundTrip,eColorSpace_Rec709RGB_Linear,eExrCompression_None,true,true);
		exr->BeginWrite(pixelCount,1);for(unsigned int i=0;i<pixelCount;++i)exr->WriteColor(recovered[i],i,0);
		exr->EndWrite();exr->release();roundTrip->seek(IBuffer::START,0);
		float associated[pixelCount*4]={};
		{
			IStreamWrapper stream(*roundTrip);Imf::InputFile file(stream);Imf::FrameBuffer frame;
			const char* channels[]={"R","G","B","A"};
			for(unsigned int c=0;c<4;++c)frame.insert(channels[c],Imf::Slice::Make(Imf::FLOAT,associated+c,
				file.header().dataWindow(),4*sizeof(float),pixelCount*4*sizeof(float)));
			file.setFrameBuffer(frame);file.readPixels(0,0);exact=!stream.Failed();
		}
		for(unsigned int i=0;i<pixelCount;++i) {
			const double alpha=expected[i].a*(1.0/255.0);
			const unsigned char rgb[]={expected[i].r,expected[i].g,expected[i].b};
			for(unsigned int c=0;c<3;++c)
				exact &= associated[i*4+c]==static_cast<float>((rgb[c]*(1.0/255.0))*alpha);
			exact &= associated[i*4+3]==static_cast<float>(alpha);
		}
		Check(exact,png?"EXR straight alpha: PNG back-to-EXR has independently decoded associated RGB and alpha":
			"EXR straight alpha: TGA back-to-EXR has independently decoded associated RGB and alpha");
		roundTrip->release();
	}
}

static void TestEXRWithoutAlphaFlattens()
{
	for(bool floats : {false,true}) {
		auto* buffer=new MemoryBuffer();
		auto* writer=new EXRWriter(*buffer,eColorSpace_Rec709RGB_Linear,eExrCompression_None,false,floats);
		writer->BeginWrite(2,1);
		writer->WriteColor(RISEColor(0.25,0.5,0.75,0.25),0,0);
		writer->WriteColor(RISEColor(0.5,0.25,0.125,0),1,0);
		writer->EndWrite();writer->release();buffer->seek(IBuffer::START,0);
		bool exact=false;
		{
			IStreamWrapper stream(*buffer);Imf::RgbaInputFile file(stream);
			Imf::Rgba pixels[2];file.setFrameBuffer(pixels,1,2);file.readPixels(0,0);
			exact=!stream.Failed() && file.header().channels().findChannel("A")==nullptr &&
				float(pixels[0].r)==0.0625f && float(pixels[0].g)==0.125f && float(pixels[0].b)==0.1875f &&
				float(pixels[0].a)==1 && float(pixels[1].r)==0 && float(pixels[1].g)==0 &&
				float(pixels[1].b)==0 && float(pixels[1].a)==1;
		}
		Check(exact,floats?"EXR straight alpha: RGB-only FLOAT export deliberately flattens over black and omits alpha":
			"EXR straight alpha: RGB-only HALF export deliberately flattens over black and omits alpha");
		buffer->release();
	}
}

static void TestEXREmbeddedWriter()
{
	for(bool floats : {false,true}) {
		auto* buffer=new MemoryBuffer();
		const char prefix[]={'p','r','e','f','i','x','!'};
		buffer->setBytes(prefix,sizeof(prefix));
		auto* writer=new EXRWriter(*buffer,eColorSpace_Rec709RGB_Linear,eExrCompression_None,true,floats);
		writer->BeginWrite(2,2);
		for(unsigned int y=0;y<2;++y) for(unsigned int x=0;x<2;++x)
			writer->WriteColor(RISEColor(0.25+0.25*x,0.5,0.75-0.25*y,0.5),x,y);
		writer->EndWrite();writer->release();
		Check(buffer->Size()>sizeof(prefix) && std::memcmp(buffer->Pointer(),prefix,sizeof(prefix))==0,
			"EXR embedded writer: file output preserves its preceding bytes");
		// Remove the prefix and hand a standalone blob to the SDK. Matching
		// producer/consumer origin bugs cannot make this independent read pass.
		std::vector<char> isolated(buffer->Pointer()+sizeof(prefix),buffer->Pointer()+buffer->Size());
		auto* input=new MemoryBuffer(isolated.data(),static_cast<unsigned int>(isolated.size()),false);
		bool exact=false;
		try {
			IStreamWrapper stream(*input);Imf::RgbaInputFile file(stream);
			std::vector<Imf::Rgba> pixels(4);
			file.setFrameBuffer(pixels.data(),1,2);file.readPixels(0,1);
			exact=!stream.Failed() && file.dataWindow()==Imath::Box2i(Imath::V2i(0,0),Imath::V2i(1,1));
			for(unsigned int y=0;y<2;++y) for(unsigned int x=0;x<2;++x) {
				const auto& pixel=pixels[y*2+x];
				exact &= float(pixel.r)==float((0.25+0.25*x)*0.5) && float(pixel.g)==0.25f &&
					float(pixel.b)==float((0.75-0.25*y)*0.5) && float(pixel.a)==0.5f;
			}
		} catch(const std::exception&) {}
		Check(exact,floats?"EXR embedded writer: isolated FLOAT blob has standard offsets and exact SDK pixels":
			"EXR embedded writer: isolated HALF blob has standard offsets and exact SDK pixels");
		input->release();buffer->release();
	}
}

static void TestEXRTruncatedFinalChunk()
{
	const RISEColor poison(-1000,-2000,-3000,-4000);
	const auto same=[](const RISEColor& a,const RISEColor& b) {
		return a.base.r==b.base.r && a.base.g==b.base.g && a.base.b==b.base.b && a.a==b.a;
	};
	for( auto channels : {Imf::WRITE_RGBA,Imf::WRITE_Y,Imf::WRITE_YCA} ) {
		auto* buffer=new MemoryBuffer();
		std::vector<Imf::Rgba> pixels(16);
		for(unsigned int i=0;i<16;++i) {
			const float value=(i+1)/16.0f;
			pixels[i]=Imf::Rgba(value,value,value,0.75f);
		}
		{
			OStreamWrapper stream(*buffer);Imf::Header header(4,4);header.compression()=Imf::NO_COMPRESSION;
			Imf::RgbaOutputFile output(stream,header,channels);
			output.setFrameBuffer(pixels.data(),1,4);output.writePixels(4);
		}
		const std::vector<char> encoded(buffer->Pointer(),buffer->Pointer()+buffer->Size());
		buffer->seek(IBuffer::START,0);
		auto* reader=new EXRReader(*buffer,eColorSpace_Rec709RGB_Linear);
		unsigned int width=0,height=0;
		const bool opened=reader->BeginRead(width,height);
		Check(opened && width==4 && height==4,"EXR final chunk: complete uncompressed input decodes");
		std::vector<RISEColor> expected(16,poison);
		if(opened) for(unsigned int i=0;i<16;++i) reader->ReadColor(expected[i],i%4,i/4);
		for(unsigned int missing : {1,4}) {
			buffer->Resize(static_cast<unsigned int>(encoded.size())-missing,true);
			width=123;height=456;
			const bool rejected=!reader->BeginRead(width,height);
			RISEColor stale=poison;reader->ReadColor(stale,3,3);
			Check(rejected && width==0 && height==0 && same(stale,poison),
				"EXR final chunk: truncated uncompressed payload fails and clears previous pixels");
			buffer->Resize(static_cast<unsigned int>(encoded.size()),true);buffer->seek(IBuffer::START,0);
			buffer->setBytes(encoded.data(),static_cast<unsigned int>(encoded.size()));
			bool recovered=reader->BeginRead(width,height) && width==4 && height==4;
			if(recovered) for(unsigned int i=0;i<16;++i) {
				RISEColor actual=poison;reader->ReadColor(actual,i%4,i/4);recovered &= same(actual,expected[i]);
			}
			Check(opened && recovered,"EXR final chunk: healthy replacement recovers every pixel without caller rewind");
		}
		reader->release();buffer->release();
	}
	// A full read ending exactly at EOF succeeds but returns false, as required
	// by Imf::IStream; a later read past EOF must latch an actual failure.
	char byte='x',actual=0;
	auto* buffer=new MemoryBuffer(&byte,1,false);
	IStreamWrapper stream(*buffer);
	Check(!stream.read(&actual,1) && actual=='x' && !stream.Failed(),
		"EXR stream: exact final-byte read reports EOF without a failure");
	bool threw=false;
	try { stream.read(&actual,1); } catch(const std::runtime_error&) { threw=true; }
	Check(threw && stream.Failed(),"EXR stream: reading past EOF latches a persistent failure");
	buffer->release();
}

static void TestEXRLuminance()
{
	const auto same = [](const RISEColor& actual,const RISEPel& expected,float alpha) {
		const auto close=[](double a,double b){return std::fabs(a-b)<=1e-12*std::max(1.0,std::fabs(b));};
		return close(actual.base.r,expected.r) && close(actual.base.g,expected.g) &&
			close(actual.base.b,expected.b) && actual.a==alpha;
	};
	for(bool alpha : {false,true}) {
		auto* buffer=new MemoryBuffer();
		const Imath::Box2i window(Imath::V2i(-3,7),Imath::V2i(-2,8));
		const float values[]={0.5f,100000,200000,400000};
		const float opacity[]={0.0f,0.5f,0.75f,1};
		{
			OStreamWrapper stream(*buffer);Imf::Header header(2,2);header.dataWindow()=window;
			header.channels().insert("Y",Imf::Channel(Imf::FLOAT));
			if(alpha) header.channels().insert("A",Imf::Channel(Imf::FLOAT));
			Imf::OutputFile file(stream,header);Imf::FrameBuffer frame;
			frame.insert("Y",Imf::Slice::Make(Imf::FLOAT,values,window,sizeof(float),2*sizeof(float)));
			if(alpha) frame.insert("A",Imf::Slice::Make(Imf::FLOAT,opacity,window,sizeof(float),2*sizeof(float)));
			file.setFrameBuffer(frame);file.writePixels(2);
		}
		buffer->seek(IBuffer::START,0);auto* reader=new EXRReader(*buffer,eColorSpace_Rec709RGB_Linear);
		unsigned int width=0,height=0;bool exact=reader->BeginRead(width,height) && width==2 && height==2;
		if(exact) for(unsigned int i=0;i<4;++i) {
			RISEColor actual;reader->ReadColor(actual,i%2,i/2);
			const double a=alpha?opacity[i]:1;
			const double straight=a!=0?double(values[i])/a:double(values[i]);
			exact &= same(actual,RISEPel(Rec709RGBPel(straight,straight,straight)),float(a));
		}
		Check(exact,alpha?"EXR luminance: FLOAT YA preserves range alpha and offset window":
			"EXR luminance: FLOAT Y preserves range and fills opaque alpha");
		reader->release();buffer->release();
	}
	for(auto channels : {Imf::WRITE_YC,Imf::WRITE_YCA}) {
		auto* buffer=new MemoryBuffer();
		const Imath::Box2i window(Imath::V2i(-2,4),Imath::V2i(1,7));
		std::vector<Imf::Rgba> source(16),reference(16);
		for(unsigned int i=0;i<16;++i) source[i]=Imf::Rgba(0.25f+0.125f*(i%4),0.2f+0.1f*(i/4),0.75f-0.125f*(i%4),i==0?0.0f:0.5f);
		{
			OStreamWrapper stream(*buffer);Imf::Header header(4,4);header.dataWindow()=window;
			Imf::RgbaOutputFile file(stream,header,channels);
			file.setFrameBuffer(Imf::ComputeBasePointer(source.data(),window),1,4);file.writePixels(4);
		}
		buffer->seek(IBuffer::START,0);
		{
			IStreamWrapper stream(*buffer);Imf::RgbaInputFile file(stream);
			file.setFrameBuffer(Imf::ComputeBasePointer(reference.data(),window),1,4);file.readPixels(4,7);
		}
		std::vector<char> embedded(7,'!');
		embedded.insert(embedded.end(),buffer->Pointer(),buffer->Pointer()+buffer->Size());
		buffer->Resize(static_cast<unsigned int>(embedded.size()),true);buffer->seek(IBuffer::START,0);
		buffer->setBytes(embedded.data(),static_cast<unsigned int>(embedded.size()));
		buffer->seek(IBuffer::START,7);auto* reader=new EXRReader(*buffer,eColorSpace_Rec709RGB_Linear);
		unsigned int width=0,height=0;bool exact=reader->BeginRead(width,height) && width==4 && height==4;
		if(exact) for(unsigned int i=0;i<16;++i) {
			RISEColor actual;reader->ReadColor(actual,i%4,i/4);const auto& p=reference[i];
			const double alpha=float(p.a);
			const RISEPel straight(Rec709RGBPel(alpha!=0?double(float(p.r))/alpha:double(float(p.r)),
				alpha!=0?double(float(p.g))/alpha:double(float(p.g)),alpha!=0?double(float(p.b))/alpha:double(float(p.b))));
			exact &= same(actual,straight,float(p.a));
		}
		Check(exact,channels==Imf::WRITE_YC?"EXR luminance: prefixed subsampled YC matches independent SDK conversion":
			"EXR luminance: prefixed subsampled YCA matches independent SDK conversion and alpha");
		reader->release();buffer->release();
	}
}

static void TestEXROffsetWindow()
{
	auto* buffer=new MemoryBuffer();
	const Imath::Box2i window(Imath::V2i(-3,7), Imath::V2i(-2,8));
	const float red[] = {100000,200000,300000,400000};
	{
		OStreamWrapper output(*buffer);
		Imf::Header header(2,2);header.dataWindow()=window;
		header.channels().insert("R",Imf::Channel(Imf::FLOAT));
		Imf::OutputFile file(output,header);
		Imf::FrameBuffer frame;
		frame.insert("R",Imf::Slice::Make(Imf::FLOAT,red,window,sizeof(float),2*sizeof(float)));
		file.setFrameBuffer(frame);file.writePixels(2);
	}
	buffer->seek(IBuffer::START,0);
	auto* reader=new EXRReader(*buffer,eColorSpace_Rec709RGB_Linear);
	unsigned int width=0,height=0;
	bool exact=reader->BeginRead(width,height) && width==2 && height==2;
	if(exact) for(unsigned int i=0;i<4;++i) {
		RISEColor actual;reader->ReadColor(actual,i%2,i/2);
		const RISEPel expected(Rec709RGBPel(red[i],0,0));
		const auto close=[](double a,double b){return std::fabs(a-b)<=1e-12*std::max(1.0,std::fabs(b));};
		exact &= close(actual.base.r,expected.r) && close(actual.base.g,expected.g) &&
			close(actual.base.b,expected.b) && actual.a==1;
	}
	Check(exact,"reader family EXR: offset data window preserves FLOAT range and fills missing channels");
	reader->release();buffer->release();
}
#endif

static int RunTests()
{
	TestNormalizedIntegerize();
	TestPremultiplyAlphaRGB();
	TestCallbackExportBounds();
	TestReaderFamilyReload();
#ifndef NO_EXR_SUPPORT
	TestEXRStraightAlpha();
	TestEXRAlphaAcrossFormats();
	TestEXRWithoutAlphaFlattens();
	TestEXREmbeddedWriter();
	TestEXRTruncatedFinalChunk();
	TestEXROffsetWindow();
	TestEXRLuminance();
#endif
	TestRasterStorage();
	TestHDRWriterBounds();
	TestSharedExponentPacking();
	TestHDRRawScanlineMarker();
	TestPPMWriterBounds();
	TestTGAWriterBounds();
	TestJPEGReload();
	TestJPEGCompleteStream();
	std::cout << "ImageReaderOverflowTest — header-driven write-side overflows are rejected"
		<< std::endl;

	TestTGA();
	TestTGAFormat();
	TestHDRNewRLE();
	TestHDROldRLE();
#ifndef NO_PNG_SUPPORT
	TestPNG();
#endif
#ifndef NO_TIFF_SUPPORT
	TestTIFF();
	TestDamagedTIFFStrip();
#endif

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}

int main(int argc,char** argv)
{
	if(argc==2 && std::strcmp(argv[1],"--callback-only")==0) {
		TestCallbackExportBounds();
		std::cout << "CallbackExportTest\nPassed: " << passCount << "\nFailed: " << failCount << std::endl;
		return failCount==0?0:1;
	}
	return RISE::TestSupport::WithOwnedScratch(RunTests);
}
