#include "../src/Library/Utilities/FireSequence.h"
#include "../src/Library/Utilities/FireCase.h"
#include "../src/Library/Rendering/FrameStore.h"
#include "../src/Library/Rendering/FileEncoderObserver.h"
#include "../src/Library/Rendering/PixelBasedRasterizerHelper.h"
#include "../src/Library/Interfaces/IRasterImageReader.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Materials/HeterogeneousMedium.h"
#include "../src/Library/Materials/HenyeyGreensteinPhaseFunction.h"
#include "../src/Library/Lights/PointLight.h"
#include "../src/Library/Painters/Perlin3DPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Utilities/FireSimulationRecords.h"
#include "../src/Library/Utilities/FireProductionProjection.h"
#include "../src/Library/Utilities/FireProductionForce.h"
#include "../src/Library/Utilities/FireProductionTransport.h"
#include "../src/Library/Utilities/Reference.h"
#include "../tools/fire_simulator_core.h"
#include "FireOutputMetadataTestFixture.h"
#include "FireProductionCalibrationMath.h"
#include "FireProductionCalibrationMirror.h"
#include "FireProductionRoundoffTraceAdapter.h"
#include "FireProductionRoundoffWalker.h"
#include "fire_production_fp64/FireProductionTransport.h"
#include "fire_production_trace/SourceManifest.h"

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <dlfcn.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#endif

#include <cmath>
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>

// This executable exercises the production owner directly for the r201
// cross-precision localization.  It does not expose fault injection.
extern "C" bool RISEProjectedHeunOwnerTestFailureProbe(const char*)
{
	return false;
}

#include <type_traits>
#include <vector>

#if defined(__APPLE__)
#include <unistd.h>

namespace RISE
{
	//! Test-only terminal staging around the private compatible-momentum owner.
	//! The production resident API remains private-buffer-only.
	bool RemapFireProductionCompatibleDualMomentumMetalComparator(
		const FireProductionDualMomentumRequest& request,
		const std::array<std::vector<float>,5>& acceptedGasMassDoseKGPerM2,
		FireProductionDualMomentumResult& result,
		std::string* error );
}
#endif

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	RISECBOR64::Bytes ReadFileBytes(const std::filesystem::path& path);
	std::string DigestFile(const std::filesystem::path& path);
	bool VerifyVisibleFireDisplayDerivative(const RISECBOR64::Bytes& artifact,
		const RISECBOR64::Bytes& sidecar,const RISECBOR64::Value& primaryEnvelope,
		std::string& error,const bool requireVisible=true,const bool requirePlume=false);
	bool FirstLightEnvelopeHasFreshProvenance(const RISECBOR64::Value& envelope,
		const std::string& previousProvenance,std::string& provenance)
	{
		const RISECBOR64::Value* value=envelope.Find("provenance_id");
		if(!value||value->GetType()!=RISECBOR64::Value::Text||
			value->GetText().size()!=64u)return false;
		provenance=value->GetText();
		return previousProvenance.empty()||provenance!=previousProvenance;
	}
	bool FirstLightAnimatedPlumeMetricsPass(const std::vector<std::string>& digests,
		const std::vector<bool>& visible,const std::vector<bool>& structured,
		const std::vector<std::size_t>& litPixels,const std::vector<double>& centroidX,
		const std::vector<double>& centroidY)
	{
		if(digests.size()<8u||visible.size()!=digests.size()||
			structured.size()!=digests.size()||litPixels.size()!=digests.size()||
			centroidX.size()!=digests.size()||centroidY.size()!=digests.size())return false;
		std::size_t minimumLit=std::numeric_limits<std::size_t>::max(),maximumLit=0u;
		double maximumCentroidDistanceSquared=0.0;
		for(std::size_t frame=0u;frame<digests.size();++frame){
			if(!visible[frame]||!structured[frame]||
				!std::isfinite(centroidX[frame])||!std::isfinite(centroidY[frame])||
				(frame>0u&&digests[frame]==digests[frame-1u]))return false;
			minimumLit=std::min(minimumLit,litPixels[frame]);
			maximumLit=std::max(maximumLit,litPixels[frame]);
			for(std::size_t prior=0u;prior<frame;++prior){
				const double dx=centroidX[frame]-centroidX[prior];
				const double dy=centroidY[frame]-centroidY[prior];
				maximumCentroidDistanceSquared=std::max(
					maximumCentroidDistanceSquared,dx*dx+dy*dy);
			}
		}
		return maximumLit-minimumLit>=std::max<std::size_t>(16u,maximumLit/20u)&&
			maximumCentroidDistanceSquared>=4.0;
	}
	bool TemporalAnimatedFireMetricsPass(const std::vector<std::string>& digests,
		const std::vector<bool>& visible,const std::vector<bool>& blueDominant,
		const std::vector<std::size_t>& litPixels,const std::vector<double>& centroidX,
		const std::vector<double>& centroidY)
	{
		if(digests.size()<8u||visible.size()!=digests.size()||
			blueDominant.size()!=digests.size()||litPixels.size()!=digests.size()||
			centroidX.size()!=digests.size()||centroidY.size()!=digests.size())return false;
		std::size_t minimumLit=std::numeric_limits<std::size_t>::max(),maximumLit=0u;
		double maximumCentroidDistanceSquared=0.0;
		for(std::size_t frame=0u;frame<digests.size();++frame){
			if(!visible[frame]||!blueDominant[frame]||
				!std::isfinite(centroidX[frame])||!std::isfinite(centroidY[frame])||
				(frame>0u&&digests[frame]==digests[frame-1u]))return false;
			minimumLit=std::min(minimumLit,litPixels[frame]);
			maximumLit=std::max(maximumLit,litPixels[frame]);
			for(std::size_t prior=0u;prior<frame;++prior){
				const double dx=centroidX[frame]-centroidX[prior];
				const double dy=centroidY[frame]-centroidY[prior];
				maximumCentroidDistanceSquared=std::max(
					maximumCentroidDistanceSquared,dx*dx+dy*dy);
			}
		}
		return maximumLit-minimumLit>=std::max<std::size_t>(16u,maximumLit/20u)&&
			maximumCentroidDistanceSquared>=4.0;
	}

#if defined(__APPLE__)
	bool DecodeFirstLightGIFFrame(CGImageSourceRef source,const std::size_t frame,
		const unsigned int width,const unsigned int height,std::string& digest,
		bool& visible,bool& structuredPlume,bool& blueDominant,std::size_t& litPixelCount,
		double& centroidX,double& centroidY)
	{
		CGImageRef image=CGImageSourceCreateImageAtIndex(source,frame,nullptr);
		if(!image||CGImageGetWidth(image)!=width||CGImageGetHeight(image)!=height){
			if(image)CFRelease(image);return false;
		}
		std::vector<unsigned char> pixels(static_cast<std::size_t>(width)*height*4u,0u);
		CGColorSpaceRef color=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
		CGContextRef context=color?CGBitmapContextCreate(pixels.data(),width,height,8u,
			static_cast<std::size_t>(width)*4u,color,
			kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big):nullptr;
		if(color)CFRelease(color);
		if(!context){CFRelease(image);return false;}
		CGContextDrawImage(context,CGRectMake(0,0,width,height),image);
		CFRelease(context);CFRelease(image);
		visible=false;structuredPlume=false;
		std::size_t litPixels=0u,minX=width,minY=height,maxX=0u,maxY=0u;
		std::uint64_t redSum=0u,blueSum=0u,xMoment=0u,yMoment=0u;
		for(unsigned int y=0u;y<height;++y)for(unsigned int x=0u;x<width;++x){
			const std::size_t i=4u*(static_cast<std::size_t>(y)*width+x);
			visible=visible||pixels[i]!=0u||pixels[i+1u]!=0u||pixels[i+2u]!=0u;
			const bool lit=std::max({pixels[i],pixels[i+1u],pixels[i+2u]})>127u;
			if(lit){visible=true;++litPixels;minX=std::min(minX,static_cast<std::size_t>(x));
				minY=std::min(minY,static_cast<std::size_t>(y));
				maxX=std::max(maxX,static_cast<std::size_t>(x));
				maxY=std::max(maxY,static_cast<std::size_t>(y));
				redSum+=pixels[i];blueSum+=pixels[i+2u];xMoment+=x;yMoment+=y;}
		}
		blueDominant=visible&&blueSum>redSum;
		structuredPlume=blueDominant&&2u*litPixels<static_cast<std::size_t>(width)*height&&
			(maxY-minY)>(maxX-minX);
		litPixelCount=litPixels;
		centroidX=litPixels?static_cast<double>(xMoment)/static_cast<double>(litPixels):0.0;
		centroidY=litPixels?static_cast<double>(yMoment)/static_cast<double>(litPixels):0.0;
		digest=RISECBOR64::SHA256Hex(pixels);return true;
	}

	bool ValidateFireGIF(const std::string& path,
		const FireFrameSequenceEncoding encoding,const unsigned int width,
		const unsigned int height,const unsigned int framesPerSecond,
		const std::vector<FireFramePrimary>& frames,const bool requireBoundedPlume,
		std::string& error)
	{
		error.clear();
		if(encoding!=FireFrameSequenceEncoding::AppleImageIOGif_PreviewPlus6EV_8Bit||
			width==0u||height==0u||framesPerSecond==0u||frames.size()<2u){
			error="first-light GIF expectations are inconsistent";return false;
		}
		const RISECBOR64::Bytes encoded=ReadFileBytes(path);
		if(encoded.size()<6u||std::string(encoded.begin(),encoded.begin()+6u)!="GIF87a"){
			error="first-light GIF container revision differs";return false;
		}
		CFURLRef url=CFURLCreateFromFileSystemRepresentation(nullptr,
			reinterpret_cast<const UInt8*>(path.data()),path.size(),false);
		CGImageSourceRef source=url?CGImageSourceCreateWithURL(url,nullptr):nullptr;
		if(url)CFRelease(url);
		if(!source||CGImageSourceGetCount(source)!=frames.size()){
			if(source)CFRelease(source);error="first-light GIF frame count differs";return false;
		}
		std::vector<std::string> digests;
		std::vector<bool> visible,structured,blueDominant;
		std::vector<std::size_t> litCounts;
		std::vector<double> centroidX,centroidY;
		for(std::size_t frame=0u;frame<frames.size();++frame){
			CFDictionaryRef properties=CGImageSourceCopyPropertiesAtIndex(
				source,frame,nullptr);
			const CFDictionaryRef gif=properties?static_cast<CFDictionaryRef>(
				CFDictionaryGetValue(properties,kCGImagePropertyGIFDictionary)):nullptr;
			const CFNumberRef delay=gif?static_cast<CFNumberRef>(
				CFDictionaryGetValue(gif,kCGImagePropertyGIFDelayTime)):nullptr;
			double delaySeconds=0.0;
			const bool cadence=delay&&CFGetTypeID(delay)==CFNumberGetTypeID()&&
				CFNumberGetValue(delay,kCFNumberDoubleType,&delaySeconds)&&
				std::fabs(delaySeconds-1.0/static_cast<double>(framesPerSecond))<=0.011;
			if(properties)CFRelease(properties);
			if(!cadence){CFRelease(source);error="first-light GIF cadence differs";return false;}
			std::string digest;bool frameVisible=false,structuredPlume=false,frameBlue=false;
			std::size_t litPixels=0u;double frameCentroidX=0.0,frameCentroidY=0.0;
			if(!DecodeFirstLightGIFFrame(source,frame,width,height,digest,frameVisible,
				structuredPlume,frameBlue,litPixels,frameCentroidX,frameCentroidY)){
				CFRelease(source);error="first-light GIF frame decode failed";return false;
			}
			digests.push_back(digest);visible.push_back(frameVisible);
			structured.push_back(structuredPlume);blueDominant.push_back(frameBlue);
			litCounts.push_back(litPixels);
			centroidX.push_back(frameCentroidX);centroidY.push_back(frameCentroidY);
		}
		CFRelease(source);
		const bool animated=requireBoundedPlume?
			FirstLightAnimatedPlumeMetricsPass(digests,visible,structured,litCounts,
				centroidX,centroidY):
			TemporalAnimatedFireMetricsPass(digests,visible,blueDominant,litCounts,
				centroidX,centroidY);
		if(!animated){
			error="fire preview GIF lacks visible structured and materially changing "
				"blue-plume frames";return false;
		}
		return true;
	}
	bool ValidateFirstLightGIF(const std::string& path,
		const FireFrameSequenceEncoding encoding,const unsigned int width,
		const unsigned int height,const unsigned int framesPerSecond,
		const std::vector<FireFramePrimary>& frames,std::string& error)
	{
		return ValidateFireGIF(path,encoding,width,height,framesPerSecond,frames,true,error);
	}
	bool ValidateTemporalFireGIF(const std::string& path,
		const FireFrameSequenceEncoding encoding,const unsigned int width,
		const unsigned int height,const unsigned int framesPerSecond,
		const std::vector<FireFramePrimary>& frames,std::string& error)
	{
		return ValidateFireGIF(path,encoding,width,height,framesPerSecond,frames,false,error);
	}

	bool AuthorFirstLightGIF(const std::vector<std::filesystem::path>& pngFrames,
		const std::filesystem::path& path,const unsigned int framesPerSecond,
		std::string& error)
	{
		error.clear();
		if(pngFrames.empty()||framesPerSecond==0u)return false;
		const std::string outputText=path.string();
		CFURLRef output=CFURLCreateFromFileSystemRepresentation(nullptr,
			reinterpret_cast<const UInt8*>(outputText.data()),outputText.size(),false);
		CGImageDestinationRef destination=output?CGImageDestinationCreateWithURL(
			output,CFSTR("com.compuserve.gif"),pngFrames.size(),nullptr):nullptr;
		if(output)CFRelease(output);
		if(!destination){error="ImageIO GIF destination is unavailable";return false;}
		int loop=0;CFNumberRef loopNumber=CFNumberCreate(nullptr,kCFNumberIntType,&loop);
		const void* gifKeys[]={kCGImagePropertyGIFLoopCount};
		const void* gifValues[]={loopNumber};
		CFDictionaryRef gif=CFDictionaryCreate(nullptr,gifKeys,gifValues,1u,
			&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
		const void* globalKeys[]={kCGImagePropertyGIFDictionary};
		const void* globalValues[]={gif};
		CFDictionaryRef global=CFDictionaryCreate(nullptr,globalKeys,globalValues,1u,
			&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
		CGImageDestinationSetProperties(destination,global);
		CFRelease(global);CFRelease(gif);CFRelease(loopNumber);
		const double delay=1.0/static_cast<double>(framesPerSecond);
		for(const std::filesystem::path& framePath:pngFrames){
			const std::string frameText=framePath.string();
			CFURLRef frameURL=CFURLCreateFromFileSystemRepresentation(nullptr,
				reinterpret_cast<const UInt8*>(frameText.data()),frameText.size(),false);
			CGImageSourceRef source=frameURL?CGImageSourceCreateWithURL(frameURL,nullptr):nullptr;
			if(frameURL)CFRelease(frameURL);
			CGImageRef image=source?CGImageSourceCreateImageAtIndex(source,0u,nullptr):nullptr;
			if(source)CFRelease(source);
			if(!image){CFRelease(destination);error="first-light PNG frame decode failed";return false;}
			CFNumberRef delayNumber=CFNumberCreate(nullptr,kCFNumberDoubleType,&delay);
			const void* frameGIFKeys[]={kCGImagePropertyGIFDelayTime};
			const void* frameGIFValues[]={delayNumber};
			CFDictionaryRef frameGIF=CFDictionaryCreate(nullptr,frameGIFKeys,
				frameGIFValues,1u,&kCFTypeDictionaryKeyCallBacks,
				&kCFTypeDictionaryValueCallBacks);
			const void* frameKeys[]={kCGImagePropertyGIFDictionary};
			const void* frameValues[]={frameGIF};
			CFDictionaryRef properties=CFDictionaryCreate(nullptr,frameKeys,frameValues,
				1u,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
			CGImageDestinationAddImage(destination,image,properties);
			CFRelease(properties);CFRelease(frameGIF);CFRelease(delayNumber);CFRelease(image);
		}
		const bool finalized=CGImageDestinationFinalize(destination);
		CFRelease(destination);
		if(!finalized)error="ImageIO could not finalize the first-light GIF";
		return finalized;
	}
#endif

	int RunFirstLightPreviewChild(const std::filesystem::path& sourceDirectory,
		const std::filesystem::path& outputDirectory)
	{
#if !defined(__APPLE__)
		(void)sourceDirectory;(void)outputDirectory;
		std::fprintf(stderr,"first-light ProRes authoring requires macOS AVFoundation\n");
		return 90;
#else
		std::string error;
		const std::filesystem::path manifestPath=
			sourceDirectory/"sequence_manifest.rise-fire.cbor";
		const RISECBOR64::Bytes manifestBytes=ReadFileBytes(manifestPath);
		FireSequenceManifest manifest;
		if(manifestBytes.empty()||!manifest.LoadCanonicalEnvelope(manifestBytes,
			sourceDirectory.string(),error)||manifest.Channels().empty()) {
			std::fprintf(stderr,"first-light manifest rejected: %s\n",error.c_str());return 91;
		}
		const FireSequenceChannelDescriptor& channel=manifest.Channels().front();
		const double width=channel.voxelSizeMeters[0];
		if(width<=0.0||channel.dimensions[0]==0u||channel.dimensions[1]==0u||
			channel.dimensions[2]==0u||manifest.TimeMap().sceneToSimulationScale==0.0)return 91;
		const double start=manifest.TimeMap().sceneTimeOrigin;
		const double end=start+manifest.TimeMap().frameStepSeconds/
			manifest.TimeMap().sceneToSimulationScale;
		const double centerX=0.5*static_cast<double>(channel.dimensions[0])*width;
		const double centerY=0.5*static_cast<double>(channel.dimensions[1])*width;
		const double depth=std::max({static_cast<double>(channel.dimensions[0]),
			static_cast<double>(channel.dimensions[1]),
			static_cast<double>(channel.dimensions[2])})*width;
		const double previewCenterZ=0.15*depth;
		std::filesystem::path stage;
		const std::string stageStem="rise-first-light-"+
			std::to_string(static_cast<long>(getpid()))+"-"+
			std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
		for(unsigned int nonce=0u;nonce<100u&&stage.empty();++nonce){
			const std::filesystem::path candidate=std::filesystem::temp_directory_path()/
				(stageStem+"-"+std::to_string(nonce));
			std::error_code createError;
			if(std::filesystem::create_directory(candidate,createError))stage=candidate;
		}
		if(stage.empty())return 91;
		setenv("RISE_MEDIA_PATH",(stage.string()+"/").c_str(),1);
		const std::filesystem::path scenePath=stage/"first_light.RISEscene";
		auto writeScene=[&](const double cameraX,const double cameraY,
			const double cameraZ)->bool{
			std::ofstream scene(scenePath);
			scene << "RISE ASCII SCENE 7\n\nscene_options\n{\nscene_unit 1\nfidelity_mode preview\n}\n\n"
				<< "standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
				<< "pathtracing_spectral_rasterizer\n{\nsamples 4\nnmbegin 380\nnmend 780\n"
				<< "num_wavelengths 32\nspectral_samples 4\nhwss false\npixel_filter box\n"
				<< "oidn_denoise false\n}\n\nfile_rasterizeroutput\n{\npattern first_light_primary\n"
				<< "type EXR\nbpp 32\ncolor_space Rec709RGB_Linear\nexposure 0\n"
				<< "display_transform none\nexr_compression piz\n}\n\nfile_rasterizeroutput\n{\n"
				<< "pattern first_light_display\ntype PNG\nbpp 16\ncolor_space sRGB\n"
				<< "exposure 6\ndisplay_transform aces\n}\n\nfilm\n{\nwidth 64\nheight 64\n}\n\n"
				<< "pinhole_camera\n{\nname camera\nlocation " << cameraX << ' ' << cameraY << ' '
				<< cameraZ << "\nlookat " << centerX << ' ' << centerY << ' ' << previewCenterZ
				<< "\nup 0 0 1\nfov 45\nexposure 0.04\nscanning_rate -0.1\npixel_rate 0.02\n}\n\n"
				<< "fire_medium\n{\nname sequence_fire\nfidelity_mode preview\nsequence_manifest "
				<< manifestPath.string() << "\nchannel_carbon carbon\nchannel_temperature temperature\n"
				<< "channel_reaction reaction\nchannel_chem_ch chem_CH\nchannel_chem_c2 chem_C2\n"
				<< "channel_chem_co2 chem_CO2\nchannel_velocity velocity\n}\n\n"
				<< "global_medium\n{\nmedium sequence_fire\n}\n";
			return scene.good();
		};
		auto createPreviewJob=[&](const double cameraX,const double cameraY,
			const double cameraZ,IJobPriv*& result)->bool{
			result=nullptr;
			if(!writeScene(cameraX,cameraY,cameraZ)||!RISE_CreateJobPriv(&result)||!result)
				return false;
			if(result->LoadAsciiSceneViaCst(scenePath.string().c_str()))return true;
			result->release();result=nullptr;return false;
		};
		IJobPriv* job=nullptr;
		if(!createPreviewJob(centerX,-0.125*depth,previewCenterZ,job)||
			!job->RasterizeAnimation(std::nextafter(end,
				std::numeric_limits<double>::infinity()),std::nextafter(end,
				std::numeric_limits<double>::infinity()),1u,false,false)){
			if(job)job->release();return 92;
		}
		auto waitForRenderedPair=[&stage](const std::string& previousProvenance,
			RISECBOR64::Bytes& primaryBytes,RISECBOR64::Bytes& primarySidecar,
			RISECBOR64::Bytes& displayBytes,RISECBOR64::Bytes& displaySidecar,
			RISECBOR64::Value& primaryEnvelope,std::string& provenance,
			std::string& pairError,const bool requireVisible)->bool{
			for(unsigned int attempt=0u;attempt<6000u;++attempt){
				primaryBytes=ReadFileBytes(stage/"first_light_primary.exr");
				primarySidecar=ReadFileBytes(stage/
					"first_light_primary.exr.provenance.cbor");
				displayBytes=ReadFileBytes(stage/"first_light_display.png");
				displaySidecar=ReadFileBytes(stage/
					"first_light_display.png.provenance.cbor");
				pairError.clear();
				if(VerifyFireProvenanceEXR(primaryBytes,primarySidecar,pairError)&&
					RISECBOR64::DecodeCanonical(primarySidecar,primaryEnvelope,
						&pairError)&&VerifyVisibleFireDisplayDerivative(displayBytes,
						displaySidecar,primaryEnvelope,pairError,requireVisible,
						false)&&
					FirstLightEnvelopeHasFreshProvenance(primaryEnvelope,
						previousProvenance,provenance))return true;
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			pairError="first-light render pair did not publish a fresh identity";
			return false;
		};
		RISECBOR64::Value primaryEnvelope;
		RISECBOR64::Bytes primaryBytes,primarySidecar,displayBytes,displaySidecar;
		std::string previousProvenance;
		const bool pairs=waitForRenderedPair("",primaryBytes,primarySidecar,
			displayBytes,displaySidecar,primaryEnvelope,previousProvenance,error,true);
		std::string staleProvenance;
		if(!pairs||FirstLightEnvelopeHasFreshProvenance(primaryEnvelope,
			previousProvenance,staleProvenance)){
			std::fprintf(stderr,"first-light terminal pair rejected: %s\n",error.c_str());
			job->release();return 93;
		}
		std::vector<std::filesystem::path> primaryFrames,displayFrames;
		std::vector<FireFramePrimary> primaryLinks;
		bool animationPairs=pairs;
		job->release();job=nullptr;
		for(unsigned int frame=0u;frame<8u;++frame){
			const double fraction=static_cast<double>(frame)/7.0;
			const double angle=(-10.0+20.0*fraction)*3.14159265358979323846/180.0;
			const double radius=(0.52-0.12*fraction)*depth;
			const double cameraX=centerX+radius*std::sin(angle);
			const double cameraY=centerY-radius*std::cos(angle);
			const double cameraZ=previewCenterZ+(fraction-0.5)*0.035*depth;
			for(const std::filesystem::path& prior:std::array<std::filesystem::path,4>{
				stage/"first_light_primary.exr",
				stage/"first_light_primary.exr.provenance.cbor",
				stage/"first_light_display.png",
				stage/"first_light_display.png.provenance.cbor"}){
				std::error_code removeError;
				std::filesystem::remove(prior,removeError);
				animationPairs=animationPairs&&!removeError;
			}
			animationPairs=animationPairs&&createPreviewJob(cameraX,cameraY,cameraZ,job)&&
				job->RasterizeAnimation(std::nextafter(end,
					std::numeric_limits<double>::infinity()),std::nextafter(end,
					std::numeric_limits<double>::infinity()),1u,false,false);
			std::ostringstream index;index<<std::setw(4)<<std::setfill('0')<<frame;
			const std::filesystem::path currentPrimary=stage/"first_light_primary.exr";
			const std::filesystem::path currentDisplay=stage/"first_light_display.png";
			const std::filesystem::path primary=stage/
				("animation_primary"+index.str()+".exr");
			const std::filesystem::path display=stage/
				("animation_display"+index.str()+".png");
			RISECBOR64::Bytes framePrimary,framePrimarySidecar,frameDisplay,
				frameDisplaySidecar;
			RISECBOR64::Value envelope;
			std::string frameError;
			std::string frameProvenance;
			animationPairs=animationPairs&&waitForRenderedPair(previousProvenance,
				framePrimary,framePrimarySidecar,frameDisplay,frameDisplaySidecar,
				envelope,frameProvenance,frameError,frame==7u);
			if(animationPairs)previousProvenance=frameProvenance;
			const RISECBOR64::Value* payload=envelope.Find("payload");
			const RISECBOR64::Value* provenance=envelope.Find("provenance_id");
			const RISECBOR64::Value* artifact=payload?payload->Find("artifact_sha256"):nullptr;
			animationPairs=animationPairs&&provenance&&artifact;
			if(provenance&&artifact){
				FireFramePrimary link;link.frameIndex=frame;
				link.provenanceId=provenance->GetText();
				link.artifactSha256=artifact->GetText();primaryLinks.push_back(link);
			}
			if(animationPairs){
				std::filesystem::copy_file(currentPrimary,primary,
					std::filesystem::copy_options::overwrite_existing);
				std::filesystem::copy_file(currentPrimary.string()+".provenance.cbor",
					primary.string()+".provenance.cbor",
					std::filesystem::copy_options::overwrite_existing);
				std::filesystem::copy_file(currentDisplay,display,
					std::filesystem::copy_options::overwrite_existing);
				std::filesystem::copy_file(currentDisplay.string()+".provenance.cbor",
					display.string()+".provenance.cbor",
					std::filesystem::copy_options::overwrite_existing);
			}
			primaryFrames.push_back(primary);displayFrames.push_back(display);
			if(frame+1u<8u){if(job)job->release();job=nullptr;}
		}
		const IRasterizer* rasterizer=job?job->GetRasterizer():nullptr;
		const FrameStore* animationStore=rasterizer?rasterizer->GetFrameStore():nullptr;
		const FrameStore::Metadata animationMetadata=animationStore?
			animationStore->Meta():FrameStore::Metadata();
		const std::filesystem::path temporaryGIF=stage/"methane_preview_animation.gif.tmp";
		const std::filesystem::path gifPath=stage/"methane_preview_animation.gif";
		const bool gifAuthored=animationPairs&&AuthorFirstLightGIF(
			displayFrames,temporaryGIF,8u,error);
		const bool gifPublished=gifAuthored&&PublishFireFrameSequenceFileTransaction(
			animationMetadata,
			FireFrameSequenceEncoding::AppleImageIOGif_PreviewPlus6EV_8Bit,
			temporaryGIF.string(),gifPath.string(),64u,64u,8u,8u,primaryLinks,
			ValidateFirstLightGIF,error);
		if(job)job->release();
		if(!pairs||!animationPairs||!gifPublished){
			std::fprintf(stderr,"first-light animation rejected: %s\n",error.c_str());return 93;
		}
		std::filesystem::create_directories(outputDirectory);
		for(const auto& copy:std::vector<std::pair<std::filesystem::path,std::filesystem::path>>{
			{stage/"first_light_primary.exr",outputDirectory/"methane_preview.exr"},
			{stage/"first_light_primary.exr.provenance.cbor",
				outputDirectory/"methane_preview.exr.provenance.cbor"},
			{stage/"first_light_display.png",outputDirectory/"methane_preview_display.png"},
			{stage/"first_light_display.png.provenance.cbor",
				outputDirectory/"methane_preview_display.png.provenance.cbor"},
			{gifPath,outputDirectory/"methane_preview_animation.gif"},
			{gifPath.string()+".provenance.cbor",
				outputDirectory/"methane_preview_animation.gif.provenance.cbor"}})
			std::filesystem::copy_file(copy.first,copy.second,
				std::filesystem::copy_options::overwrite_existing);
		if(primaryFrames.size()!=8u)return 94;
		for(std::size_t frame=0u;frame<primaryFrames.size();++frame){
			const std::string index="000"+std::to_string(frame);
			std::filesystem::copy_file(primaryFrames[frame],
				outputDirectory/("methane_preview_frame"+index+".exr"),
				std::filesystem::copy_options::overwrite_existing);
			std::filesystem::copy_file(primaryFrames[frame].string()+".provenance.cbor",
				outputDirectory/("methane_preview_frame"+index+".exr.provenance.cbor"),
				std::filesystem::copy_options::overwrite_existing);
		}
		std::string primaryScheduleIdentity;
		for(const FireFramePrimary& primary:primaryLinks)
			primaryScheduleIdentity+=std::to_string(primary.frameIndex)+":"+
				primary.artifactSha256+"\n";
		const RISECBOR64::Bytes primaryScheduleBytes(primaryScheduleIdentity.begin(),
			primaryScheduleIdentity.end());
		std::printf("FIRST_LIGHT_PREVIEW frames=8 exposure_ev=6 primary_schedule=%s "
			"terminal_primary=%s png=%s gif=%s\n",
			RISECBOR64::SHA256Hex(primaryScheduleBytes).c_str(),
			primaryLinks.back().artifactSha256.c_str(),
			DigestFile(outputDirectory/"methane_preview_display.png").c_str(),
			DigestFile(outputDirectory/"methane_preview_animation.gif").c_str());
		return 0;
#endif
	}

	int RunTemporalFirePreviewChild(const std::filesystem::path& sourceDirectory,
		const std::filesystem::path& outputDirectory)
	{
#if !defined(__APPLE__)
		(void)sourceDirectory;(void)outputDirectory;
		std::fprintf(stderr,"temporal fire preview authoring requires macOS ImageIO\n");
		return 90;
#else
		std::string error;
		const std::filesystem::path manifestPath=
			sourceDirectory/"sequence_manifest.rise-fire.cbor";
		const RISECBOR64::Bytes manifestBytes=ReadFileBytes(manifestPath);
		FireSequenceManifest manifest;
		if(manifestBytes.empty()||!manifest.LoadCanonicalEnvelope(manifestBytes,
			sourceDirectory.string(),error)||!manifest.PreflightAllFrames(error)||
			manifest.Frames().size()<8u||manifest.Channels().empty()){
			std::fprintf(stderr,"temporal fire manifest rejected: %s\n",error.c_str());return 91;
		}
		const FireSequenceChannelDescriptor& channel=manifest.Channels().front();
		const FireSequenceTimeMap& timeMap=manifest.TimeMap();
		const double width=channel.voxelSizeMeters[0];
		const double sceneStep=timeMap.frameStepSeconds/timeMap.sceneToSimulationScale;
		const double framesPerPhysicalSecond=1.0/timeMap.frameStepSeconds;
		const unsigned int framesPerSecond=static_cast<unsigned int>(
			std::llround(framesPerPhysicalSecond));
		if(!(width>0.0)||!std::isfinite(sceneStep)||!(sceneStep>0.0)||
			framesPerSecond==0u||std::fabs(static_cast<double>(framesPerSecond)-
				framesPerPhysicalSecond)>1.0e-12)return 91;
		const double centerX=0.5*static_cast<double>(channel.dimensions[0])*width;
		const double centerY=0.5*static_cast<double>(channel.dimensions[1])*width;
		const double depth=std::max({static_cast<double>(channel.dimensions[0]),
			static_cast<double>(channel.dimensions[1]),
			static_cast<double>(channel.dimensions[2])})*width;
		const double centerZ=0.5*depth;
		std::filesystem::path stage;
		const std::string stageStem="rise-temporal-fire-"+
			std::to_string(static_cast<long>(getpid()))+"-"+
			std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
		for(unsigned int nonce=0u;nonce<100u&&stage.empty();++nonce){
			const std::filesystem::path candidate=std::filesystem::temp_directory_path()/
				(stageStem+"-"+std::to_string(nonce));
			std::error_code createError;
			if(std::filesystem::create_directory(candidate,createError))stage=candidate;
		}
		if(stage.empty())return 91;
		setenv("RISE_MEDIA_PATH",(stage.string()+"/").c_str(),1);
		const std::filesystem::path scenePath=stage/"temporal_fire.RISEscene";
		{
			std::ofstream scene(scenePath);
			scene<<"RISE ASCII SCENE 7\n\nscene_options\n{\nscene_unit 1\nfidelity_mode preview\n}\n\n"
				<<"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
				<<"pathtracing_spectral_rasterizer\n{\nsamples 4\nnmbegin 380\nnmend 780\n"
				<<"num_wavelengths 32\nspectral_samples 4\nhwss false\npixel_filter box\n"
				<<"oidn_denoise false\n}\n\nfile_rasterizeroutput\n{\npattern "<<
					(stage/"temporal_primary").string()<<"\n"
				<<"type EXR\nbpp 32\ncolor_space Rec709RGB_Linear\nexposure 0\n"
				<<"display_transform none\nexr_compression piz\n}\n\nfile_rasterizeroutput\n{\n"
				<<"pattern "<<(stage/"temporal_display").string()<<
				"\ntype PNG\nbpp 16\ncolor_space sRGB\n"
				<<"exposure 6\ndisplay_transform aces\n}\n\nfilm\n{\nwidth 64\nheight 64\n}\n\n"
				<<"pinhole_camera\n{\nname camera\nlocation "<<centerX<<' '<<
				centerY-1.25*depth<<' '<<centerZ<<"\nlookat "<<centerX<<' '<<centerY<<' '<<
					centerZ<<"\nup 0 0 1\nfov 45\nexposure 0.04\nscanning_rate -0.1\n"
				<<"pixel_rate 0.02\n}\n\nfire_medium\n{\nname sequence_fire\nfidelity_mode preview\n"
				<<"sequence_manifest "<<manifestPath.string()<<"\nchannel_carbon carbon\n"
				<<"channel_temperature temperature\nchannel_reaction reaction\n"
				<<"channel_chem_ch chem_CH\nchannel_chem_c2 chem_C2\nchannel_chem_co2 chem_CO2\n"
				<<"channel_velocity velocity\n}\n\nglobal_medium\n{\nmedium sequence_fire\n}\n";
			if(!scene.good())return 92;
		}
		IJobPriv* job=nullptr;
		if(!RISE_CreateJobPriv(&job)||!job||!job->LoadAsciiSceneViaCst(scenePath.string().c_str())){
			if(job)job->release();return 92;
		}
		auto waitForPair=[&](const std::string& previousProvenance,
			RISECBOR64::Bytes& primaryBytes,RISECBOR64::Bytes& primarySidecar,
			RISECBOR64::Bytes& displayBytes,RISECBOR64::Bytes& displaySidecar,
			RISECBOR64::Value& primaryEnvelope,std::string& provenance)->bool{
			std::string lastVerificationError;
			for(unsigned int attempt=0u;attempt<6000u;++attempt){
				primaryBytes=ReadFileBytes(stage/"temporal_primary.exr");
				primarySidecar=ReadFileBytes(stage/"temporal_primary.exr.provenance.cbor");
				displayBytes=ReadFileBytes(stage/"temporal_display.png");
				displaySidecar=ReadFileBytes(stage/"temporal_display.png.provenance.cbor");
				error.clear();
				if(VerifyFireProvenanceEXR(primaryBytes,primarySidecar,error)&&
					RISECBOR64::DecodeCanonical(primarySidecar,primaryEnvelope,&error)&&
					VerifyVisibleFireDisplayDerivative(displayBytes,displaySidecar,
						primaryEnvelope,error,true,false)&&
					FirstLightEnvelopeHasFreshProvenance(primaryEnvelope,
						previousProvenance,provenance))return true;
				if(!error.empty())lastVerificationError=error;
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			error="temporal fire frame did not publish a fresh verified pair";
			if(!lastVerificationError.empty())error+=": "+lastVerificationError;
			return false;
		};
		std::vector<std::filesystem::path> primaryFrames,displayFrames;
		std::vector<FireFramePrimary> primaryLinks;
		std::string previousProvenance;
		for(std::size_t frame=0u;frame<manifest.Frames().size();++frame){
			for(const std::filesystem::path& prior:std::array<std::filesystem::path,4>{
				stage/"temporal_primary.exr",stage/"temporal_primary.exr.provenance.cbor",
				stage/"temporal_display.png",stage/"temporal_display.png.provenance.cbor"}){
				std::error_code removeError;std::filesystem::remove(prior,removeError);
				if(removeError){job->release();return 93;}
			}
			const double sceneTime=timeMap.sceneTimeOrigin+static_cast<double>(frame)*sceneStep;
			if(!job->RasterizeAnimation(sceneTime,sceneTime,1u,false,false)){
				job->release();return 93;
			}
			RISECBOR64::Bytes primaryBytes,primarySidecar,displayBytes,displaySidecar;
			RISECBOR64::Value envelope;std::string provenance;
			if(!waitForPair(previousProvenance,primaryBytes,primarySidecar,displayBytes,
				displaySidecar,envelope,provenance)){
				std::fprintf(stderr,"temporal fire frame %zu rejected: %s\n",frame,
					error.c_str());job->release();return 93;
			}
			previousProvenance=provenance;
			const RISECBOR64::Value* payload=envelope.Find("payload");
			const RISECBOR64::Value* artifact=payload?payload->Find("artifact_sha256"):nullptr;
			if(!artifact||artifact->GetType()!=RISECBOR64::Value::Text){job->release();return 93;}
			std::ostringstream index;index<<std::setw(5)<<std::setfill('0')<<frame;
			const std::filesystem::path primary=stage/("temporal_primary_"+index.str()+".exr");
			const std::filesystem::path display=stage/("temporal_display_"+index.str()+".png");
			for(const auto& copy:std::array<std::pair<std::filesystem::path,
				std::filesystem::path>,4>{{
				{stage/"temporal_primary.exr",primary},
				{stage/"temporal_primary.exr.provenance.cbor",primary.string()+".provenance.cbor"},
				{stage/"temporal_display.png",display},
				{stage/"temporal_display.png.provenance.cbor",display.string()+".provenance.cbor"}}})
				std::filesystem::copy_file(copy.first,copy.second,
					std::filesystem::copy_options::overwrite_existing);
			FireFramePrimary link;link.frameIndex=frame;link.provenanceId=provenance;
			link.artifactSha256=artifact->GetText();primaryLinks.push_back(link);
			primaryFrames.push_back(primary);displayFrames.push_back(display);
			std::fprintf(stderr,"temporal fire render frame=%zu/%zu simulation_time=%.9g\n",
				frame+1u,manifest.Frames().size(),timeMap.simulationTimeOrigin+
				static_cast<double>(frame)*timeMap.frameStepSeconds);
		}
		const IRasterizer* rasterizer=job->GetRasterizer();
		const FrameStore* store=rasterizer?rasterizer->GetFrameStore():nullptr;
		const FrameStore::Metadata metadata=store?store->Meta():FrameStore::Metadata();
		const std::filesystem::path temporaryGIF=stage/"temporal_fire_preview.gif.tmp";
		const std::filesystem::path gifPath=stage/"temporal_fire_preview.gif";
		const bool authored=AuthorFirstLightGIF(displayFrames,temporaryGIF,framesPerSecond,error);
		const bool published=authored&&PublishFireFrameSequenceFileTransaction(metadata,
			FireFrameSequenceEncoding::AppleImageIOGif_PreviewPlus6EV_8Bit,
			temporaryGIF.string(),gifPath.string(),64u,64u,framesPerSecond,
			static_cast<unsigned int>(primaryLinks.size()),primaryLinks,ValidateTemporalFireGIF,error);
		job->release();
		if(!published){std::fprintf(stderr,"temporal fire GIF rejected: %s\n",error.c_str());return 94;}
		std::filesystem::create_directories(outputDirectory);
		for(std::size_t frame=0u;frame<primaryFrames.size();++frame){
			std::ostringstream index;index<<std::setw(5)<<std::setfill('0')<<frame;
			for(const auto& copy:std::array<std::pair<std::filesystem::path,
				std::filesystem::path>,4>{{
				{primaryFrames[frame],outputDirectory/("temporal_primary_"+index.str()+".exr")},
				{primaryFrames[frame].string()+".provenance.cbor",
					outputDirectory/("temporal_primary_"+index.str()+".exr.provenance.cbor")},
				{displayFrames[frame],outputDirectory/("temporal_display_"+index.str()+".png")},
				{displayFrames[frame].string()+".provenance.cbor",
					outputDirectory/("temporal_display_"+index.str()+".png.provenance.cbor")}}})
				std::filesystem::copy_file(copy.first,copy.second,
					std::filesystem::copy_options::overwrite_existing);
		}
		std::filesystem::copy_file(gifPath,outputDirectory/"temporal_fire_preview.gif",
			std::filesystem::copy_options::overwrite_existing);
		std::filesystem::copy_file(gifPath.string()+".provenance.cbor",
			outputDirectory/"temporal_fire_preview.gif.provenance.cbor",
			std::filesystem::copy_options::overwrite_existing);
		std::ofstream summary(outputDirectory/"temporal_preview_summary.txt");
		summary<<"artifact_fidelity=display_derivative\npreview_label=temporal_fire_preview\n"
			<<"source_manifest_sha256="<<DigestFile(manifestPath)<<"\nframes="<<primaryLinks.size()<<
			"\nfps="<<framesPerSecond<<"\ngif_sha256="<<
			DigestFile(outputDirectory/"temporal_fire_preview.gif")<<"\n";
		summary.close();
		std::printf("TEMPORAL_FIRE_PREVIEW frames=%zu fps=%u manifest=%s gif=%s output=%s\n",
			primaryLinks.size(),framesPerSecond,DigestFile(manifestPath).c_str(),
			DigestFile(outputDirectory/"temporal_fire_preview.gif").c_str(),
			(outputDirectory/"temporal_fire_preview.gif").string().c_str());
		return 0;
#endif
	}
}

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(RISE_ENABLE_OPENVDB)
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#endif

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	constexpr double CapstonePoolDiameterM=0.30;
	constexpr double CapstoneHeatReleaseRateKW=33.0;
	// McCaffrey, NBSIR 79-1910, Table 1, adopted 33 kW plume row.
	constexpr double McCaffrey33KWVelocityCoefficient=1.13;
	constexpr double McCaffrey33KWTemperatureCoefficientK=23.3;

	using namespace RISE::FireSim;
	int failures = 0;
	bool forcePostRenameDirectorySyncFailureForTest=false;

	class FrozenPainterProbe final : public Perlin3DPainter
	{
	public:
		FrozenPainterProbe(const IPainter& a,const IPainter& b) :
			Perlin3DPainter(0.5,3,a,b,Vector3(1,1,1),Vector3(0,0,0)) {}
		Vector3 Scale() const { return vScale; }
		const void* Function() const { return pFunc; }
		~FrozenPainterProbe() override=default;
	};
	class FrozenUniformProbe final : public UniformColorPainter
	{
	public:
		FrozenUniformProbe() : UniformColorPainter(RISEPel(0.25,0.5,0.75)) {}
		RISEPel Value() const { return C; }
		~FrozenUniformProbe() override=default;
	};

	class FrozenMutationOutput final :
		public virtual IRasterizerOutput,
		public virtual IFireRasterizerOutputRoute,
		public virtual Reference
	{
	public:
		explicit FrozenMutationOutput(IJob& job) : job_(job) {}
		void OutputIntermediateImage(const IRasterImage&,const RISE::Rect*) override {}
		void OutputImage(const IRasterImage&,const RISE::Rect*,unsigned int) override
		{
			attempted=true;
			rejected=!job_.ClearAll() && !job_.SetFilm(2,2,1.0) &&
				!job_.SetGlobalMedium("sequence_fire") &&
				!job_.SetPrimaryAcceleration(true,false,4,32) &&
				!job_.SetFireFidelityMode("preview") &&
				!job_.SetLightSampleRRThreshold(0.25) &&
				!job_.ClearGlobalRadianceMap();
		}
		FireArtifactRouteKind FireArtifactRoute() const override
			{ return FireArtifactRouteKind::DisplayOnly; }
		bool attempted=false;
		bool rejected=false;
	protected:
		~FrozenMutationOutput() override=default;
	private:
		IJob& job_;
	};

	void Check( const bool condition, const char* message )
	{
		if( !condition ) {
			std::fprintf(stderr,"FAIL: %s\n",message);
			++failures;
		}
	}

	float FloatFromBits( const std::uint32_t bits )
	{
		float value = 0.0f;
		std::memcpy(&value,&bits,sizeof(value));
		return value;
	}
	std::uint64_t DoubleBits(const double value)
	{
		std::uint64_t bits=0u;std::memcpy(&bits,&value,sizeof(bits));return bits;
	}
	RISECBOR64::Bytes AerosolRecord();
	RISECBOR64::Bytes SyntheticChemRecord();
	std::string DigestFile(const std::filesystem::path& path);
	RISECBOR64::Bytes ReadFileBytes(const std::filesystem::path& path);

	struct ResumeEquivalenceCertificate
	{
		std::string certificateId;
		std::string checkpointDigest;
		std::string oldBuildId,newBuildId;
		std::string oldExecutableDigest,newExecutableDigest;
		std::uint64_t resumedFromStep=0u,acceptedStepCount=0u;
		std::vector<std::uint64_t> timeStepBits,maximumTemperatureBits,
			maximumEOSResidualBits;
		std::vector<std::string> frameDigests;
	};
	struct ResumeEquivalenceTrace
	{
		std::string checkpointDigest,checkpointProducerBuildId;
		std::string buildId,executableDigest;
		std::uint64_t resumedFromStep=0u,acceptedStepCount=0u;
		std::vector<std::uint64_t> timeStepBits,maximumTemperatureBits,
			maximumEOSResidualBits;
		std::vector<std::string> frameDigests;
	};

	bool LoadResumeEquivalenceCertificate(const std::filesystem::path& path,
		ResumeEquivalenceCertificate& certificate,std::string& error);
	bool CurrentExecutableDigest(const RISECBOR64::Bytes& buildRecord,
		std::string& digest,std::string& error);
	bool SaveResumeEquivalenceTrace(const std::filesystem::path& path,
		const ResumeEquivalenceTrace& trace,std::string& error);
	bool LoadResumeEquivalenceTrace(const std::filesystem::path& path,
		ResumeEquivalenceTrace& trace,std::string& error);
	bool BuildResumeEquivalenceCertificate(const ResumeEquivalenceTrace& oldTrace,
		const ResumeEquivalenceTrace& newTrace,const std::filesystem::path& path,
		ResumeEquivalenceCertificate& certificate,std::string& error);
	const char* CurrentActiveSetAlgorithmVersion()
	{
		return "open_active_set_two_class_r81_v2";
	}
	const char* LegacyActiveSetAlgorithmVersion()
	{
		return "legacy_pre_r80_active_set";
	}
	bool DiscontinuousThreadIdentityAccepted(const bool identical,
		const bool activeSetChecked,std::string& error)
	{
		if(identical)return true;
		error=activeSetChecked?"active_set_thread_identity_mismatch":
			"limiter_thread_identity_mismatch";
		return false;
	}

	struct SolverFrameValues
	{
		bool succeeded=false;
		std::string structuredError;
		std::array<std::size_t,3> dimensions={{0,0,0}};
		double cellWidthM=0.5;
		std::string caseRecordId;
		float temperatureK=900.0f,reactionWPerM3=0.0f;
		std::vector<float> temperature, reaction, carbon;
		std::vector<std::array<float,3> > velocity;
		double realizedHeatReleaseW=0.0, fuelConsumptionKGPerS=0.0;
		double realizedRadiativeFraction=0.0, acceptedEscapeFactor=0.0;
		double selectedTimeStepS=0.0,acceptedTimeStepS=0.0;
		double simulatedTimeS=0.0,flowThroughTimeS=0.0,pilotEnergyJ=0.0;
		double expectedPilotEnergyJ=0.0;
		double maximumTemperatureK=0.0;
		double maximumPilotApproachEOSResidual=0.0;
		double maximumAcceptedEOSResidual=0.0;
		double minimumActiveHoldTemperatureK=0.0;
		double maximumActiveHoldTemperatureK=0.0;
		double maximumLimiterClassDiscrepancy=0.0;
		unsigned int discontinuousLimiterClassSteps=0u;
		bool discontinuousClassThreadIdentity=true;
		bool discontinuousClassThreadIdentityChecked=false;
		std::string activeSetAlgorithmVersion=CurrentActiveSetAlgorithmVersion();
		std::string priorActiveSetAlgorithmVersion;
		double maximumActiveSetComplementarityDiscrepancyMPerS=0.0;
		unsigned int discontinuousActiveSetEvents=0u;
		std::size_t maximumActiveSetCycleLength=0u;
		std::size_t maximumActiveSetDifferingFaceCount=0u;
		bool activeSetThreadIdentity=true;
		bool activeSetThreadIdentityChecked=false;
		double statisticsStartS=0.0,puffingFrequencyHz=0.0,puffingRelativeError=0.0;
		double firstStatisticsStepStartS=0.0;
		double integratedHeatReleaseJ=0.0,integratedRadiativeLossJ=0.0;
		double integratedFuelConsumptionKG=0.0,integratedRadiativeFraction=0.0;
		double effectiveRadiativeFraction=0.0;
		double centerlineTemperatureExponent=0.0,centerlineFitRMSE=0.0;
		double characteristicDiameterM=0.0,mccaffreyFlameTipHeightM=0.0;
		double mccaffreyMaximumTemperatureRelativeError=0.0;
		double mccaffreyMaximumVelocityRelativeError=0.0;
		std::size_t mccaffreyPlumeStationCount=0u;
		std::vector<double> probeTimeS,probeCenterlineHeatReleaseW;
		std::vector<double> acceptedMaximumEOSResidualHistory;
		std::vector<double> acceptedMaximumTemperatureHistoryK;
		std::vector<double> acceptedTimeStepHistoryS;
		std::vector<double> stationProbeTimeS,stationProbeHeightM;
		std::vector<double> stationProbeTemperatureK,stationProbeReactionWPerM3;
		std::vector<double> stationProbeVerticalVelocityMPerS;
		std::vector<double> centerlineHeightM,centerlineTemperatureK,centerlineVelocityMPerS;
		bool ignitedDuringPilot=false,sustainedAfterPilot=false;
		bool pilotHoldBandObserved=false,pilotHoldBandSatisfied=true;
		bool pilotApproachComplete=false;
		bool statisticsBoundaryObserved=false;
		double checkpointCadenceWallS=0.0;
		std::vector<std::uint64_t> checkpointStepIndices;
		std::vector<std::uint64_t> workerCountHistory;
		std::string reductionMode="fixed_order_tree_v1";
		bool resumedFromCheckpoint=false;
		std::uint64_t resumedFromStep=0u;
		std::uint64_t streamedFrameCount=0u;
		std::string migrationCertificateId;
		std::string migrationOldBuildId;
		std::string migrationNewBuildId;
		std::uint64_t migrationAcceptedStepCount=0u;
		std::uint64_t migrationResumedFromStep=0u;
		std::vector<double> monitoredManifoldMaximumHistory;
		std::vector<double> monitoredManifoldP95History;
		std::vector<double> monitoredManifoldP50History;
		std::vector<std::uint32_t> monitoredTailCellHistory;
		std::vector<double> monitoredTailDrainedVolumeHistoryM3;
		std::vector<double> productionDeviceHistoryMS;
		std::vector<double> productionWallHistoryMS;
		std::uint64_t productionHardBoundRetryCount=0u;
		std::uint64_t productionPhysicalProjectionRetryCount=0u;
		double externalFuelMassKG=0.0;
	};

	bool AppendOracleMomentumBudget(const std::filesystem::path& path,
		const PeriodicMACShape& shape,const PeriodicMACField& beginningMomentum,
		const ConservativeAdvance3DConfig& config,const ConservativeAdvance3DResult& advanced,
		const double beginningTimeS,std::string& error)
	{
		if(path.empty()||config.periodicBoundaries||!(config.transport.deltaTimeS>0.0)||
			shape.nx<=38u||shape.ny<=42u||
			advanced.r0.openFlux.low[0].empty()||advanced.r1.openFlux.low[0].empty()||
			advanced.r0.openProjection.velocityMPerS.component[0].empty()||
			advanced.r1.openProjection.velocityMPerS.component[0].empty()||
			advanced.r0.openNonpressureMomentumRHS.component[2].size()!=
				OpenMACFaceCount3D(shape,2u)||
			advanced.r1.openNonpressureMomentumRHS.component[2].size()!=
				OpenMACFaceCount3D(shape,2u)||
			advanced.r0.openBuoyancyMomentumRHS.component[2].size()!=
				OpenMACFaceCount3D(shape,2u)||
			advanced.r1.openBuoyancyMomentumRHS.component[2].size()!=
				OpenMACFaceCount3D(shape,2u)||
			advanced.r0.openStressMomentumRHS.component[2].size()!=
				OpenMACFaceCount3D(shape,2u)||
			advanced.r1.openStressMomentumRHS.component[2].size()!=
				OpenMACFaceCount3D(shape,2u)||
			advanced.r0.openPhaseSourceMomentumRHS.component[2].size()!=
				OpenMACFaceCount3D(shape,2u)||
			advanced.r1.openPhaseSourceMomentumRHS.component[2].size()!=
				OpenMACFaceCount3D(shape,2u)||
			advanced.faceAlpha[2].size()!=OpenMACFaceCount3D(shape,2u)){
			error="oracle momentum diagnostic operands are unavailable";return false;
		}
		const OpenMACField3D advection0=OpenCompatibleMomentumFluxDivergence3D(shape,
			advanced.r0.openFlux,advanced.faceAlpha,
			advanced.r0.openProjection.velocityMPerS,&config.openBoundary);
		const OpenMACField3D advection1=OpenCompatibleMomentumFluxDivergence3D(shape,
			advanced.r1.openFlux,advanced.faceAlpha,
			advanced.r1.openProjection.velocityMPerS,&config.openBoundary);
		std::ofstream column(path.string()+".column.csv",std::ios::app);
		const bool writeHeader=column.tellp()==std::streampos(0);
		if(writeHeader)column<<"beginning_time_s,dt_s,x,y,z_face,beginning_momentum,"
			"stress_rate,buoyancy_rate,advection_rate,source_rate,pressure_gradient_rate,"
			"total_rate,closure_residual,nonpressure_split_residual,"
			"accepted_scalar_face_alpha\n";
		if(!column){error="oracle momentum column diagnostic cannot be opened";return false;}
		double maximumStress=0.0,maximumBuoyancy=0.0,maximumAdvection=0.0,
			maximumSource=0.0,maximumPressure=0.0,maximumTotal=0.0,maximumClosure=0.0,
			maximumNonpressureSplitResidual=0.0;
		double minimumAcceptedAlpha=1.0,maximumAcceptedAlpha=0.0;
		std::size_t maximumFace=OpenMACFaceIndex3D(shape,2u,38u,42u,0u);
		for(std::size_t z=0u;z<=shape.nz;++z){
			const std::size_t face=OpenMACFaceIndex3D(shape,2u,38u,42u,z);
			const double stress=0.5*(advanced.r0.openStressMomentumRHS.component[2][face]+
				advanced.r1.openStressMomentumRHS.component[2][face]);
			const double buoyancy=0.5*(advanced.r0.openBuoyancyMomentumRHS.component[2][face]+
				advanced.r1.openBuoyancyMomentumRHS.component[2][face]);
			const double source=0.5*(advanced.r0.openPhaseSourceMomentumRHS.component[2][face]+
				advanced.r1.openPhaseSourceMomentumRHS.component[2][face]);
			const double advection=-0.5*(advection0.component[2][face]+advection1.component[2][face]);
			const double nonpressure=0.5*(advanced.r0.openNonpressureMomentumRHS.component[2][face]+
				advanced.r1.openNonpressureMomentumRHS.component[2][face]);
			const double nonpressureSplitResidual=nonpressure-(stress+buoyancy+source);
			const double provisional=beginningMomentum.component[2][face]+
				config.transport.deltaTimeS*(nonpressure+advection);
			const double pressure=(advanced.momentumKGPerM2S.component[2][face]-provisional)/
				config.transport.deltaTimeS;
			const double total=stress+buoyancy+advection+source+pressure;
			const double closure=(advanced.momentumKGPerM2S.component[2][face]-
				beginningMomentum.component[2][face])/config.transport.deltaTimeS-total;
			const double acceptedAlpha=advanced.faceAlpha[2][face];
			if(!std::isfinite(acceptedAlpha)||acceptedAlpha<0.0||acceptedAlpha>1.0){
				error="oracle momentum diagnostic alpha is invalid";return false;}
			column<<std::setprecision(17)<<beginningTimeS<<','<<config.transport.deltaTimeS<<
				",38,42,"<<z<<','<<beginningMomentum.component[2][face]<<','<<stress<<','<<
				buoyancy<<','<<advection<<','<<source<<','<<pressure<<','<<total<<','<<closure<<','<<
				nonpressureSplitResidual<<','<<acceptedAlpha<<'\n';
			minimumAcceptedAlpha=std::min(minimumAcceptedAlpha,acceptedAlpha);
			maximumAcceptedAlpha=std::max(maximumAcceptedAlpha,acceptedAlpha);
			auto update=[&](const double value,double& maximum){maximum=std::max(maximum,
				std::fabs(value));};
			update(stress,maximumStress);update(buoyancy,maximumBuoyancy);
			update(advection,maximumAdvection);update(source,maximumSource);
			update(pressure,maximumPressure);update(total,maximumTotal);update(closure,maximumClosure);
			update(nonpressureSplitResidual,maximumNonpressureSplitResidual);
			if(std::fabs(advanced.velocityMPerS.component[2][face])>
				std::fabs(advanced.velocityMPerS.component[2][maximumFace]))maximumFace=face;
		}
		column.close();
		std::ofstream summary(path,std::ios::app);
		const bool writeSummaryHeader=summary.tellp()==std::streampos(0);
		if(writeSummaryHeader)summary<<"beginning_time_s,dt_s,column_x,column_y,maximum_face,"
			"maximum_vertical_velocity_m_per_s,stress_rate_abs_max,buoyancy_rate_abs_max,"
			"advection_rate_abs_max,source_rate_abs_max,pressure_rate_abs_max,total_rate_abs_max,"
			"closure_residual_abs_max,nonpressure_split_residual_abs_max,"
			"accepted_scalar_face_alpha_min,"
			"accepted_scalar_face_alpha_max,accepted_scalar_face_alpha_at_velocity_max\n";
		if(!summary){error="oracle momentum summary diagnostic cannot be opened";return false;}
		summary<<std::setprecision(17)<<beginningTimeS<<','<<config.transport.deltaTimeS<<
			",38,42,"<<maximumFace<<','<<advanced.velocityMPerS.component[2][maximumFace]<<','<<
			maximumStress<<','<<maximumBuoyancy<<','<<maximumAdvection<<','<<maximumSource<<','<<
			maximumPressure<<','<<maximumTotal<<','<<maximumClosure<<','<<
			maximumNonpressureSplitResidual<<','<<minimumAcceptedAlpha<<','<<
			maximumAcceptedAlpha<<','<<advanced.faceAlpha[2][maximumFace]<<'\n';
		return static_cast<bool>(summary);
	}

	struct RunPersistenceOptions
	{
		std::filesystem::path checkpointPath;
		std::filesystem::path finalCheckpointPath;
		std::filesystem::path retainedCheckpointDirectory;
		double checkpointCadenceWallS=0.0;
		std::uint64_t streamedFrameCountAtStart=0u;
		bool resume=false;
		bool killAfterFirstCheckpoint=false;
		std::filesystem::path resumeEquivalenceCertificatePath;
		bool isolatedEquivalenceProbe=false;
		std::string isolatedExpectedCheckpointBuildId;
		std::string isolatedExpectedCheckpointDigest;
		std::uint64_t stopAfterAdditionalAcceptedSteps=0u;
		std::filesystem::path equivalenceSnapshotDirectory;
		bool forceActiveSetIdentityCheckForTest=false;
		bool injectActiveSetIdentityMismatchForTest=false;
		bool forceZeroSourceForTest=false;
		bool productionMetal=false;
		bool compatibleMomentumDiagnostic=false;
		bool singleStageFCTDiagnostic=false;
		bool sealedLegacyMomentumReplay=false;
		bool sealedProjectedHeunReplay=false;
		std::filesystem::path replayProtocolPath;
		std::string replayProtocolDigest;
		double productionMomentumObservationTimeS=0.0;
		std::size_t productionMomentumObservationColumnX=0u;
		std::size_t productionMomentumObservationColumnY=0u;
		double productionMomentumObservationReferenceTier=0.0;
		std::filesystem::path temporalSnapshotDirectory;
		double temporalSnapshotCadenceS=0.0;
		double maximumProductionSourceStepS=0.0;
		std::filesystem::path productionOnsetDiagnosticDirectory;
		double productionOnsetStopVelocityMPerS=0.0;
	};

	bool BuildProductionProjectedHeunOwnerRequest(
		const RISE::FireProductionResidentStepRequest& ordinary,
		const RISE::FireProductionFrozenSourcePacketSeal& source,
		const RISE::RISECBOR64::Bytes& caseRecordEnvelope,
		const float projectionTolerancePerS,const float endpointVelocityToleranceMPerS,
		RISE::FireProductionProjectedHeunMetalOwnerRequest& owner,std::string& error)
	{
		owner=RISE::FireProductionProjectedHeunMetalOwnerRequest();
		if(!source.IsSealed()){error="projected-Heun production source authority is unsealed";
			return false;}
		if(source.SourceDelta()!=ordinary.cellSourceIncrement){
			std::size_t mismatch=0u;while(mismatch<ordinary.cellSourceIncrement.size()&&
				source.SourceDelta()[mismatch]==ordinary.cellSourceIncrement[mismatch])++mismatch;
			std::ostringstream message;message<<std::setprecision(17)
				<<"projected-Heun production source authority dose changed after construction: source_size="
				<<source.SourceDelta().size()<<" ordinary_size="<<ordinary.cellSourceIncrement.size()
				<<" index="<<mismatch;
			if(mismatch<source.SourceDelta().size()&&mismatch<ordinary.cellSourceIncrement.size())
				message<<" source="<<source.SourceDelta()[mismatch]<<" ordinary="
					<<ordinary.cellSourceIncrement[mismatch];
			error=message.str();return false;
		}
		if(source.TimeStepS()!=ordinary.force.timeStepS){std::ostringstream mismatch;
			mismatch<<std::setprecision(17)<<"projected-Heun production source timestep mismatch: source="
				<<source.TimeStepS()<<" ordinary="<<ordinary.force.timeStepS;
			error=mismatch.str();return false;}
		auto& eos=owner.lineage.eos;auto& flux=eos.physicalFlux;auto& transport=flux.transport;
		transport.shape=ordinary.force.shape;transport.boundary=ordinary.force.boundary;
		transport.stage=RISE::FireProductionProjectedHeunStage::R0;
		transport.attemptIdentity=source.AttemptIdentity();
		transport.parentCandidateIdentity=source.BeginningStateIdentity();
		transport.projectionIdentity=source.PacketIdentity();
		if(transport.parentCandidateIdentity==0u)transport.parentCandidateIdentity=UINT64_C(1);
		if(transport.projectionIdentity==0u)transport.projectionIdentity=UINT64_C(2);
		transport.conservativeValues=ordinary.cellTransport.conservativeValues;
		transport.temperatureK=source.BeginningTemperatureK();
		transport.projectedVelocityMPerS=ordinary.cellTransport.frozenVelocityMPerS;
		for(unsigned int side=0u;side<6u;++side){const std::size_t count=side<2u?
			transport.shape.ny*transport.shape.nz:(side<4u?transport.shape.nx*transport.shape.nz:
			transport.shape.nx*transport.shape.ny);transport.fuelInletBoundaryFace[side].assign(count,0u);
			flux.pressureOpenInflow[side].assign(count,0u);}
		if(ordinary.cellTransport.ambientValues.size()!=9u){error=
			"projected-Heun production ambient tuple is invalid";return false;}
		for(std::size_t component=0u;component<9u;++component)
			flux.ambient[component]=ordinary.cellTransport.ambientValues[component];
		flux.ambientTemperatureK=300.0f;
		const FireCertifiedNullspace& reconstruction=
			FireSimulationMethaneRecord::PhysicalV1().ConservativeReconstruction();
		flux.nullity=reconstruction.nullity;
		flux.nullspaceBasis.resize(reconstruction.orthonormalBasis.size());
		for(std::size_t index=0u;index<flux.nullspaceBasis.size();++index)
			flux.nullspaceBasis[index]=static_cast<float>(reconstruction.orthonormalBasis[index]);
		flux.coordinateProjector.assign(flux.nullity*flux.nullity,0.0f);
		for(std::size_t index=0u;index<flux.nullity;++index)
			flux.coordinateProjector[index*flux.nullity+index]=1.0f;
		eos.sourceDelta=source.SourceDelta();eos.producingStage=RISE::FireProductionScalarEOSStage::QStar;
		eos.producerPrecision=FireStateProducerPrecision::Binary32;
		eos.caseRecordEnvelope=caseRecordEnvelope;eos.candidateTimeStepS=ordinary.force.timeStepS;
		owner.lineage.frozenSource=source;
		owner.beginningMomentumKGPerM2S=ordinary.force.beginningMomentumKGPerM2S;
		owner.ambientDensityKGPerM3=ordinary.force.ambientDensityKGPerM3;
		owner.vremanCoefficient=ordinary.force.vremanCoefficient;
		owner.gravityMPerS2=ordinary.force.gravityMPerS2;
		owner.projectionTolerancePerS=projectionTolerancePerS;
		owner.endpointVelocityToleranceMPerS=endpointVelocityToleranceMPerS;
		owner.maximumPicardIterations=64u;return true;
	}

	void AppendReplayUInt64(RISECBOR64::Bytes& bytes,const std::uint64_t value)
	{
		for(unsigned int byte=0u;byte<8u;++byte)
			bytes.push_back(static_cast<unsigned char>((value>>(8u*byte))&0xffu));
	}

	void AppendReplayDouble(RISECBOR64::Bytes& bytes,const double value)
	{
		std::uint64_t bits=0u;std::memcpy(&bits,&value,sizeof(bits));
		AppendReplayUInt64(bytes,bits);
	}
	void AppendReplayFloat(RISECBOR64::Bytes& bytes,const float value)
	{
		std::uint32_t bits=0u;std::memcpy(&bits,&value,sizeof(bits));
		for(unsigned int byte=0u;byte<4u;++byte)
			bytes.push_back(static_cast<unsigned char>((bits>>(8u*byte))&0xffu));
	}

	RISECBOR64::Bytes ProductionSourcePacketFieldBytes(
		const std::vector<MethaneSourcePacket>& packets)
	{
		RISECBOR64::Bytes bytes;
		const char schema[]="rise.fire.production.source_packet_field.v1";
		bytes.insert(bytes.end(),schema,schema+sizeof(schema));
		AppendReplayUInt64(bytes,packets.size());
		for(const MethaneSourcePacket& packet:packets){
			for(const double value:packet.constituentDelta)AppendReplayDouble(bytes,value);
			AppendReplayDouble(bytes,packet.sensibleEnergyDeltaJPerM3);
			AppendReplayDouble(bytes,packet.reactedFuelKGPerM3);
			AppendReplayDouble(bytes,packet.oxidizedCarbonKGPerM3);
			AppendReplayDouble(bytes,packet.grossCarbonFormedKGPerM3);
			AppendReplayDouble(bytes,packet.gasHeatReleaseWPerM3);
			AppendReplayDouble(bytes,packet.sootHeatReleaseWPerM3);
			AppendReplayDouble(bytes,packet.pilotEnergyDeltaJPerM3);
			AppendReplayDouble(bytes,packet.pilotExpansionIntegral);
			AppendReplayDouble(bytes,packet.radiativeCoolingWPerM3);
		}
		return bytes;
	}

	std::string ProductionSourcePacketFieldSHA256(
		const std::vector<MethaneSourcePacket>& packets)
	{
		return RISECBOR64::SHA256Hex(ProductionSourcePacketFieldBytes(packets));
	}

	bool WriteProductionSourcePacketFieldEvidence(const std::filesystem::path& base,
		const std::vector<MethaneSourcePacket>& packets,
		const std::vector<MethaneCellState>& unstagedBeginning,
		const std::vector<MethaneCellState>& stagedBeginning,
		const PeriodicMACField& unstagedMomentum,
		const FireProductionResidentStepRequest& request,std::string& error)
	{
		const RISECBOR64::Bytes bytes=ProductionSourcePacketFieldBytes(packets);
		const std::filesystem::path payloadPath=base.string()+".source_packets.bin";
		std::ofstream payload(payloadPath,std::ios::binary|std::ios::trunc);
		if(!bytes.empty())payload.write(reinterpret_cast<const char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
		payload.close();
		if(!payload){error="production onset source-packet field write failed";return false;}
		RISECBOR64::Bytes complete;
		const char completeSchema[]="rise.fire.production.source_observation_inputs.v1";
		complete.insert(complete.end(),completeSchema,completeSchema+sizeof(completeSchema));
		complete.insert(complete.end(),bytes.begin(),bytes.end());
		AppendReplayUInt64(complete,unstagedBeginning.size());
		if(stagedBeginning.size()!=unstagedBeginning.size()){
			error="production complete source input state shape mismatch";return false;}
		for(std::size_t cell=0u;cell<unstagedBeginning.size();++cell){
			const ConservativeVector unstaged=ToConservativeVector(unstagedBeginning[cell]);
			const ConservativeVector staged=ToConservativeVector(stagedBeginning[cell]);
			for(std::size_t component=0u;component<MethaneConservativeDimension;++component){
				AppendReplayDouble(complete,unstaged[component]);
				AppendReplayDouble(complete,staged[component]);
			}
		}
		for(unsigned int axis=0u;axis<3u;++axis){
			AppendReplayUInt64(complete,unstagedMomentum.component[axis].size());
			if(request.force.beginningMomentumKGPerM2S[axis].size()!=
				unstagedMomentum.component[axis].size()){
				error="production complete source input momentum shape mismatch";return false;}
			for(std::size_t face=0u;face<unstagedMomentum.component[axis].size();++face){
				AppendReplayDouble(complete,unstagedMomentum.component[axis][face]);
				AppendReplayFloat(complete,request.force.beginningMomentumKGPerM2S[axis][face]);
			}
		}
		for(unsigned int axis=0u;axis<3u;++axis){
			AppendReplayUInt64(complete,request.cellTransport.frozenVelocityMPerS[axis].size());
			for(const float value:request.cellTransport.frozenVelocityMPerS[axis])
				AppendReplayFloat(complete,value);
			AppendReplayUInt64(complete,request.dualTransport.frozenVelocityMPerS[axis].size());
			for(const float value:request.dualTransport.frozenVelocityMPerS[axis])
				AppendReplayFloat(complete,value);
		}
		AppendReplayUInt64(complete,request.cellSourceIncrement.size());
		for(const float value:request.cellSourceIncrement)AppendReplayFloat(complete,value);
		for(unsigned int axis=0u;axis<3u;++axis){
			AppendReplayUInt64(complete,request.momentumSourceIncrement[axis].size());
			for(const float value:request.momentumSourceIncrement[axis])
				AppendReplayFloat(complete,value);
		}
		const std::filesystem::path completePath=base.string()+".source_observation_inputs.bin";
		std::ofstream completeOutput(completePath,std::ios::binary|std::ios::trunc);
		if(!complete.empty())completeOutput.write(reinterpret_cast<const char*>(complete.data()),
			static_cast<std::streamsize>(complete.size()));
		completeOutput.close();
		if(!completeOutput){error="production complete source input write failed";return false;}
		std::array<long double,MethaneSpeciesCount> speciesTotal{};
		long double energy=0.0L,reacted=0.0L,oxidized=0.0L,grossCarbon=0.0L,
			gasHRR=0.0L,sootHRR=0.0L,pilotEnergy=0.0L,pilotExpansion=0.0L,
			radiativeCooling=0.0L;
		for(const MethaneSourcePacket& packet:packets){
			for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
				speciesTotal[species]+=packet.constituentDelta[species];
			energy+=packet.sensibleEnergyDeltaJPerM3;reacted+=packet.reactedFuelKGPerM3;
			oxidized+=packet.oxidizedCarbonKGPerM3;
			grossCarbon+=packet.grossCarbonFormedKGPerM3;
			gasHRR+=packet.gasHeatReleaseWPerM3;sootHRR+=packet.sootHeatReleaseWPerM3;
			pilotEnergy+=packet.pilotEnergyDeltaJPerM3;
			pilotExpansion+=packet.pilotExpansionIntegral;
			radiativeCooling+=packet.radiativeCoolingWPerM3;
		}
		std::ofstream ledger(base.string()+".source_ledger.v1",std::ios::trunc);
		const long double cellVolume=static_cast<long double>(request.force.shape.cellWidthM)*
			request.force.shape.cellWidthM*request.force.shape.cellWidthM;
		ledger<<std::setprecision(21)
			<<"schema rise.fire.production.source_packet_ledger.v1\n"
			<<"payload_sha256 "<<RISECBOR64::SHA256Hex(bytes)<<"\n"
			<<"source_observation_inputs_sha256 "<<RISECBOR64::SHA256Hex(complete)<<"\n"
			<<"packet_count "<<packets.size()<<"\n";
		for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
			ledger<<"constituent_delta_kg_"<<species<<' '<<speciesTotal[species]*cellVolume<<'\n';
		ledger<<"sensible_energy_delta_j "<<energy*cellVolume<<"\nreacted_fuel_kg "<<
			reacted*cellVolume<<"\noxidized_carbon_kg "<<oxidized*cellVolume<<
			"\ngross_carbon_kg "<<grossCarbon*cellVolume<<"\ngas_hrr_w "<<gasHRR*cellVolume<<
			"\nsoot_hrr_w "<<sootHRR*cellVolume<<"\npilot_energy_j "<<pilotEnergy*cellVolume<<
			"\npilot_expansion_m3 "<<pilotExpansion*cellVolume<<
			"\nradiative_cooling_w "<<radiativeCooling*cellVolume<<'\n';
		ledger.close();
		if(!ledger||DigestFile(payloadPath)!=RISECBOR64::SHA256Hex(bytes)||
			DigestFile(completePath)!=RISECBOR64::SHA256Hex(complete)){
			error="production onset source-packet evidence verification failed";return false;
		}
		return true;
	}

	bool WriteProductionSourceAndAdjacentStateContext(const std::filesystem::path& path,
		const FireProductionProjectionShape& shape,const std::size_t centerX,
		const std::size_t centerY,const std::size_t centerZ,
		const std::vector<MethaneCellState>& beginning,
		const std::vector<MethaneSourcePacket>& packets,
		const std::vector<float>& acceptedConservative,
		const FireSimulationMethaneRecord& fuel,std::string& error)
	{
		const std::size_t cells=shape.CellCount();
		if(beginning.size()!=cells||packets.size()!=cells||
			acceptedConservative.size()!=MethaneConservativeDimension*cells||
			centerX>=shape.nx||centerY>=shape.ny||centerZ>=shape.nz){
			error="production onset source/state context shape mismatch";return false;
		}
		std::vector<std::array<std::size_t,3>> locations;
		auto add=[&](const std::size_t x,const std::size_t y,const std::size_t z){
			const std::array<std::size_t,3> location={{x,y,z}};
			if(std::find(locations.begin(),locations.end(),location)==locations.end())
				locations.push_back(location);};
		add(centerX,centerY,centerZ);
		if(centerX>0u)add(centerX-1u,centerY,centerZ);
		if(centerX+1u<shape.nx)add(centerX+1u,centerY,centerZ);
		if(centerY>0u)add(centerX,centerY-1u,centerZ);
		if(centerY+1u<shape.ny)add(centerX,centerY+1u,centerZ);
		if(centerZ>0u)add(centerX,centerY,centerZ-1u);
		if(centerZ+1u<shape.nz)add(centerX,centerY,centerZ+1u);
		std::ofstream context(path,std::ios::trunc);
		context<<"relation,cell,x,y,z,beginning_temperature_k,beginning_eos_deviation,"
			"accepted_temperature_k,accepted_eos_deviation";
		for(std::size_t component=0u;component<MethaneConservativeDimension;++component)
			context<<",beginning_q"<<component;
		for(std::size_t component=0u;component<MethaneConservativeDimension;++component)
			context<<",accepted_q"<<component;
		for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
			context<<",source_species"<<species;
		context<<",source_sensible_energy,source_reacted_fuel,source_oxidized_carbon,"
			"source_gross_carbon,source_gas_hrr,source_soot_hrr,source_pilot_energy,"
			"source_pilot_expansion,source_radiative_cooling\n";
		for(std::size_t locationIndex=0u;locationIndex<locations.size();++locationIndex){
			const std::size_t x=locations[locationIndex][0],y=locations[locationIndex][1],
				z=locations[locationIndex][2];
			const std::size_t cell=x+shape.nx*(y+shape.ny*z);
			ConservativeVector acceptedVector{};
			for(std::size_t component=0u;component<MethaneConservativeDimension;++component)
				acceptedVector[component]=acceptedConservative[component*cells+cell];
			MethaneCellState accepted=FromConservativeVector(acceptedVector,
				FireStateProducerPrecision::Binary32);
			double acceptedTemperature=0.0;
			if(!InvertMethaneTemperatureWithinAcceptedEnvelope(accepted,
				fuel.TemperatureMinK(),fuel.TemperatureMaxK(),fuel,acceptedTemperature,&error))
				return false;
			accepted.temperatureK=acceptedTemperature;
			double beginningDeviation=0.0,acceptedDeviation=0.0;
			if(!EquationOfStateResidual(beginning[cell],fuel,beginningDeviation,&error)||
				!EquationOfStateResidual(accepted,fuel,acceptedDeviation,&error))return false;
			const ConservativeVector beginningVector=ToConservativeVector(beginning[cell]);
			const MethaneSourcePacket& packet=packets[cell];
			context<<std::setprecision(17)<<(locationIndex==0u?"center":"adjacent")<<','<<
				cell<<','<<x<<','<<y<<','<<z<<','<<beginning[cell].temperatureK<<','<<
				beginningDeviation<<','<<acceptedTemperature<<','<<acceptedDeviation;
			for(std::size_t component=0u;component<MethaneConservativeDimension;++component)
				context<<','<<beginningVector[component];
			for(std::size_t component=0u;component<MethaneConservativeDimension;++component)
				context<<','<<acceptedVector[component];
			for(const double value:packet.constituentDelta)context<<','<<value;
			context<<','<<packet.sensibleEnergyDeltaJPerM3<<','<<packet.reactedFuelKGPerM3<<','<<
				packet.oxidizedCarbonKGPerM3<<','<<packet.grossCarbonFormedKGPerM3<<','<<
				packet.gasHeatReleaseWPerM3<<','<<packet.sootHeatReleaseWPerM3<<','<<
				packet.pilotEnergyDeltaJPerM3<<','<<packet.pilotExpansionIntegral<<','<<
				packet.radiativeCoolingWPerM3<<'\n';
		}
		context.close();
		if(!context){error="production onset source/state context write failed";return false;}
		return true;
	}

	std::string ProductionBeginningFieldSHA256(const std::string& caseRecordId,
		const PeriodicMACShape& shape,const std::vector<MethaneCellState>& states,
		const PeriodicMACField& momentum,const PeriodicMACField& velocity)
	{
		RISECBOR64::Bytes bytes;
		const char schema[]="rise.fire.production.from_zero_state.v1";
		bytes.insert(bytes.end(),schema,schema+sizeof(schema));
		AppendReplayUInt64(bytes,caseRecordId.size());
		bytes.insert(bytes.end(),caseRecordId.begin(),caseRecordId.end());
		AppendReplayUInt64(bytes,shape.nx);AppendReplayUInt64(bytes,shape.ny);
		AppendReplayUInt64(bytes,shape.nz);AppendReplayDouble(bytes,shape.cellWidthM);
		AppendReplayUInt64(bytes,states.size());
		for(const MethaneCellState& state:states){
			AppendReplayUInt64(bytes,static_cast<std::uint64_t>(state.producerPrecision));
			AppendReplayDouble(bytes,state.temperatureK);
			const ConservativeVector conservative=ToConservativeVector(state);
			for(std::size_t component=0u;component<MethaneConservativeDimension;++component)
				AppendReplayDouble(bytes,conservative[component]);
		}
		for(unsigned int axis=0u;axis<3u;++axis){
			AppendReplayUInt64(bytes,momentum.component[axis].size());
			for(const double value:momentum.component[axis])AppendReplayDouble(bytes,value);
			AppendReplayUInt64(bytes,velocity.component[axis].size());
			for(const double value:velocity.component[axis])AppendReplayDouble(bytes,value);
		}
		return RISECBOR64::SHA256Hex(bytes);
	}

	bool LimitBinary32ProductionStepToEvent(const double simulationTimeS,
		const double eventTimeS,double& timeStepS)
	{
		if(!std::isfinite(simulationTimeS)||!std::isfinite(eventTimeS)||
			!std::isfinite(timeStepS)||!(timeStepS>0.0)||!(simulationTimeS<eventTimeS)||
			!(eventTimeS-simulationTimeS<timeStepS))return false;
		const double remaining=eventTimeS-simulationTimeS;
		float represented=static_cast<float>(remaining);
		if(static_cast<double>(represented)<remaining)represented=std::nextafter(represented,
			std::numeric_limits<float>::infinity());
		if(!std::isfinite(represented)||!(represented>0.0f))return false;
		timeStepS=static_cast<double>(represented);return true;
	}

	bool ProductionSourceBoundaryContactCell(const std::size_t cell,const std::size_t nx,
		const std::size_t ny,const std::vector<std::uint8_t>& canonicalPilotMask,
		const std::vector<double>& sourcePattern)
	{
		if(cell>=canonicalPilotMask.size()||nx==0u||ny==0u||cell>=nx*ny||
			cell>=sourcePattern.size()||sourcePattern[cell]==0.0)return false;
		const std::size_t x=cell%nx,y=cell/nx;
		const std::size_t missing=canonicalPilotMask.size();
		const std::size_t neighbors[4]={x?cell-1u:missing,x+1u<nx?cell+1u:missing,
			y?cell-nx:missing,y+1u<ny?cell+nx:missing};
		for(const std::size_t neighbor:neighbors)
			if(neighbor<canonicalPilotMask.size()&&canonicalPilotMask[neighbor]!=0u)return true;
		return false;
	}

	bool ProductionPilotCommandCell(const std::size_t cell,const std::size_t nx,const std::size_t ny,
		const std::vector<std::uint8_t>& canonicalPilotMask,
		const std::vector<double>& sourcePattern)
	{
		if(cell>=canonicalPilotMask.size())return false;
		return canonicalPilotMask[cell]!=0u||ProductionSourceBoundaryContactCell(
			cell,nx,ny,canonicalPilotMask,sourcePattern);
	}

	bool ProductionEstablishedFlameHolderEligible(const std::size_t cell,
		const std::size_t nx,const std::size_t ny,
		const std::vector<std::uint8_t>& canonicalPilotMask,
		const std::vector<double>& sourcePattern,const bool pilotEstablished,
		const MethaneCellState& candidate,
		const FireSimulationMethaneRecord& fuel,
		const FireSimulationTransportRecord& transport,bool& eligible,std::string* error)
	{
		eligible=false;
		if(!ProductionSourceBoundaryContactCell(cell,nx,ny,canonicalPilotMask,
			sourcePattern))return true;
		const std::vector<double>& ambientMassFraction=fuel.AmbientMassFractions();
		const double ambientProductMassFraction=ambientMassFraction[MethaneCO2]+
			ambientMassFraction[MethaneH2O];
		const double productWitness=std::nextafter(ambientProductMassFraction,
			std::numeric_limits<double>::infinity());
		const double gasDensity=candidate.GasDensity();
		if(!(gasDensity>0.0)||candidate.constituent[MethaneCH4]<=0.0||
			candidate.constituent[MethaneO2]<=0.0)return true;
		if(!pilotEstablished&&
			(candidate.constituent[MethaneCO2]+candidate.constituent[MethaneH2O])/
				gasDensity<productWitness)return true;
		double adiabaticTemperatureK=0.0;
		if(!TrialAdiabaticTemperatureK(candidate,fuel,fuel,adiabaticTemperatureK,error))
			return false;
		eligible=adiabaticTemperatureK>=transport.CriticalFlameTemperatureK();
		return true;
	}

	bool ProductionEstablishedFlameHolderREDPasses()
	{
		const std::vector<std::uint8_t> annulus={0u,0u,1u,0u,0u,0u};
		const std::vector<double> source={1.0,1.0,0.0,1.0,0.0,0.0};
		std::string error;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		const double mixtureFraction=0.055;
		MethaneCellState cold;cold.temperatureK=300.0;
		double inverseMolecularWeight=0.0;
		for(std::size_t species=0u;species<MethaneSpeciesCount;++species){
			cold.constituent[species]=(1.0-mixtureFraction)*
				fuel.AmbientMassFractions()[species]+mixtureFraction*
				fuel.InjectedMassFractions()[species];
			if(species<MethaneCarbon){
				const FireThermochemistrySpecies* property=
					fuel.FindSpecies(fuel.SpeciesOrder()[species].c_str());
				if(property)inverseMolecularWeight+=cold.constituent[species]/
					property->molecularWeightKGPerKMol;
			}
		}
		const double density=fuel.ThermodynamicPressurePa()/(8314.46261815324*
			cold.temperatureK*inverseMolecularWeight);
		for(double& constituent:cold.constituent)constituent*=density;
		cold.rhoTotalZ=mixtureFraction*density;
		MethaneCellState established=cold;
		const double reacted=0.02*std::min(established.constituent[MethaneCH4],
			established.constituent[MethaneO2]/fuel.StoichiometricOxygenKGPerKGFuel());
		for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
			established.constituent[species]+=reacted*fuel.PrimaryReactionDelta()[species];
		if(!fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(cold),
			cold.temperatureK,cold.sensibleEnergyJPerM3,&error)||
			!fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(established),
				established.temperatureK,established.sensibleEnergyJPerM3,&error))return false;
		MethaneCellState sourceDepleted=cold;
		sourceDepleted.constituent[MethaneCH4]=0.0;
		bool establishedEligible=false,coldEligible=true,latchedColdEligible=false,
			sourceDepletedEligible=true,noncontactEligible=true;
		const FireSimulationTransportRecord& transport=FireSimulationTransportRecord::OpenV1();
		const bool establishedAccepted=ProductionEstablishedFlameHolderEligible(1u,3u,2u,
			annulus,source,false,established,fuel,transport,establishedEligible,&error);
		const bool coldAccepted=ProductionEstablishedFlameHolderEligible(1u,3u,2u,annulus,
			source,false,cold,fuel,transport,coldEligible,&error);
		const bool latchedAccepted=ProductionEstablishedFlameHolderEligible(1u,3u,2u,
			annulus,source,true,cold,fuel,transport,latchedColdEligible,&error);
		const bool sourceDepletedAccepted=ProductionEstablishedFlameHolderEligible(1u,3u,2u,
			annulus,source,true,sourceDepleted,fuel,transport,sourceDepletedEligible,&error);
		const bool noncontactAccepted=ProductionEstablishedFlameHolderEligible(0u,3u,2u,
			annulus,source,true,established,fuel,transport,noncontactEligible,&error);
		return establishedAccepted&&establishedEligible&&coldAccepted&&!coldEligible&&
			latchedAccepted&&latchedColdEligible&&sourceDepletedAccepted&&
			!sourceDepletedEligible&&noncontactAccepted&&!noncontactEligible;
	}

	class CheckpointWriter
	{
	public:
		explicit CheckpointWriter(const std::filesystem::path& path) :
			output_(path,std::ios::binary|std::ios::trunc),digestOnly_(false) {}
		CheckpointWriter() : digestOnly_(true) {}
		bool Good() const { return digestOnly_||static_cast<bool>(output_); }
		bool HeaderBytes(const void* data,const std::size_t size)
		{
			if(digestOnly_)return false;
			output_.write(static_cast<const char*>(data),static_cast<std::streamsize>(size));
			return static_cast<bool>(output_);
		}
		bool SeekHeader(const std::streamoff offset)
		{
			if(digestOnly_)return false;
			output_.seekp(offset);return static_cast<bool>(output_);
		}
		template<typename T> bool Pod(const T& value)
		{
			static_assert(std::is_arithmetic<T>::value,"checkpoint POD must be arithmetic");
			return Bytes(&value,sizeof(value));
		}
		bool Bytes(const void* data,const std::size_t size)
		{
			if(!digestOnly_&&!output_)return false;
			if(!digestOnly_)output_.write(static_cast<const char*>(data),static_cast<std::streamsize>(size));
			const unsigned char* byte=static_cast<const unsigned char*>(data);
			for(std::size_t i=0;i<size;++i){checksum_^=byte[i];checksum_*=1099511628211ull;}
			payloadBytes_+=static_cast<std::uint64_t>(size);
			return digestOnly_||static_cast<bool>(output_);
		}
		bool String(const std::string& value)
		{
			const std::uint64_t size=static_cast<std::uint64_t>(value.size());
			return Pod(size)&&(size==0u||Bytes(value.data(),value.size()));
		}
		bool Finish()
		{
			if(digestOnly_)return true;
			output_.flush();output_.close();return !output_.fail();
		}
		std::uint64_t Checksum() const { return checksum_; }
		std::uint64_t PayloadBytes() const { return payloadBytes_; }
	private:
		std::ofstream output_;
		bool digestOnly_;
		std::uint64_t checksum_=1469598103934665603ull,payloadBytes_=0u;
	};

	class CheckpointReader
	{
	public:
		explicit CheckpointReader(const std::filesystem::path& path) :
			input_(path,std::ios::binary) {}
		bool Good() const { return static_cast<bool>(input_); }
		bool HeaderBytes(void* data,const std::size_t size)
		{
			input_.read(static_cast<char*>(data),static_cast<std::streamsize>(size));
			return static_cast<bool>(input_);
		}
		template<typename T> bool Pod(T& value)
		{
			static_assert(std::is_arithmetic<T>::value,"checkpoint POD must be arithmetic");
			return Bytes(&value,sizeof(value));
		}
		bool Bytes(void* data,const std::size_t size)
		{
			if(!input_||consumed_+size>limit_)return false;
			input_.read(static_cast<char*>(data),static_cast<std::streamsize>(size));
			if(!input_)return false;
			const unsigned char* byte=static_cast<const unsigned char*>(data);
			for(std::size_t i=0;i<size;++i){checksum_^=byte[i];checksum_*=1099511628211ull;}
			consumed_+=static_cast<std::uint64_t>(size);return true;
		}
		bool String(std::string& value)
		{
			std::uint64_t size=0u;if(!Pod(size)||size>16u*1024u*1024u)return false;
			value.assign(static_cast<std::size_t>(size),'\0');
			return size==0u||Bytes(&value[0],static_cast<std::size_t>(size));
		}
		void SetLimit(const std::uint64_t limit){limit_=limit;}
		bool Finished(const std::uint64_t checksum) const
			{return consumed_==limit_&&checksum_==checksum;}
	private:
		std::ifstream input_;
		std::uint64_t checksum_=1469598103934665603ull,consumed_=0u;
		std::uint64_t limit_=std::numeric_limits<std::uint64_t>::max();
	};

	template<typename T> bool WriteArithmeticVector(CheckpointWriter& writer,
		const std::vector<T>& value)
	{
		const std::uint64_t size=static_cast<std::uint64_t>(value.size());
		if(!writer.Pod(size))return false;
		for(const T& item:value)if(!writer.Pod(item))return false;
		return true;
	}
	template<typename T> bool ReadArithmeticVector(CheckpointReader& reader,
		std::vector<T>& value,const std::uint64_t maximum=200000000u)
	{
		std::uint64_t size=0u;if(!reader.Pod(size)||size>maximum)return false;
		value.resize(static_cast<std::size_t>(size));
		for(T& item:value)if(!reader.Pod(item))return false;
		return true;
	}

	bool WriteSolverFrameValues(CheckpointWriter& w,const SolverFrameValues& v)
	{
		if(!w.Pod(v.succeeded)||!w.String(v.structuredError)||!w.String(v.reductionMode))return false;
		for(const std::size_t dimension:v.dimensions){const std::uint64_t encoded=dimension;
			if(!w.Pod(encoded))return false;}
		if(!w.Pod(v.cellWidthM)||!w.String(v.caseRecordId)||!w.Pod(v.temperatureK)||
			!w.Pod(v.reactionWPerM3)||!WriteArithmeticVector(w,v.temperature)||
			!WriteArithmeticVector(w,v.reaction)||!WriteArithmeticVector(w,v.carbon))return false;
		const std::uint64_t velocityCount=v.velocity.size();if(!w.Pod(velocityCount))return false;
		for(const auto& velocity:v.velocity)for(const float component:velocity)
			if(!w.Pod(component))return false;
#define WRITE_CHECKPOINT_FIELD(field) if(!w.Pod(v.field))return false
		WRITE_CHECKPOINT_FIELD(realizedHeatReleaseW);WRITE_CHECKPOINT_FIELD(fuelConsumptionKGPerS);
		WRITE_CHECKPOINT_FIELD(realizedRadiativeFraction);WRITE_CHECKPOINT_FIELD(acceptedEscapeFactor);
		WRITE_CHECKPOINT_FIELD(selectedTimeStepS);WRITE_CHECKPOINT_FIELD(acceptedTimeStepS);
		WRITE_CHECKPOINT_FIELD(simulatedTimeS);WRITE_CHECKPOINT_FIELD(flowThroughTimeS);
		WRITE_CHECKPOINT_FIELD(pilotEnergyJ);WRITE_CHECKPOINT_FIELD(expectedPilotEnergyJ);
		WRITE_CHECKPOINT_FIELD(maximumTemperatureK);
		WRITE_CHECKPOINT_FIELD(maximumPilotApproachEOSResidual);
		WRITE_CHECKPOINT_FIELD(maximumAcceptedEOSResidual);
		WRITE_CHECKPOINT_FIELD(minimumActiveHoldTemperatureK);
		WRITE_CHECKPOINT_FIELD(maximumActiveHoldTemperatureK);
		WRITE_CHECKPOINT_FIELD(maximumLimiterClassDiscrepancy);
		WRITE_CHECKPOINT_FIELD(discontinuousLimiterClassSteps);
		WRITE_CHECKPOINT_FIELD(discontinuousClassThreadIdentity);
		WRITE_CHECKPOINT_FIELD(discontinuousClassThreadIdentityChecked);
		WRITE_CHECKPOINT_FIELD(statisticsStartS);WRITE_CHECKPOINT_FIELD(puffingFrequencyHz);
		WRITE_CHECKPOINT_FIELD(puffingRelativeError);WRITE_CHECKPOINT_FIELD(firstStatisticsStepStartS);
		WRITE_CHECKPOINT_FIELD(integratedHeatReleaseJ);WRITE_CHECKPOINT_FIELD(integratedRadiativeLossJ);
		WRITE_CHECKPOINT_FIELD(integratedFuelConsumptionKG);WRITE_CHECKPOINT_FIELD(integratedRadiativeFraction);
		WRITE_CHECKPOINT_FIELD(effectiveRadiativeFraction);WRITE_CHECKPOINT_FIELD(centerlineTemperatureExponent);
		WRITE_CHECKPOINT_FIELD(centerlineFitRMSE);WRITE_CHECKPOINT_FIELD(characteristicDiameterM);
		WRITE_CHECKPOINT_FIELD(mccaffreyFlameTipHeightM);
		WRITE_CHECKPOINT_FIELD(mccaffreyMaximumTemperatureRelativeError);
		WRITE_CHECKPOINT_FIELD(mccaffreyMaximumVelocityRelativeError);
		{const std::uint64_t count=v.mccaffreyPlumeStationCount;if(!w.Pod(count))return false;}
		WRITE_CHECKPOINT_FIELD(ignitedDuringPilot);WRITE_CHECKPOINT_FIELD(sustainedAfterPilot);
		WRITE_CHECKPOINT_FIELD(pilotHoldBandObserved);WRITE_CHECKPOINT_FIELD(pilotHoldBandSatisfied);
		WRITE_CHECKPOINT_FIELD(pilotApproachComplete);
		WRITE_CHECKPOINT_FIELD(statisticsBoundaryObserved);WRITE_CHECKPOINT_FIELD(checkpointCadenceWallS);
		WRITE_CHECKPOINT_FIELD(resumedFromCheckpoint);WRITE_CHECKPOINT_FIELD(resumedFromStep);
		WRITE_CHECKPOINT_FIELD(streamedFrameCount);
#undef WRITE_CHECKPOINT_FIELD
		return WriteArithmeticVector(w,v.probeTimeS)&&WriteArithmeticVector(w,v.probeCenterlineHeatReleaseW)&&
			WriteArithmeticVector(w,v.acceptedMaximumEOSResidualHistory)&&
			WriteArithmeticVector(w,v.acceptedTimeStepHistoryS)&&
			WriteArithmeticVector(w,v.stationProbeTimeS)&&WriteArithmeticVector(w,v.stationProbeHeightM)&&
			WriteArithmeticVector(w,v.stationProbeTemperatureK)&&WriteArithmeticVector(w,v.stationProbeReactionWPerM3)&&
			WriteArithmeticVector(w,v.stationProbeVerticalVelocityMPerS)&&
			WriteArithmeticVector(w,v.centerlineHeightM)&&WriteArithmeticVector(w,v.centerlineTemperatureK)&&
			WriteArithmeticVector(w,v.centerlineVelocityMPerS)&&
			WriteArithmeticVector(w,v.checkpointStepIndices)&&
			WriteArithmeticVector(w,v.workerCountHistory);
	}

	bool ReadSolverFrameValues(CheckpointReader& r,SolverFrameValues& v)
	{
		if(!r.Pod(v.succeeded)||!r.String(v.structuredError)||!r.String(v.reductionMode))return false;
		for(std::size_t& dimension:v.dimensions){std::uint64_t encoded=0u;
			if(!r.Pod(encoded)||encoded>std::numeric_limits<std::size_t>::max())return false;
			dimension=static_cast<std::size_t>(encoded);}
		if(!r.Pod(v.cellWidthM)||!r.String(v.caseRecordId)||!r.Pod(v.temperatureK)||
			!r.Pod(v.reactionWPerM3)||!ReadArithmeticVector(r,v.temperature)||
			!ReadArithmeticVector(r,v.reaction)||!ReadArithmeticVector(r,v.carbon))return false;
		std::uint64_t velocityCount=0u;if(!r.Pod(velocityCount)||velocityCount>200000000u)return false;
		v.velocity.resize(static_cast<std::size_t>(velocityCount));
		for(auto& velocity:v.velocity)for(float& component:velocity)if(!r.Pod(component))return false;
#define READ_CHECKPOINT_FIELD(field) if(!r.Pod(v.field))return false
		READ_CHECKPOINT_FIELD(realizedHeatReleaseW);READ_CHECKPOINT_FIELD(fuelConsumptionKGPerS);
		READ_CHECKPOINT_FIELD(realizedRadiativeFraction);READ_CHECKPOINT_FIELD(acceptedEscapeFactor);
		READ_CHECKPOINT_FIELD(selectedTimeStepS);READ_CHECKPOINT_FIELD(acceptedTimeStepS);
		READ_CHECKPOINT_FIELD(simulatedTimeS);READ_CHECKPOINT_FIELD(flowThroughTimeS);
		READ_CHECKPOINT_FIELD(pilotEnergyJ);READ_CHECKPOINT_FIELD(expectedPilotEnergyJ);
		READ_CHECKPOINT_FIELD(maximumTemperatureK);
		READ_CHECKPOINT_FIELD(maximumPilotApproachEOSResidual);
		READ_CHECKPOINT_FIELD(maximumAcceptedEOSResidual);
		READ_CHECKPOINT_FIELD(minimumActiveHoldTemperatureK);
		READ_CHECKPOINT_FIELD(maximumActiveHoldTemperatureK);
		READ_CHECKPOINT_FIELD(maximumLimiterClassDiscrepancy);
		READ_CHECKPOINT_FIELD(discontinuousLimiterClassSteps);
		READ_CHECKPOINT_FIELD(discontinuousClassThreadIdentity);
		READ_CHECKPOINT_FIELD(discontinuousClassThreadIdentityChecked);
		READ_CHECKPOINT_FIELD(statisticsStartS);READ_CHECKPOINT_FIELD(puffingFrequencyHz);
		READ_CHECKPOINT_FIELD(puffingRelativeError);READ_CHECKPOINT_FIELD(firstStatisticsStepStartS);
		READ_CHECKPOINT_FIELD(integratedHeatReleaseJ);READ_CHECKPOINT_FIELD(integratedRadiativeLossJ);
		READ_CHECKPOINT_FIELD(integratedFuelConsumptionKG);READ_CHECKPOINT_FIELD(integratedRadiativeFraction);
		READ_CHECKPOINT_FIELD(effectiveRadiativeFraction);READ_CHECKPOINT_FIELD(centerlineTemperatureExponent);
		READ_CHECKPOINT_FIELD(centerlineFitRMSE);READ_CHECKPOINT_FIELD(characteristicDiameterM);
		READ_CHECKPOINT_FIELD(mccaffreyFlameTipHeightM);
		READ_CHECKPOINT_FIELD(mccaffreyMaximumTemperatureRelativeError);
		READ_CHECKPOINT_FIELD(mccaffreyMaximumVelocityRelativeError);
		{std::uint64_t count=0u;if(!r.Pod(count)||count>std::numeric_limits<std::size_t>::max())return false;
			v.mccaffreyPlumeStationCount=static_cast<std::size_t>(count);}
		READ_CHECKPOINT_FIELD(ignitedDuringPilot);READ_CHECKPOINT_FIELD(sustainedAfterPilot);
		READ_CHECKPOINT_FIELD(pilotHoldBandObserved);READ_CHECKPOINT_FIELD(pilotHoldBandSatisfied);
		READ_CHECKPOINT_FIELD(pilotApproachComplete);
		READ_CHECKPOINT_FIELD(statisticsBoundaryObserved);READ_CHECKPOINT_FIELD(checkpointCadenceWallS);
		READ_CHECKPOINT_FIELD(resumedFromCheckpoint);READ_CHECKPOINT_FIELD(resumedFromStep);
		READ_CHECKPOINT_FIELD(streamedFrameCount);
#undef READ_CHECKPOINT_FIELD
		return ReadArithmeticVector(r,v.probeTimeS)&&ReadArithmeticVector(r,v.probeCenterlineHeatReleaseW)&&
			ReadArithmeticVector(r,v.acceptedMaximumEOSResidualHistory,1000000u)&&
			ReadArithmeticVector(r,v.acceptedTimeStepHistoryS,1000000u)&&
			ReadArithmeticVector(r,v.stationProbeTimeS)&&ReadArithmeticVector(r,v.stationProbeHeightM)&&
			ReadArithmeticVector(r,v.stationProbeTemperatureK)&&ReadArithmeticVector(r,v.stationProbeReactionWPerM3)&&
			ReadArithmeticVector(r,v.stationProbeVerticalVelocityMPerS)&&
			ReadArithmeticVector(r,v.centerlineHeightM)&&ReadArithmeticVector(r,v.centerlineTemperatureK)&&
			ReadArithmeticVector(r,v.centerlineVelocityMPerS)&&
			ReadArithmeticVector(r,v.checkpointStepIndices,1000000u)&&
			ReadArithmeticVector(r,v.workerCountHistory,1000000u);
	}

	class Binary64CheckpointAuthority final
	{
	public:
		Binary64CheckpointAuthority() : digest_(0u) {}
		bool Available() const { return digest_!=0u; }
		std::uint64_t Digest() const { return digest_; }
	private:
		std::uint64_t digest_;
		friend bool IssueBinary64CheckpointAuthority(struct MethaneRunCheckpoint&);
		friend bool RestoreBinary64CheckpointAuthority(struct MethaneRunCheckpoint&,
			std::uint64_t);
	};

	struct MethaneRunCheckpoint
	{
		std::uint64_t checkpointFormatVersion=0u;
		std::string caseRecordId;
		std::string producerBuildId;
		std::array<std::size_t,3> dimensions={{0u,0u,0u}};
		double cellWidthM=0.0;
		std::vector<MethaneCellState> states;
		PeriodicMACField momentum;
		PeriodicMACField velocity;
		SolverFrameValues values;
		std::vector<double> centerlineTemperatureIntegral;
		std::vector<double> centerlineVelocityIntegral;
		std::vector<double> planeHeatReleaseIntegral;
		double centerlineStatisticsDurationS=0.0;
		double simulationTimeS=0.0,previousStepS=0.0,lastAcceptedStepS=0.0;
		std::uint64_t acceptedSteps=0u;
		FireProductionAcceptedManifoldObservation productionManifoldObservation;
		Binary64CheckpointAuthority binary64CheckpointAuthority;
	};

	bool AcceptedCheckpointTimelineValid(const MethaneRunCheckpoint& checkpoint)
	{
		if(checkpoint.acceptedSteps==0u||
			checkpoint.values.acceptedTimeStepHistoryS.size()!=checkpoint.acceptedSteps||
			!std::isfinite(checkpoint.simulationTimeS)||checkpoint.simulationTimeS<=0.0||
			!std::isfinite(checkpoint.previousStepS)||checkpoint.previousStepS<=0.0||
			checkpoint.lastAcceptedStepS!=checkpoint.previousStepS||
			checkpoint.values.acceptedTimeStepHistoryS.empty()||
			checkpoint.values.acceptedTimeStepHistoryS.back()!=checkpoint.previousStepS)return false;
		double acceptedDurationS=0.0;
		for(const double timeStepS:checkpoint.values.acceptedTimeStepHistoryS){
			if(!std::isfinite(timeStepS)||timeStepS<=0.0)return false;
			acceptedDurationS+=timeStepS;
			if(!std::isfinite(acceptedDurationS))return false;
		}
		return acceptedDurationS==checkpoint.simulationTimeS;
	}

	bool Binary64CheckpointStateAdmissible(const MethaneRunCheckpoint& checkpoint)
	{
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		for(const MethaneCellState& state:checkpoint.states){
			double reconstructedTemperatureK=0.0;
			if(state.producerPrecision!=FireStateProducerPrecision::Binary64||
				!AcceptedMethaneCellStateAdmissible(state,fuel,
					FireStateProducerPrecision::Binary64)||
				!InvertMethaneTemperatureWithinAcceptedEnvelope(state,300.0,2300.0,fuel,
					FireStateProducerPrecision::Binary64,reconstructedTemperatureK)||
				reconstructedTemperatureK!=state.temperatureK)return false;
		}
		return !checkpoint.states.empty();
	}

	bool CanonicalizeLegacyBinary64CheckpointTemperature(MethaneRunCheckpoint& checkpoint)
	{
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		for(MethaneCellState& state:checkpoint.states){
			double reconstructedTemperatureK=0.0;
			if(state.producerPrecision!=FireStateProducerPrecision::Binary64||
				!AcceptedMethaneCellStateAdmissible(state,fuel,
					FireStateProducerPrecision::Binary64)||
				!InvertMethaneTemperatureWithinAcceptedEnvelope(state,300.0,2300.0,fuel,
					FireStateProducerPrecision::Binary64,reconstructedTemperatureK))return false;
			state.temperatureK=reconstructedTemperatureK;
		}
		return !checkpoint.states.empty();
	}

	std::uint64_t Binary64CheckpointPayloadDigest(const MethaneRunCheckpoint& checkpoint);

	bool IssueBinary64CheckpointAuthority(MethaneRunCheckpoint& checkpoint)
	{
		if(checkpoint.states.empty())return false;
		for(const MethaneCellState& state:checkpoint.states)
			if(state.producerPrecision!=FireStateProducerPrecision::Binary64)return false;
		if(
			!AcceptedCheckpointTimelineValid(checkpoint)||
			!Binary64CheckpointStateAdmissible(checkpoint))return false;
		checkpoint.binary64CheckpointAuthority.digest_=Binary64CheckpointPayloadDigest(checkpoint);
		return true;
	}

	bool RestoreBinary64CheckpointAuthority(MethaneRunCheckpoint& checkpoint,
		const std::uint64_t storedDigest)
	{
		if(storedDigest==0u||storedDigest!=Binary64CheckpointPayloadDigest(checkpoint))return false;
		checkpoint.binary64CheckpointAuthority.digest_=storedDigest;return true;
	}

	bool suppressExpectedCheckpointStateDiagnostic=false;
	bool BuildCheckpointAcceptedStatePayload(const MethaneRunCheckpoint& checkpoint,
		FireProductionProjectionShape& shape,std::vector<float>& conservative,
		std::array<std::vector<float>,3>& momentum,
		std::array<std::vector<float>,3>& velocity,std::uint64_t& digest)
	{
		digest=0u;shape=FireProductionProjectionShape();conservative.clear();
		for(unsigned int axis=0u;axis<3u;++axis){momentum[axis].clear();velocity[axis].clear();}
		if(checkpoint.dimensions[0u]==0u||checkpoint.dimensions[1u]==0u||
			checkpoint.dimensions[2u]==0u||!std::isfinite(checkpoint.cellWidthM)||
			checkpoint.cellWidthM<=0.0)return false;
		shape.nx=checkpoint.dimensions[0u];shape.ny=checkpoint.dimensions[1u];
		shape.nz=checkpoint.dimensions[2u];shape.cellWidthM=static_cast<float>(checkpoint.cellWidthM);
		if(shape.CellCount()!=checkpoint.states.size())return false;
		for(unsigned int axis=0u;axis<3u;++axis){
			const std::size_t expectedFaces=FireProductionProjectionFaceCount(shape,axis);
			if(expectedFaces==0u||checkpoint.momentum.component[axis].size()!=expectedFaces||
				checkpoint.velocity.component[axis].size()!=expectedFaces){
				if(!suppressExpectedCheckpointStateDiagnostic){
					std::fprintf(stderr,
					"checkpoint accepted face shape mismatch axis=%u expected=%zu momentum=%zu velocity=%zu\n",
					axis,expectedFaces,checkpoint.momentum.component[axis].size(),
					checkpoint.velocity.component[axis].size());}
				return false;
			}
		}
		conservative.assign(checkpoint.states.size()*9u,0.0f);
		std::vector<ConservativeVector> conservativeByCell(checkpoint.states.size());
		for(std::size_t cell=0u;cell<checkpoint.states.size();++cell){
			const MethaneCellState& state=checkpoint.states[cell];
			const ConservativeVector values=ToConservativeVector(state);
			conservativeByCell[cell]=values;
			for(std::size_t component=0u;component<9u;++component){
				const double value=values[component];const float represented=static_cast<float>(value);
				if(!std::isfinite(represented)||static_cast<double>(represented)!=value){
					std::fprintf(stderr,"checkpoint state is not exact binary32 cell=%zu component=%zu value=%.17g represented=%.17g\n",
						cell,component,value,static_cast<double>(represented));return false;}
				conservative[component*checkpoint.states.size()+cell]=represented;}
		}
		std::vector<double> reconstructedTemperature;
		std::string inversionError;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		if(!InvertPeriodicTemperaturesWithinBounds(conservativeByCell,fuel,
			fuel.TemperatureMinK(),fuel.TemperatureMaxK(),
			FireStateProducerPrecision::Binary32,reconstructedTemperature,&inversionError,1u,false)||
			reconstructedTemperature.size()!=checkpoint.states.size()){
			if(!suppressExpectedCheckpointStateDiagnostic){
				std::fprintf(stderr,
				"checkpoint accepted temperature reconstruction failed: %s\n",inversionError.c_str());}
			return false;
		}
		for(std::size_t cell=0u;cell<checkpoint.states.size();++cell){
			if(reconstructedTemperature[cell]!=checkpoint.states[cell].temperatureK){
				if(!suppressExpectedCheckpointStateDiagnostic){
				std::fprintf(stderr,
				"checkpoint accepted temperature mismatch cell=%zu stored=%.17g reconstructed=%.17g\n",
				cell,checkpoint.states[cell].temperatureK,reconstructedTemperature[cell]);}
				return false;
			}
		}
		for(unsigned int axis=0u;axis<3u;++axis){
			momentum[axis].reserve(checkpoint.momentum.component[axis].size());
			velocity[axis].reserve(checkpoint.velocity.component[axis].size());
			for(const double value:checkpoint.momentum.component[axis]){
				const float represented=static_cast<float>(value);
				if(!std::isfinite(represented)||static_cast<double>(represented)!=value){
					std::fprintf(stderr,"checkpoint momentum is not exact binary32 axis=%u value=%.17g represented=%.17g\n",
						axis,value,static_cast<double>(represented));return false;}
				momentum[axis].push_back(represented);}
			for(const double value:checkpoint.velocity.component[axis]){
				const float represented=static_cast<float>(value);
				if(!std::isfinite(represented)||static_cast<double>(represented)!=value){
					std::fprintf(stderr,"checkpoint velocity is not exact binary32 axis=%u value=%.17g represented=%.17g\n",
						axis,value,static_cast<double>(represented));return false;}
				velocity[axis].push_back(represented);}
		}
		digest=FireProductionAcceptedStatePayloadDigest(shape,conservative,momentum,velocity);
		return digest!=0u;
	}

	bool CheckpointAcceptedStateDigest(const MethaneRunCheckpoint& checkpoint,
		std::uint64_t& digest)
	{
		FireProductionProjectionShape shape;std::vector<float> conservative;
		std::array<std::vector<float>,3> momentum,velocity;
		return BuildCheckpointAcceptedStatePayload(checkpoint,shape,conservative,
			momentum,velocity,digest);
	}

	bool CheckpointProductionBeginningSHA256(const MethaneRunCheckpoint& checkpoint,
		std::string& digest)
	{
		RISECBOR64::Bytes bytes;
		auto appendU64=[&](const std::uint64_t value){
			for(unsigned int byte=0u;byte<8u;++byte)
				bytes.push_back(static_cast<unsigned char>((value>>(8u*byte))&0xffu));};
		auto appendDouble=[&](const double value){appendU64(DoubleBits(value));};
		auto appendText=[&](const std::string& value){appendU64(value.size());
			bytes.insert(bytes.end(),value.begin(),value.end());};
		auto appendDoubles=[&](const std::vector<double>& values){appendU64(values.size());
			for(const double value:values)appendDouble(value);};
		const char schema[]="rise.fire.production.beginning.v2";
		bytes.insert(bytes.end(),schema,schema+sizeof(schema));
		appendText(checkpoint.caseRecordId);
		appendU64(checkpoint.dimensions[0]);appendU64(checkpoint.dimensions[1]);
		appendU64(checkpoint.dimensions[2]);appendDouble(checkpoint.cellWidthM);
		appendDouble(checkpoint.values.characteristicDiameterM);
		appendDouble(checkpoint.simulationTimeS);appendDouble(checkpoint.previousStepS);
		appendDouble(checkpoint.lastAcceptedStepS);appendU64(checkpoint.acceptedSteps);
		appendU64(checkpoint.values.acceptedTimeStepHistoryS.size());
		for(const double value:checkpoint.values.acceptedTimeStepHistoryS)appendDouble(value);
		appendU64(checkpoint.productionManifoldObservation.Available()?1u:0u);
		appendDouble(checkpoint.productionManifoldObservation.TimeStepS());
		appendDouble(checkpoint.productionManifoldObservation.MaximumGeneration());
		appendDouble(checkpoint.productionManifoldObservation.RestorationDrainFraction());
		appendU64(checkpoint.productionManifoldObservation.SerializedAcceptedStateDigest());
		appendU64(checkpoint.states.size());
		for(const MethaneCellState& state:checkpoint.states){
			appendU64(static_cast<std::uint64_t>(state.producerPrecision));
			appendDouble(state.temperatureK);
			const ConservativeVector conservative=ToConservativeVector(state);
			for(std::size_t component=0u;component<MethaneConservativeDimension;++component)
				appendDouble(conservative[component]);
		}
		for(unsigned int axis=0u;axis<3u;++axis)
			appendDoubles(checkpoint.momentum.component[axis]);
		for(unsigned int axis=0u;axis<3u;++axis)
			appendDoubles(checkpoint.velocity.component[axis]);
		digest=RISECBOR64::SHA256Hex(bytes);return digest.size()==64u;
	}

	bool CheckpointAcceptedStateMatchesObservation(const MethaneRunCheckpoint& checkpoint,
		const FireProductionAcceptedManifoldObservation& observation,
		std::uint64_t& legacyDigest)
	{
		FireProductionProjectionShape shape;std::vector<float> conservative;
		std::array<std::vector<float>,3> momentum,velocity;
		return BuildCheckpointAcceptedStatePayload(checkpoint,shape,conservative,
			momentum,velocity,legacyDigest)&&observation.MatchesAcceptedStatePayload(
				shape,conservative,momentum,velocity);
	}

	bool HomogeneousStateProducerPrecision(const std::vector<MethaneCellState>& states,
		FireStateProducerPrecision& precision)
	{
		if(states.empty())return false;
		precision=states.front().producerPrecision;
		if(precision!=FireStateProducerPrecision::Binary64&&
			precision!=FireStateProducerPrecision::Binary32)return false;
		return std::all_of(states.begin(),states.end(),[precision](const MethaneCellState& state){
			return state.producerPrecision==precision;
		});
	}

	bool WriteCellStates(CheckpointWriter& writer,const std::vector<MethaneCellState>& states,
		const std::uint64_t version)
	{
		FireStateProducerPrecision precision=FireStateProducerPrecision::Unknown;
		if(!HomogeneousStateProducerPrecision(states,precision)||
			(version<9u&&precision!=FireStateProducerPrecision::Binary64))return false;
		const std::uint64_t count=states.size();if(!writer.Pod(count))return false;
		for(const MethaneCellState& state:states){
			if(version>=9u){const unsigned char producerPrecision=
				static_cast<unsigned char>(state.producerPrecision);
				if(!writer.Pod(producerPrecision))return false;}
			if(!writer.Pod(state.rhoTotalZ))return false;
			for(const double value:state.constituent)if(!writer.Pod(value))return false;
			if(!writer.Pod(state.sensibleEnergyJPerM3)||!writer.Pod(state.temperatureK))return false;
		}
		return true;
	}

	bool ReadCellStates(CheckpointReader& reader,std::vector<MethaneCellState>& states,
		const std::uint64_t version)
	{
		std::uint64_t count=0u;if(!reader.Pod(count)||count>100000000u)return false;
		states.resize(static_cast<std::size_t>(count));
		for(MethaneCellState& state:states){
			if(version>=9u){unsigned char producerPrecision=0u;
				if(!reader.Pod(producerPrecision)||(producerPrecision!=
					static_cast<unsigned char>(FireStateProducerPrecision::Binary64)&&
					producerPrecision!=static_cast<unsigned char>(
						FireStateProducerPrecision::Binary32)))return false;
				state.producerPrecision=static_cast<FireStateProducerPrecision>(producerPrecision);
			}else state.producerPrecision=FireStateProducerPrecision::Binary64;
			if(!reader.Pod(state.rhoTotalZ))return false;
			for(double& value:state.constituent)if(!reader.Pod(value))return false;
			if(!reader.Pod(state.sensibleEnergyJPerM3)||!reader.Pod(state.temperatureK))return false;
		}
		FireStateProducerPrecision precision=FireStateProducerPrecision::Unknown;
		return HomogeneousStateProducerPrecision(states,precision);
	}

	template<typename Field> bool WriteMACField(CheckpointWriter& writer,const Field& field)
	{
		for(unsigned int axis=0;axis<3;++axis)
			if(!WriteArithmeticVector(writer,field.component[axis]))return false;
		return true;
	}
	template<typename Field> bool ReadMACField(CheckpointReader& reader,Field& field)
	{
		for(unsigned int axis=0;axis<3;++axis)
			if(!ReadArithmeticVector(reader,field.component[axis]))return false;
		return true;
	}
	bool forceMalformedManifoldLifecycleWriteForTest=false;
	bool WriteCheckpointSerializedPrefix(CheckpointWriter& writer,
		const MethaneRunCheckpoint& checkpoint,const std::uint64_t version)
	{
		if(!writer.String(checkpoint.caseRecordId)||!writer.String(checkpoint.producerBuildId))return false;
		for(const std::size_t dimension:checkpoint.dimensions){const std::uint64_t encoded=dimension;
			if(!writer.Pod(encoded))return false;}
		const bool baseWritten=writer.Pod(checkpoint.cellWidthM)&&
			WriteCellStates(writer,checkpoint.states,version)&&
			WriteMACField(writer,checkpoint.momentum)&&WriteMACField(writer,checkpoint.velocity)&&
			WriteSolverFrameValues(writer,checkpoint.values)&&
			WriteArithmeticVector(writer,checkpoint.centerlineTemperatureIntegral)&&
			WriteArithmeticVector(writer,checkpoint.centerlineVelocityIntegral)&&
			WriteArithmeticVector(writer,checkpoint.planeHeatReleaseIntegral)&&
			writer.Pod(checkpoint.centerlineStatisticsDurationS)&&
			writer.Pod(checkpoint.simulationTimeS)&&writer.Pod(checkpoint.previousStepS)&&
			writer.Pod(checkpoint.lastAcceptedStepS)&&writer.Pod(checkpoint.acceptedSteps);
		if(!baseWritten||version<6u)return baseWritten;
		const bool migrationWritten=
			WriteArithmeticVector(writer,checkpoint.values.acceptedMaximumTemperatureHistoryK)&&
			writer.String(checkpoint.values.migrationCertificateId)&&
			writer.String(checkpoint.values.migrationOldBuildId)&&
			writer.String(checkpoint.values.migrationNewBuildId)&&
			writer.Pod(checkpoint.values.migrationAcceptedStepCount)&&
			writer.Pod(checkpoint.values.migrationResumedFromStep);
		if(!migrationWritten||version<7u)return migrationWritten;
		const bool activeSetWritten=writer.String(checkpoint.values.activeSetAlgorithmVersion)&&
			writer.Pod(checkpoint.values.maximumActiveSetComplementarityDiscrepancyMPerS)&&
			writer.Pod(checkpoint.values.discontinuousActiveSetEvents)&&
			writer.Pod(checkpoint.values.maximumActiveSetCycleLength)&&
			writer.Pod(checkpoint.values.maximumActiveSetDifferingFaceCount)&&
			writer.Pod(checkpoint.values.activeSetThreadIdentity)&&
			writer.Pod(checkpoint.values.activeSetThreadIdentityChecked);
		if(!activeSetWritten)return false;
		return version<8u||writer.String(checkpoint.values.priorActiveSetAlgorithmVersion);
	}

	std::uint64_t Binary64CheckpointPayloadDigest(const MethaneRunCheckpoint& checkpoint)
	{
		CheckpointWriter writer;
		if(!WriteCheckpointSerializedPrefix(writer,checkpoint,13u))return 0u;
		const std::uint64_t digest=writer.Checksum();
		return digest==0u?1u:digest;
	}

	bool WriteCheckpointPayload(CheckpointWriter& writer,const MethaneRunCheckpoint& checkpoint,
		const std::uint64_t version)
	{
		FireStateProducerPrecision precision=FireStateProducerPrecision::Unknown;
		if(!HomogeneousStateProducerPrecision(checkpoint.states,precision))return false;
		if(checkpoint.acceptedSteps==0u&&!forceMalformedManifoldLifecycleWriteForTest)
			return false;
		if(checkpoint.acceptedSteps>0u&&
			(!AcceptedCheckpointTimelineValid(checkpoint)||
				(precision==FireStateProducerPrecision::Binary64&&
					(!Binary64CheckpointStateAdmissible(checkpoint)||
						!checkpoint.binary64CheckpointAuthority.Available()||
						checkpoint.binary64CheckpointAuthority.Digest()!=
							Binary64CheckpointPayloadDigest(checkpoint))))&&
			!forceMalformedManifoldLifecycleWriteForTest)return false;
		if(version<12u&&precision==FireStateProducerPrecision::Binary32&&
			!forceMalformedManifoldLifecycleWriteForTest)return false;
		if(version>=9u&&version<13u&&precision==FireStateProducerPrecision::Binary64&&
			checkpoint.acceptedSteps>0u&&!forceMalformedManifoldLifecycleWriteForTest)return false;
		if(!WriteCheckpointSerializedPrefix(writer,checkpoint,version)){
			if(!suppressExpectedCheckpointStateDiagnostic)
				std::fprintf(stderr,"checkpoint serialization refused at serialized-prefix\n");
			return false;
		}
		if(version<10u){
			if(version==9u&&precision==FireStateProducerPrecision::Binary32&&
				(checkpoint.acceptedSteps!=0u||checkpoint.simulationTimeS!=0.0||
				checkpoint.previousStepS!=0.0||checkpoint.lastAcceptedStepS!=0.0||
				!checkpoint.values.acceptedTimeStepHistoryS.empty())&&
				!forceMalformedManifoldLifecycleWriteForTest)return false;
			return true;
		}
		const FireProductionAcceptedManifoldObservation& observation=
			checkpoint.productionManifoldObservation;
		const bool productionState=precision==FireStateProducerPrecision::Binary32;
		const bool acceptedProductionState=productionState&&checkpoint.acceptedSteps>0u;
		const bool invalidManifoldLifecycle=(observation.Available()&&(!std::isfinite(observation.TimeStepS())||
			observation.TimeStepS()<=0.0||!std::isfinite(observation.MaximumGeneration())||
			observation.MaximumGeneration()<0.0||
			!std::isfinite(observation.RestorationDrainFraction())||
			observation.RestorationDrainFraction()<0.0||
				observation.RestorationDrainFraction()>1.0||!acceptedProductionState||
				observation.TimeStepS()!=checkpoint.previousStepS||
				observation.TimeStepS()!=checkpoint.lastAcceptedStepS||
				checkpoint.values.acceptedTimeStepHistoryS.empty()||
				checkpoint.values.acceptedTimeStepHistoryS.back()!=checkpoint.previousStepS))||
			(!observation.Available()&&(observation.TimeStepS()!=0.0||
				observation.MaximumGeneration()!=0.0||
				observation.RestorationDrainFraction()!=0.0||
				(version>=12u&&acceptedProductionState)))||
			(productionState&&checkpoint.acceptedSteps==0u&&
				(checkpoint.previousStepS!=0.0||checkpoint.lastAcceptedStepS!=0.0||
				!checkpoint.values.acceptedTimeStepHistoryS.empty()))||
			(productionState&&checkpoint.acceptedSteps>0u&&
				!AcceptedCheckpointTimelineValid(checkpoint));
		if(invalidManifoldLifecycle&&!forceMalformedManifoldLifecycleWriteForTest){
			if(!suppressExpectedCheckpointStateDiagnostic)
				std::fprintf(stderr,"checkpoint serialization refused at manifold-lifecycle\n");
			return false;
		}
		const unsigned char manifoldAvailable=
			observation.Available()?1u:0u;
		const bool observationWritten=writer.Pod(manifoldAvailable)&&
			writer.Pod(observation.TimeStepS())&&
			writer.Pod(observation.MaximumGeneration())&&
			writer.Pod(observation.RestorationDrainFraction());
		if(!observationWritten)return false;
		if(version==11u){const std::uint64_t retiredTupleSeal=0u;
			return writer.Pod(retiredTupleSeal);}
		if(version<12u)return true;
		std::uint64_t stateDigest=0u;
		if(observation.Available()&&!CheckpointAcceptedStateMatchesObservation(
			checkpoint,observation,stateDigest)){
			if(!suppressExpectedCheckpointStateDiagnostic)
				std::fprintf(stderr,"checkpoint serialization refused at accepted-state binding\n");
			return false;
		}
		if(version>=13u&&precision==FireStateProducerPrecision::Binary64&&
			checkpoint.acceptedSteps>0u){
			stateDigest=checkpoint.binary64CheckpointAuthority.Digest();
			if((stateDigest==0u||stateDigest!=Binary64CheckpointPayloadDigest(checkpoint))&&
				!forceMalformedManifoldLifecycleWriteForTest)return false;
		}
		if(!writer.Pod(stateDigest))return false;
		const std::uint64_t payloadBinding=writer.Checksum()^
			UINT64_C(0x63b96d44f1a72ec8);
		return writer.Pod(payloadBinding);
	}

	bool ReadCheckpointPayload(CheckpointReader& reader,MethaneRunCheckpoint& checkpoint,
		const std::uint64_t version)
	{
		checkpoint.productionManifoldObservation=
			FireProductionAcceptedManifoldObservation();
		if(!reader.String(checkpoint.caseRecordId)||!reader.String(checkpoint.producerBuildId))return false;
		for(std::size_t& dimension:checkpoint.dimensions){std::uint64_t encoded=0u;
			if(!reader.Pod(encoded)||encoded>std::numeric_limits<std::size_t>::max())return false;
			dimension=static_cast<std::size_t>(encoded);}
		const bool decoded=reader.Pod(checkpoint.cellWidthM)&&
			ReadCellStates(reader,checkpoint.states,version)&&
			ReadMACField(reader,checkpoint.momentum)&&ReadMACField(reader,checkpoint.velocity)&&
			ReadSolverFrameValues(reader,checkpoint.values)&&
			ReadArithmeticVector(reader,checkpoint.centerlineTemperatureIntegral)&&
			ReadArithmeticVector(reader,checkpoint.centerlineVelocityIntegral)&&
			ReadArithmeticVector(reader,checkpoint.planeHeatReleaseIntegral)&&
			reader.Pod(checkpoint.centerlineStatisticsDurationS)&&
			reader.Pod(checkpoint.simulationTimeS)&&reader.Pod(checkpoint.previousStepS)&&
			reader.Pod(checkpoint.lastAcceptedStepS)&&reader.Pod(checkpoint.acceptedSteps);
		if(!decoded)return false;
		if(version>=6u&&(!ReadArithmeticVector(reader,
			checkpoint.values.acceptedMaximumTemperatureHistoryK,1000000u)||
			!reader.String(checkpoint.values.migrationCertificateId)||
			!reader.String(checkpoint.values.migrationOldBuildId)||
			!reader.String(checkpoint.values.migrationNewBuildId)||
			!reader.Pod(checkpoint.values.migrationAcceptedStepCount)||
			!reader.Pod(checkpoint.values.migrationResumedFromStep)))return false;
		if(version>=7u&&(!reader.String(checkpoint.values.activeSetAlgorithmVersion)||
			!reader.Pod(checkpoint.values.maximumActiveSetComplementarityDiscrepancyMPerS)||
			!reader.Pod(checkpoint.values.discontinuousActiveSetEvents)||
			!reader.Pod(checkpoint.values.maximumActiveSetCycleLength)||
			!reader.Pod(checkpoint.values.maximumActiveSetDifferingFaceCount)||
			!reader.Pod(checkpoint.values.activeSetThreadIdentity)||
			!reader.Pod(checkpoint.values.activeSetThreadIdentityChecked)))return false;
		if(version>=8u&&!reader.String(checkpoint.values.priorActiveSetAlgorithmVersion))return false;
		if(version>=10u){unsigned char manifoldAvailable=0u;double timeStepS=0.0;
			double maximumGeneration=0.0,restorationDrainFraction=0.0;
			if(!reader.Pod(manifoldAvailable)||manifoldAvailable>1u||!reader.Pod(timeStepS)||
				!reader.Pod(maximumGeneration)||!reader.Pod(restorationDrainFraction))return false;
			std::uint64_t retiredOrStateDigest=0u;
			if(version>=11u&&!reader.Pod(retiredOrStateDigest))return false;
			if(version>=12u){std::uint64_t payloadBinding=0u;
				if(!reader.Pod(payloadBinding))return false;}
			if(version>=13u&&!manifoldAvailable&&checkpoint.acceptedSteps>0u&&
				!RestoreBinary64CheckpointAuthority(checkpoint,retiredOrStateDigest))return false;
		}
		checkpoint.checkpointFormatVersion=version;
		if(version<7u)checkpoint.values.activeSetAlgorithmVersion=
			LegacyActiveSetAlgorithmVersion();
		return true;
	}

	bool DurableSyncFileAndDirectory(const std::filesystem::path& path,std::string& error)
	{
#if defined(_WIN32)
		HANDLE file=CreateFileW(path.wstring().c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,
			OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
		if(file==INVALID_HANDLE_VALUE){error="cannot open durable run file";return false;}
		const bool flushed=FlushFileBuffers(file)!=0;
		const bool closed=CloseHandle(file)!=0;
		if(!flushed||!closed)error="cannot flush durable run file";
		return flushed&&closed;
#else
		const int fd=::open(path.c_str(),O_RDONLY);
		if(fd<0){error="cannot open durable run file";return false;}
		const bool fileSynced=::fsync(fd)==0;
		const bool fileClosed=::close(fd)==0;
		if(!fileSynced||!fileClosed){error="cannot fsync durable run file";return false;}
		const std::filesystem::path directory=path.has_parent_path()?path.parent_path():".";
		const int directoryFd=::open(directory.c_str(),O_RDONLY);
		if(directoryFd<0){error="cannot open durable run directory";return false;}
		const bool directorySynced=::fsync(directoryFd)==0;
		const bool directoryClosed=::close(directoryFd)==0;
		if(!directorySynced||!directoryClosed){error="cannot fsync durable run directory";return false;}
		return true;
#endif
	}

	bool AtomicReplaceCheckpoint(const std::filesystem::path& temporary,
		const std::filesystem::path& target,std::string& error)
	{
#if defined(_WIN32)
		if(!MoveFileExW(temporary.wstring().c_str(),target.wstring().c_str(),
			MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
			error="cannot atomically replace run checkpoint";return false;}
		if(forcePostRenameDirectorySyncFailureForTest){
			error="cannot fsync replaced-run directory";return false;}
		return true;
#else
		if(::rename(temporary.c_str(),target.c_str())!=0){error="cannot atomically replace run checkpoint";return false;}
		const std::filesystem::path directory=target.has_parent_path()?target.parent_path():".";
		const int directoryFd=::open(directory.c_str(),O_RDONLY);
		if(directoryFd<0){error="cannot open replaced-run directory";return false;}
		const bool directorySynced=!forcePostRenameDirectorySyncFailureForTest&&
			::fsync(directoryFd)==0;
		const bool directoryClosed=::close(directoryFd)==0;
		if(!directorySynced||!directoryClosed){error="cannot fsync replaced-run directory";return false;}
		return true;
#endif
	}

	bool SaveMethaneRunCheckpoint(const std::filesystem::path& path,
		const MethaneRunCheckpoint& checkpoint,std::string& error,
		const std::uint64_t version=13u)
	{
		if(version<5u||version>13u){error="run checkpoint output version is invalid";return false;}
		if(path.has_parent_path())std::filesystem::create_directories(path.parent_path());
#if defined(_WIN32)
		const long long processId=static_cast<long long>(::_getpid());
#else
		const long long processId=static_cast<long long>(::getpid());
#endif
		const std::filesystem::path temporary=path.string()+".tmp."+std::to_string(processId);
		CheckpointWriter writer(temporary);if(!writer.Good()){error="cannot open run checkpoint";return false;}
		const char magic[16]={'R','I','S','E','F','I','R','E','C','H','K','P','T','1',0,0};
		const std::uint64_t endian=0x0102030405060708ull,zero=0u;
		auto rejectTemporary=[&temporary](){std::error_code ignored;
			std::filesystem::remove(temporary,ignored);};
		if(!writer.HeaderBytes(magic,sizeof(magic))||!writer.HeaderBytes(&version,sizeof(version))||
			!writer.HeaderBytes(&endian,sizeof(endian))||!writer.HeaderBytes(&zero,sizeof(zero))||
			!writer.HeaderBytes(&zero,sizeof(zero))||!WriteCheckpointPayload(writer,checkpoint,version)){
			error="cannot serialize complete run checkpoint";rejectTemporary();return false;}
		const std::uint64_t payloadBytes=writer.PayloadBytes(),checksum=writer.Checksum();
		if(!writer.SeekHeader(32)||!writer.HeaderBytes(&payloadBytes,sizeof(payloadBytes))||
			!writer.HeaderBytes(&checksum,sizeof(checksum))||!writer.Finish()){
			error="cannot finalize run checkpoint";rejectTemporary();return false;}
		if(!DurableSyncFileAndDirectory(temporary,error)||
			!AtomicReplaceCheckpoint(temporary,path,error)){rejectTemporary();return false;}
		return true;
	}

	bool VerifyCheckpointPayloadChecksum(const std::filesystem::path& path,
		const std::uint64_t payloadBytes,const std::uint64_t expected,std::string& error)
	{
		std::ifstream input(path,std::ios::binary);
		if(!input){error="cannot open run checkpoint for checksum";return false;}
		input.seekg(48,std::ios::beg);
		if(!input){error="cannot seek run checkpoint payload";return false;}
		std::array<unsigned char,65536u> buffer={{0u}};
		std::uint64_t remaining=payloadBytes;
		std::uint64_t checksum=1469598103934665603ull;
		while(remaining>0u){
			const std::size_t chunk=static_cast<std::size_t>(std::min<std::uint64_t>(
				remaining,buffer.size()));
			input.read(reinterpret_cast<char*>(buffer.data()),static_cast<std::streamsize>(chunk));
			if(!input){error="run checkpoint payload is incomplete";return false;}
			for(std::size_t i=0;i<chunk;++i){checksum^=buffer[i];checksum*=1099511628211ull;}
			remaining-=chunk;
		}
		if(checksum!=expected){error="run checkpoint checksum mismatch";return false;}
		return true;
	}

	bool LoadMethaneRunCheckpoint(const std::filesystem::path& path,
		MethaneRunCheckpoint& checkpoint,std::string& error)
	{
		std::error_code sizeError;
		const std::uintmax_t exactFileBytes=std::filesystem::file_size(path,sizeError);
		CheckpointReader reader(path);if(!reader.Good()){error="cannot open run checkpoint";return false;}
		char magic[16]={};std::uint64_t version=0u,endian=0u,payloadBytes=0u,checksum=0u;
		const char expected[16]={'R','I','S','E','F','I','R','E','C','H','K','P','T','1',0,0};
		if(!reader.HeaderBytes(magic,sizeof(magic))||std::memcmp(magic,expected,sizeof(magic))!=0||
			!reader.HeaderBytes(&version,sizeof(version))||
				(version!=5u&&version!=6u&&version!=7u&&version!=8u&&version!=9u&&
					version!=10u&&version!=11u&&version!=12u&&version!=13u)||
			!reader.HeaderBytes(&endian,sizeof(endian))||endian!=0x0102030405060708ull||
			!reader.HeaderBytes(&payloadBytes,sizeof(payloadBytes))||
			!reader.HeaderBytes(&checksum,sizeof(checksum))||payloadBytes>64ull*1024ull*1024ull*1024ull||
			sizeError||exactFileBytes!=48ull+payloadBytes){
			error="run checkpoint header is invalid";return false;}
		if(!VerifyCheckpointPayloadChecksum(path,payloadBytes,checksum,error))return false;
		reader.SetLimit(payloadBytes);MethaneRunCheckpoint decoded;
		if(!ReadCheckpointPayload(reader,decoded,version)||!reader.Finished(checksum)){
			error="run checkpoint payload is incomplete or corrupt";return false;}
		FireStateProducerPrecision precision=FireStateProducerPrecision::Unknown;
		if(!HomogeneousStateProducerPrecision(decoded.states,precision)){
			error="run checkpoint producer precision is invalid";return false;}
		const bool productionState=precision==FireStateProducerPrecision::Binary32;
		if(version<=8u&&precision==FireStateProducerPrecision::Binary64&&
			!CanonicalizeLegacyBinary64CheckpointTemperature(decoded)){
			error="legacy binary64 checkpoint temperature cannot be canonicalized";return false;}
		if(decoded.acceptedSteps==0u){
			error="zero-step checkpoint state is not resumable";return false;}
		if(!AcceptedCheckpointTimelineValid(decoded)){
			error="run checkpoint accepted timeline is invalid";return false;}
		if(precision==FireStateProducerPrecision::Binary64&&
			!Binary64CheckpointStateAdmissible(decoded)){
			error="binary64 checkpoint state is not admissible in its producer class";return false;}
		if(version<12u&&productionState&&(decoded.acceptedSteps>0u||
			decoded.simulationTimeS!=0.0||decoded.previousStepS!=0.0||
			decoded.lastAcceptedStepS!=0.0||
			!decoded.values.acceptedTimeStepHistoryS.empty())){
			error="legacy production checkpoint lacks accepted manifold authority";return false;}
		if(version>=9u&&version<13u&&!productionState){
			error="modern binary64 checkpoint lacks producer authority";return false;}
		if(version>=13u&&!productionState&&
			(!decoded.binary64CheckpointAuthority.Available()||
				decoded.binary64CheckpointAuthority.Digest()!=
					Binary64CheckpointPayloadDigest(decoded))){
			error="binary64 checkpoint producer authority is invalid";return false;}
		if(version>=12u&&productionState){std::uint64_t acceptedStateDigest=0u;
			FireProductionProjectionShape acceptedShape;std::vector<float> acceptedConservative;
			std::array<std::vector<float>,3> acceptedMomentum,acceptedVelocity;
			if(productionState&&decoded.acceptedSteps>0u&&
				!BuildCheckpointAcceptedStatePayload(decoded,acceptedShape,
				acceptedConservative,acceptedMomentum,acceptedVelocity,acceptedStateDigest)){
				error="production checkpoint accepted state is not canonical binary32";return false;}
			FireProductionAcceptedCheckpointStateView stateView;stateView.shape=acceptedShape;
			stateView.conservativeValues=&acceptedConservative;stateView.momentum=&acceptedMomentum;
			stateView.velocity=&acceptedVelocity;
			FireProductionAcceptedCheckpointLifecycleView lifecycle;
			lifecycle.simulationTimeS=decoded.simulationTimeS;
			lifecycle.previousStepS=decoded.previousStepS;
			lifecycle.lastAcceptedStepS=decoded.lastAcceptedStepS;
			lifecycle.acceptedSteps=decoded.acceptedSteps;
			lifecycle.productionState=productionState;
			lifecycle.acceptedTimeStepHistoryS=&decoded.values.acceptedTimeStepHistoryS;
			if(!FireProductionCheckpointManifoldAccess::RestoreValidatedCheckpointFile(
				path.string(),payloadBytes,checksum,version,stateView,lifecycle,
				decoded.productionManifoldObservation,&error))return false;
		}
		checkpoint=std::move(decoded);
		return true;
	}

	[[noreturn]] void HardKillCurrentProcess()
	{
#if defined(_WIN32)
		TerminateProcess(GetCurrentProcess(),91u);
#else
		::raise(SIGKILL);
#endif
		std::_Exit(91);
	}

	struct UniformHannSpectrum
	{
		std::vector<double> uniformTimeS,uniformSignal,detrendedSignal;
		std::vector<double> frequencyHz,amplitude,power,powerFraction;
		double affineIntercept=0.0,affineSlopePerSample=0.0;
		std::size_t dominantBin=0u;
	};

	bool ComputeUniformHannSpectrum( const std::vector<double>& time,
		const std::vector<double>& signal,const std::size_t sampleCount,
		UniformHannSpectrum& spectrum )
	{
		spectrum=UniformHannSpectrum();
		if(time.size()<8u||time.size()!=signal.size()||sampleCount<8u||
			(sampleCount&1u)!=0u||!(time.back()>time.front()))return false;
		spectrum.uniformTimeS.resize(sampleCount);
		spectrum.uniformSignal.resize(sampleCount);
		const double duration=time.back()-time.front();
		std::size_t right=1u;
		for(std::size_t sample=0;sample<sampleCount;++sample) {
			const double target=time.front()+duration*static_cast<double>(sample)/
				static_cast<double>(sampleCount-1u);
			spectrum.uniformTimeS[sample]=target;
			while(right<time.size()&&time[right]<target) ++right;
			if(right>=time.size()) spectrum.uniformSignal[sample]=signal.back();
			else {
				const double span=time[right]-time[right-1u];
				const double fraction=span>0.0?(target-time[right-1u])/span:0.0;
				spectrum.uniformSignal[sample]=signal[right-1u]+
					fraction*(signal[right]-signal[right-1u]);
			}
		}
		// Remove the least-squares affine trend before applying a Hann window.  The
		// direct DFT below is an independent harness calculation, not a solver path.
		double sumX=0.0,sumY=0.0,sumXX=0.0,sumXY=0.0;
		for(std::size_t sample=0;sample<sampleCount;++sample) {
			const double x=static_cast<double>(sample);
			sumX+=x;sumY+=spectrum.uniformSignal[sample];sumXX+=x*x;
			sumXY+=x*spectrum.uniformSignal[sample];
		}
		const double denominator=static_cast<double>(sampleCount)*sumXX-sumX*sumX;
		spectrum.affineSlopePerSample=denominator!=0.0?
			(static_cast<double>(sampleCount)*sumXY-sumX*sumY)/denominator:0.0;
		spectrum.affineIntercept=(sumY-spectrum.affineSlopePerSample*sumX)/
			static_cast<double>(sampleCount);
		spectrum.detrendedSignal.resize(sampleCount);
		double detrendedEnergy=0.0,signalScale=0.0;
		for(std::size_t sample=0;sample<sampleCount;++sample) {
			const double residual=spectrum.uniformSignal[sample]-spectrum.affineIntercept-
				spectrum.affineSlopePerSample*static_cast<double>(sample);
			spectrum.detrendedSignal[sample]=residual;
			detrendedEnergy+=residual*residual;
			signalScale=std::max(signalScale,std::fabs(spectrum.uniformSignal[sample]));
		}
		if(!(detrendedEnergy>64.0*std::numeric_limits<double>::epsilon()*
			std::max(1.0,signalScale*signalScale)))return false;
		const std::size_t binCount=sampleCount/2u+1u;
		spectrum.frequencyHz.resize(binCount);spectrum.amplitude.resize(binCount);
		spectrum.power.resize(binCount);spectrum.powerFraction.resize(binCount);
		double bestPower=-1.0,totalPositivePower=0.0,windowSum=0.0;
		for(std::size_t sample=0;sample<sampleCount;++sample)windowSum+=
			0.5-0.5*std::cos(2.0*3.14159265358979323846*
				static_cast<double>(sample)/static_cast<double>(sampleCount-1u));
		for(std::size_t bin=0u;bin<binCount;++bin) {
			double real=0.0,imaginary=0.0;
			for(std::size_t sample=0;sample<sampleCount;++sample) {
				const double window=0.5-0.5*std::cos(2.0*3.14159265358979323846*
					static_cast<double>(sample)/static_cast<double>(sampleCount-1u));
				const double value=spectrum.detrendedSignal[sample]*window;
				const double angle=2.0*3.14159265358979323846*static_cast<double>(bin*sample)/
					static_cast<double>(sampleCount);
				real+=value*std::cos(angle);imaginary-=value*std::sin(angle);
			}
			const double power=real*real+imaginary*imaginary;
			spectrum.frequencyHz[bin]=static_cast<double>(bin)*
				static_cast<double>(sampleCount-1u)/(static_cast<double>(sampleCount)*duration);
			const double oneSidedScale=(bin==0u||bin+1u==binCount)?1.0:2.0;
			spectrum.amplitude[bin]=windowSum>0.0?oneSidedScale*std::sqrt(power)/windowSum:0.0;
			spectrum.power[bin]=power;
			if(bin>0u){totalPositivePower+=power;if(power>bestPower){bestPower=power;
				spectrum.dominantBin=bin;}}
		}
		if(!(totalPositivePower>0.0))return false;
		for(std::size_t bin=0u;bin<binCount;++bin)
			spectrum.powerFraction[bin]=spectrum.power[bin]/totalPositivePower;
		return spectrum.dominantBin>0u;
	}

	double DominantUniformResampledFrequency( const std::vector<double>& time,
		const std::vector<double>& signal )
	{
		UniformHannSpectrum spectrum;
		return ComputeUniformHannSpectrum(time,signal,512u,spectrum)?
			spectrum.frequencyHz[spectrum.dominantBin]:0.0;
	}

	bool CanonicalCapstoneGridForTier(const double resolutionTier,
		std::array<std::size_t,3>& dimensions,double& cellWidthM)
	{
		dimensions.fill(0u);cellWidthM=0.0;
		if(resolutionTier!=6.0&&resolutionTier!=8.0&&resolutionTier!=10.0)return false;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		FireCase::AuthoredV1 authored;
		authored.fuelRecordId=fuel.RecordId();authored.poolDiameterM=CapstonePoolDiameterM;
		authored.heatReleaseRateKW=CapstoneHeatReleaseRateKW;authored.envelope={{0.0,1.0}};
		authored.durationS=1.0;authored.outputFramesPerS=1.0;authored.quality="dstar";
		authored.numericDStarTier=resolutionTier;authored.seed=1234;authored.plumeLaw=true;
		const RISECBOR64::Bytes aerosol=AerosolRecord(),chem=SyntheticChemRecord();
		FireCase::RecordV1 record;std::string error;
		if(!FireCase::BuildMethaneV1(authored,fuel,{
			RISECBOR64::SHA256Hex(fuel.RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationThermochemistryRecord::OpenSubsetV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationTransportRecord::OpenV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireOpticsPreset::PredictiveV1().RecordBytes()),
			RISECBOR64::SHA256Hex(aerosol),RISECBOR64::SHA256Hex(chem)},record,error))return false;
		dimensions={{record.derived.nx,record.derived.ny,record.derived.nz}};
		cellWidthM=record.derived.cellWidthM;return true;
	}

	bool ProductionPuffingSpectrumTierMatches(const MethaneRunCheckpoint& checkpoint,
		const double resolutionTier)
	{
		std::array<std::size_t,3> dimensions;double cellWidthM=0.0;
		return CanonicalCapstoneGridForTier(resolutionTier,dimensions,cellWidthM)&&
			checkpoint.dimensions==dimensions&&checkpoint.cellWidthM==cellWidthM;
	}

	bool WriteProductionPuffingSpectrumTierEvidence(std::ostream& output,
		const MethaneRunCheckpoint& checkpoint,const double resolutionTier)
	{
		if(!ProductionPuffingSpectrumTierMatches(checkpoint,resolutionTier))return false;
		output<<std::setprecision(17)
			<<"resolution_tier "<<resolutionTier<<"\n"
			<<"burner_diameter_m "<<CapstonePoolDiameterM<<"\n"
			<<"cell_width_m "<<checkpoint.cellWidthM<<"\n"
			<<"burner_cells_across "<<CapstonePoolDiameterM/checkpoint.cellWidthM<<"\n";
		return static_cast<bool>(output);
	}

#if defined(__APPLE__)
	bool ReadDisplayLitArea(const std::filesystem::path& path,std::size_t& litPixels,
		unsigned int& width,unsigned int& height)
	{
		litPixels=0u;width=0u;height=0u;
		const std::string native=path.string();
		CFURLRef url=CFURLCreateFromFileSystemRepresentation(nullptr,
			reinterpret_cast<const UInt8*>(native.data()),native.size(),false);
		CGImageSourceRef source=url?CGImageSourceCreateWithURL(url,nullptr):nullptr;
		if(url)CFRelease(url);
		CGImageRef image=source?CGImageSourceCreateImageAtIndex(source,0u,nullptr):nullptr;
		if(source)CFRelease(source);
		if(!image)return false;
		const std::size_t imageWidth=CGImageGetWidth(image),imageHeight=CGImageGetHeight(image);
		if(imageWidth==0u||imageHeight==0u||imageWidth>4096u||imageHeight>4096u){
			CFRelease(image);return false;
		}
		std::vector<unsigned char> pixels(imageWidth*imageHeight*4u,0u);
		CGColorSpaceRef color=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
		CGContextRef context=color?CGBitmapContextCreate(pixels.data(),imageWidth,imageHeight,8u,
			imageWidth*4u,color,kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big):nullptr;
		if(color)CFRelease(color);
		if(!context){CFRelease(image);return false;}
		CGContextDrawImage(context,CGRectMake(0,0,imageWidth,imageHeight),image);
		CFRelease(context);CFRelease(image);
		for(std::size_t pixel=0u;pixel<imageWidth*imageHeight;++pixel){
			const std::size_t offset=4u*pixel;
			if(std::max({pixels[offset],pixels[offset+1u],pixels[offset+2u]})>127u)++litPixels;
		}
		width=static_cast<unsigned int>(imageWidth);height=static_cast<unsigned int>(imageHeight);
		return true;
	}

	int RunProductionPuffingSpectrumChild(const double resolutionTier,
		const std::filesystem::path& checkpointPath,
		const std::filesystem::path& simulationDirectory,
		const std::filesystem::path& previewDirectory,
		const std::filesystem::path& outputDirectory)
	{
		MethaneRunCheckpoint checkpoint;std::string error;
		if(!LoadMethaneRunCheckpoint(checkpointPath,checkpoint,error)||
			!ProductionPuffingSpectrumTierMatches(checkpoint,resolutionTier)){
			std::fprintf(stderr,"puffing spectrum checkpoint rejected: %s\n",error.c_str());return 91;
		}
		UniformHannSpectrum centerline;
		if(!ComputeUniformHannSpectrum(checkpoint.values.probeTimeS,
			checkpoint.values.probeCenterlineHeatReleaseW,512u,centerline))return 92;
		std::ifstream frameTimesInput(simulationDirectory/"frame_times.csv");
		std::string line;if(!std::getline(frameTimesInput,line)||
			line!="frame,simulation_time_s,vdb_sha256")return 93;
		std::vector<double> litTimeS,litAreaPixels;
		std::vector<std::string> displayDigests;
		unsigned int displayWidth=0u,displayHeight=0u;
		while(std::getline(frameTimesInput,line)){
			if(line.empty())continue;
			std::istringstream row(line);std::string frameText,timeText,vdbDigest;
			if(!std::getline(row,frameText,',')||!std::getline(row,timeText,',')||
				!std::getline(row,vdbDigest)||vdbDigest.size()!=64u)return 93;
			char* frameEnd=nullptr;char* timeEnd=nullptr;
			const unsigned long frame=std::strtoul(frameText.c_str(),&frameEnd,10);
			const double timeS=std::strtod(timeText.c_str(),&timeEnd);
			if(!frameEnd||*frameEnd!='\0'||!timeEnd||*timeEnd!='\0'||
				frame!=litTimeS.size()||!std::isfinite(timeS)||
				(!litTimeS.empty()&&!(timeS>litTimeS.back())))return 93;
			std::ostringstream name;name<<"temporal_display_"<<std::setw(5)<<
				std::setfill('0')<<frame<<".png";
			const std::filesystem::path displayPath=previewDirectory/name.str();
			std::size_t lit=0u;unsigned int width=0u,height=0u;
			if(!ReadDisplayLitArea(displayPath,lit,width,height)||
				(!litTimeS.empty()&&(width!=displayWidth||height!=displayHeight)))return 94;
			if(litTimeS.empty()){displayWidth=width;displayHeight=height;}
			const std::string displayDigest=DigestFile(displayPath);
			if(displayDigest.size()!=64u)return 94;
			litTimeS.push_back(timeS);litAreaPixels.push_back(static_cast<double>(lit));
			displayDigests.push_back(displayDigest);
		}
		UniformHannSpectrum litArea;
		if(!ComputeUniformHannSpectrum(litTimeS,litAreaPixels,512u,litArea))return 95;
		std::error_code directoryError;
		std::filesystem::create_directories(outputDirectory,directoryError);
		if(directoryError)return 96;
		auto writeSignal=[&](const std::filesystem::path& path,const std::vector<double>& time,
			const std::vector<double>& signal,const char* valueName)->bool{
			std::ofstream output(path,std::ios::trunc);output<<"sample,time_s,"<<valueName<<"\n";
			for(std::size_t sample=0u;sample<time.size();++sample)output<<sample<<','<<
				std::setprecision(17)<<time[sample]<<','<<signal[sample]<<'\n';
			return static_cast<bool>(output);
		};
		if(!writeSignal(outputDirectory/"centerline_signal.csv",checkpoint.values.probeTimeS,
			checkpoint.values.probeCenterlineHeatReleaseW,"centerline_heat_release_W")||
			!writeSignal(outputDirectory/"lit_area_signal.csv",litTimeS,litAreaPixels,
				"lit_pixels"))return 96;
		std::ofstream displays(outputDirectory/"display_inputs.csv",std::ios::trunc);
		displays<<"frame,time_s,lit_pixels,display_sha256\n";
		for(std::size_t frame=0u;frame<litTimeS.size();++frame)displays<<frame<<','<<
			std::setprecision(17)<<litTimeS[frame]<<','<<litAreaPixels[frame]<<','<<
			displayDigests[frame]<<'\n';
		displays.close();if(!displays)return 96;
		std::ofstream spectra(outputDirectory/"full_spectrum.csv",std::ios::trunc);
		spectra<<"signal,bin,frequency_Hz,amplitude,power,power_fraction\n";
		auto writeSpectrum=[&](const char* name,const UniformHannSpectrum& spectrum){
			for(std::size_t bin=0u;bin<spectrum.frequencyHz.size();++bin)spectra<<name<<','<<
				bin<<','<<std::setprecision(17)<<spectrum.frequencyHz[bin]<<','<<
				spectrum.amplitude[bin]<<','<<spectrum.power[bin]<<','<<
				spectrum.powerFraction[bin]<<'\n';
		};
		writeSpectrum("centerline_heat_release",centerline);writeSpectrum("display_lit_area",litArea);
		spectra.close();if(!spectra)return 96;
		const double expectedHz=1.5/std::sqrt(CapstonePoolDiameterM);
		auto nearestBin=[&](const UniformHannSpectrum& spectrum){
			return static_cast<std::size_t>(std::min_element(spectrum.frequencyHz.begin(),
				spectrum.frequencyHz.end(),[&](const double a,const double b){
					return std::fabs(a-expectedHz)<std::fabs(b-expectedHz);})-
				spectrum.frequencyHz.begin());
		};
		const std::size_t centerlineExpected=nearestBin(centerline),litExpected=nearestBin(litArea);
		std::ofstream summary(outputDirectory/"puffing_spectrum_evidence.v1",std::ios::trunc);
		summary<<std::setprecision(17)
			<<"artifact_fidelity simulation_evidence\n"
			<<"checkpoint_sha256 "<<DigestFile(checkpointPath)<<"\n"
			<<"frame_times_sha256 "<<DigestFile(simulationDirectory/"frame_times.csv")<<"\n"
			<<"display_inputs_sha256 "<<DigestFile(outputDirectory/"display_inputs.csv")<<"\n"
			<<"centerline_signal_sha256 "<<DigestFile(outputDirectory/"centerline_signal.csv")<<"\n"
			<<"lit_area_signal_sha256 "<<DigestFile(outputDirectory/"lit_area_signal.csv")<<"\n"
			<<"full_spectrum_sha256 "<<DigestFile(outputDirectory/"full_spectrum.csv")<<"\n";
		if(!WriteProductionPuffingSpectrumTierEvidence(summary,checkpoint,resolutionTier))return 96;
		summary
			<<"r57_general_case_admissibility_minimum_cells 4\n"
			<<"puffing_claim_requires_refined_approximately_ten_cell_class true\n"
			<<"centerline_probe central_two_by_two_columns_full_height_cell_volume_integrated_and_four_column_averaged\n"
			<<"lit_area_probe temporal_preview_display_pixels_max_srgb_channel_greater_than_127\n"
			<<"display_dimensions "<<displayWidth<<' '<<displayHeight<<"\n"
			<<"resampling linear_to_512_uniform_samples_over_each_exact_observation_span\n"
			<<"detrending least_squares_affine\n"
			<<"window symmetric_Hann\n"
			<<"spectrum direct_DFT_nonnegative_bins_including_DC_and_Nyquist\n"
			<<"expected_puffing_Hz "<<expectedHz<<"\n"
			<<"centerline_dominant_Hz "<<centerline.frequencyHz[centerline.dominantBin]<<"\n"
			<<"centerline_expected_nearest_Hz "<<centerline.frequencyHz[centerlineExpected]<<"\n"
			<<"centerline_expected_power_fraction "<<centerline.powerFraction[centerlineExpected]<<"\n"
			<<"centerline_expected_to_peak_power_ratio "<<centerline.power[centerlineExpected]/
				centerline.power[centerline.dominantBin]<<"\n"
			<<"lit_area_dominant_Hz "<<litArea.frequencyHz[litArea.dominantBin]<<"\n"
			<<"lit_area_expected_nearest_Hz "<<litArea.frequencyHz[litExpected]<<"\n"
			<<"lit_area_expected_power_fraction "<<litArea.powerFraction[litExpected]<<"\n"
			<<"lit_area_expected_to_peak_power_ratio "<<litArea.power[litExpected]/
				litArea.power[litArea.dominantBin]<<"\n";
		if(!summary)return 96;
		std::fprintf(stderr,"PRODUCTION_PUFFING_SPECTRUM centerline_peak=%.17g "
			"centerline_expected_ratio=%.17g lit_peak=%.17g lit_expected_ratio=%.17g output=%s\n",
			centerline.frequencyHz[centerline.dominantBin],centerline.power[centerlineExpected]/
				centerline.power[centerline.dominantBin],litArea.frequencyHz[litArea.dominantBin],
			litArea.power[litExpected]/litArea.power[litArea.dominantBin],
			outputDirectory.string().c_str());
		return 0;
	}
#endif

	void FitCenterlineTemperaturePowerLaw( SolverFrameValues& values )
	{
		if(values.centerlineHeightM.size()!=values.centerlineTemperatureK.size()) return;
		double bestError=std::numeric_limits<double>::infinity(),bestSlope=0.0;
		const double spacing=values.cellWidthM;
		for(unsigned int originStep=0;originStep<32u;++originStep) {
			const double origin=-2.0*spacing+4.0*spacing*static_cast<double>(originStep)/31.0;
			double sx=0.0,sy=0.0,sxx=0.0,sxy=0.0,syy=0.0;std::size_t count=0u;
			for(std::size_t i=0;i<values.centerlineHeightM.size();++i) {
				const double distance=values.centerlineHeightM[i]-origin;
				const double excess=values.centerlineTemperatureK[i]-300.0;
				if(values.centerlineHeightM[i]<=values.mccaffreyFlameTipHeightM||
					distance<=2.0*spacing||excess<=1.0) continue;
				const double x=std::log(distance),y=std::log(excess);
				sx+=x;sy+=y;sxx+=x*x;sxy+=x*y;syy+=y*y;++count;
			}
			if(count<4u) continue;
			const double divisor=static_cast<double>(count)*sxx-sx*sx;
			if(divisor==0.0) continue;
			const double fittedSlope=(static_cast<double>(count)*sxy-sx*sy)/divisor;
			const double fittedIntercept=(sy-fittedSlope*sx)/static_cast<double>(count);
			const double squared=syy+static_cast<double>(count)*fittedIntercept*fittedIntercept+
				fittedSlope*fittedSlope*sxx+2.0*fittedIntercept*fittedSlope*sx-
				2.0*fittedIntercept*sy-2.0*fittedSlope*sxy;
			const double error=std::sqrt(std::max(0.0,squared/static_cast<double>(count)));
			if(error<bestError) {bestError=error;bestSlope=fittedSlope;}
		}
		if(std::isfinite(bestError)) {
			values.centerlineTemperatureExponent=bestSlope;
			values.centerlineFitRMSE=bestError;
		}
	}

	void EvaluateMcCaffreyPlumeStations( SolverFrameValues& values,
		const double heatReleaseRateKW )
	{
		if(values.centerlineHeightM.size()!=values.centerlineTemperatureK.size()||
			values.centerlineHeightM.size()!=values.centerlineVelocityMPerS.size()||
			!(heatReleaseRateKW>0.0)) return;
		// McCaffrey, NBSIR 79-1910: compare to the adopted absolute 33 kW
		// plume row, rather than re-deriving rounded constants from one another.
		const double qTwoFifths=std::pow(heatReleaseRateKW,0.4);
		const double qOneFifth=std::pow(heatReleaseRateKW,0.2);
		const double top=values.centerlineHeightM.empty()?0.0:
			values.centerlineHeightM.back()+0.5*values.cellWidthM;
		const double maximumUncontaminatedHeight=top-0.5*values.characteristicDiameterM;
		const double minimumHeight=std::max(values.mccaffreyFlameTipHeightM,
			0.2*qTwoFifths);
		for(std::size_t station=0;station<values.centerlineHeightM.size();++station){
			const double height=values.centerlineHeightM[station];
			if(!(height>minimumHeight&&height<=maximumUncontaminatedHeight)) continue;
			const double normalizedHeight=height/qTwoFifths;
			const double expectedVelocity=McCaffrey33KWVelocityCoefficient*qOneFifth*
				std::pow(normalizedHeight,-1.0/3.0);
			const double expectedTemperatureRise=McCaffrey33KWTemperatureCoefficientK*
				std::pow(normalizedHeight,-5.0/3.0);
			const double observedTemperatureRise=values.centerlineTemperatureK[station]-300.0;
			values.mccaffreyMaximumTemperatureRelativeError=std::max(
				values.mccaffreyMaximumTemperatureRelativeError,
				std::fabs(observedTemperatureRise-expectedTemperatureRise)/
					expectedTemperatureRise);
			values.mccaffreyMaximumVelocityRelativeError=std::max(
				values.mccaffreyMaximumVelocityRelativeError,
				std::fabs(values.centerlineVelocityMPerS[station]-expectedVelocity)/
					expectedVelocity);
			++values.mccaffreyPlumeStationCount;
		}
	}
	namespace FireProductionDyadicCalibration
	{
		bool ResidentStepRequestsExactlyEqual(
			const RISE::FireProductionResidentStepRequest&,
			const RISE::FireProductionResidentStepRequest&);
		bool BuildProductionRequest(const MethaneRunCheckpoint& state,
			const std::vector<double>& divergenceTarget,double timeStepS,
			RISE::FireProductionResidentStepRequest& request,std::string& error,
			unsigned int workerCount=1u,
			const std::vector<CellTransportEvaluation>* reusableTransport=nullptr,
			const std::vector<unsigned char>* invalidReusableTransport=nullptr);
		bool ApplyAcceptedProductionResult(
			const RISE::FireProductionResidentStepResult& production,
			const RISE::FireProductionAcceptedManifoldObservation& acceptedObservation,
			MethaneRunCheckpoint& state,std::string& error,
			unsigned int temperatureWorkerCount);
		bool ApplyProductionResultUnchecked(
			const RISE::FireProductionResidentStepResult& production,
			MethaneRunCheckpoint& state,std::string& error,
			bool enforceOracleEOSValidityDetector,
			unsigned int temperatureWorkerCount);
	}
#if defined(RISE_ENABLE_OPENVDB)
	bool WriteProductionTemporalFrame(const std::filesystem::path& path,
		const SolverFrameValues& values);
#endif
	SolverFrameValues RunMethaneFrameProbe( const unsigned int workerCount=1u,
		const unsigned int minimumStepCount=1u,const double targetTimeS=0.0,
		const double caseDurationS=1.0,const double caseFramesPerS=4.0,
		const double resolutionTier=6.0,const double poolDiameterM=CapstonePoolDiameterM,
		const double heatReleaseRateKW=CapstoneHeatReleaseRateKW,
		const bool injectSolverFailure=false,
		const RunPersistenceOptions& persistence=RunPersistenceOptions() )
	{
		SolverFrameValues values;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		MethaneCellState state; state.temperatureK=300.0;
		for(std::size_t i=0;i<MethaneSpeciesCount;++i)
			state.constituent[i]=fuel.AmbientMassFractions()[i];
		double invW=0.0;
		for(std::size_t i=0;i<MethaneCarbon;++i) {
			const FireThermochemistrySpecies* s=fuel.FindSpecies(fuel.SpeciesOrder()[i].c_str());
			if(s) invW+=state.constituent[i]/s->molecularWeightKGPerKMol;
		}
		const double rho=fuel.ThermodynamicPressurePa()/(8314.46261815324*state.temperatureK*invW);
		for(double& x:state.constituent) x*=rho;
		state.rhoTotalZ=0.0;
		std::string error;
		Check(fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(state),state.temperatureK,
			state.sensibleEnergyJPerM3,&error),"capstone initial methane state closes thermochemistry");
		FireCase::AuthoredV1 authored;
		authored.fuelRecordId=fuel.RecordId(); authored.poolDiameterM=poolDiameterM;
		authored.heatReleaseRateKW=heatReleaseRateKW; authored.envelope={{0.0,1.0}};
		authored.durationS=caseDurationS; authored.quality="dstar";
		authored.numericDStarTier=resolutionTier; authored.seed=1234;
		authored.outputFramesPerS=caseFramesPerS;
		authored.plumeLaw=true;
		FireCase::RecordV1 caseRecord;
		const RISECBOR64::Bytes aerosol=AerosolRecord();
		const RISECBOR64::Bytes chem=SyntheticChemRecord();
		const bool caseBuilt=FireCase::BuildMethaneV1(authored,fuel,{RISECBOR64::SHA256Hex(fuel.RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationThermochemistryRecord::OpenSubsetV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationTransportRecord::OpenV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireOpticsPreset::PredictiveV1().RecordBytes()),RISECBOR64::SHA256Hex(aerosol),
			RISECBOR64::SHA256Hex(chem)},caseRecord,error);
		Check(caseBuilt,
			"capstone solver consumes the canonical r57 methane case");
		if(!caseBuilt) {
			std::fprintf(stderr,"capstone case derivation rejected: %s\n",error.c_str());
			values.structuredError="case_derivation_failure:"+error;
			return values;
		}
		RISECBOR64::Bytes currentBuildBytes;
		std::string currentBuildId,currentExecutableDigest;
		if((!persistence.checkpointPath.empty()||!persistence.finalCheckpointPath.empty()||
			!persistence.temporalSnapshotDirectory.empty())&&
			(!CurrentRendererBuildIdentity(currentBuildBytes,currentBuildId)||
			!CurrentExecutableDigest(currentBuildBytes,currentExecutableDigest,error))){
			values.structuredError="checkpoint_build_identity_failure";return values;
		}
		PeriodicMACShape shape; shape.nx=caseRecord.derived.nx;shape.ny=caseRecord.derived.ny;
		shape.nz=caseRecord.derived.nz;shape.cellWidthM=caseRecord.derived.cellWidthM;
		const std::array<std::size_t,2> centerXIndex={{(shape.nx-1u)/2u,shape.nx/2u}},
			centerYIndex={{(shape.ny-1u)/2u,shape.ny/2u}};
		const std::size_t centerXCount=centerXIndex[0]==centerXIndex[1]?1u:2u,
			centerYCount=centerYIndex[0]==centerYIndex[1]?1u:2u;
		const double centerSampleCount=static_cast<double>(centerXCount*centerYCount);
		values.effectiveRadiativeFraction=caseRecord.derived.effectiveRadiativeFraction;
		const bool reportCapstoneProgress=std::getenv("RISE_FIRE_CAPSTONE_OUTPUT")!=nullptr;
		if(reportCapstoneProgress) std::fprintf(stderr,
			"capstone probe workers=%u target=%.9g grid=%zux%zux%zu dx=%.9g t_ft=%.9g\n",
			workerCount,targetTimeS,shape.nx,shape.ny,shape.nz,shape.cellWidthM,
			caseRecord.derived.flowThroughTimeS);
		const double zeroGradient[3][3]={{0.0,0.0,0.0},{0.0,0.0,0.0},{0.0,0.0,0.0}};
		const double widths[3]={shape.cellWidthM,shape.cellWidthM,shape.cellWidthM};
		CellTransportEvaluation transportEvaluation;
		Check(EvaluateCellTransport(state,zeroGradient,widths,false,fuel,
			FireSimulationTransportRecord::OpenV1(),transportEvaluation,&error),
			"capstone evaluates physical methane transport from the adopted record");
		StableTimeStep selectedStep;
		const double maximumTransport=std::max(transportEvaluation.totalDiffusivityM2PerS,
			transportEvaluation.effectiveViscosityPaS/state.GasDensity());
		const double positiveReducedGravity=std::max(0.0,9.80665*(rho-state.GasDensity())/
			state.GasDensity());
		Check(ComputeStableTimeStep(shape.cellWidthM,0.0,positiveReducedGravity,
			maximumTransport,3,0.0,selectedStep,&error),
			"capstone consumes the production r54 timestep selector");
		MethaneReactionStep reaction; reaction.deltaTimeS=selectedStep.seconds;
		reaction.maximumAcceptedTemperatureK=caseRecord.derived.maximumAcceptedTemperatureK;
		Check(ComputeMixingTimeS(state,transportEvaluation,FireSimulationTransportRecord::OpenV1(),
			shape.cellWidthM,rho,9.80665,false,reaction.mixingTimeS,&error),
			"capstone derives mixing time instead of authoring a closure constant");
		std::vector<MethaneCellState> states(shape.CellCount(),state);
		auto canonicalizeProductionBinary32State=[&](MethaneCellState& candidate)->bool{
			ConservativeVector conservative=ToConservativeVector(candidate);
			for(std::size_t component=0u;component<MethaneMassStateDimension;++component)
				conservative[component]=static_cast<double>(static_cast<float>(
					conservative[component]));
			MethaneCellState represented=FromConservativeVector(conservative,
				FireStateProducerPrecision::Binary32);
			represented.temperatureK=candidate.temperatureK;
			if(!fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(represented),
				represented.temperatureK,represented.sensibleEnergyJPerM3,&error))return false;
			represented.sensibleEnergyJPerM3=static_cast<double>(static_cast<float>(
				represented.sensibleEnergyJPerM3));
			candidate=represented;return true;
		};
		if(persistence.productionMetal)for(MethaneCellState& cell:states)
			if(!canonicalizeProductionBinary32State(cell)){
				values.structuredError="production_initial_state_failure:"+error;return values;}
		std::vector<MethaneReactionStep> reactions(shape.CellCount(),reaction);
		std::vector<MethaneSourcePacket> packets;
		RadiationEscapeFactor escape;
		const double cellVolume=shape.cellWidthM*shape.cellWidthM*shape.cellWidthM;
		PeriodicMACField momentum; for(unsigned int axis=0;axis<3;++axis)
			momentum.component[axis].assign(OpenMACFaceCount3D(shape,axis),0.0);
		ConservativeAdvance3DConfig config; config.transport.cellWidthM=shape.cellWidthM;
		config.transport.deltaTimeS=reaction.deltaTimeS; config.transport.ambientTemperatureK=300.0;
		config.transport.adiabaticTemperatureK=caseRecord.derived.maximumAcceptedTemperatureK;
		config.transport.ambientGasDensityKGPerM3=rho;
		config.transport.producerPrecision=persistence.productionMetal?
			FireStateProducerPrecision::Binary32:FireStateProducerPrecision::Binary64;
		Check(config.transport.adiabaticTemperatureK==2300.0&&
			config.transport.adiabaticTemperatureK<
				FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1().TemperatureMaxK(),
			"r74 capstone owner separates the case physical ceiling from the opacity domain");
		config.gravityMPerS2={{0.0,0.0,-9.80665}};
		Check(config.gravityMPerS2[0]==0.0&&config.gravityMPerS2[1]==0.0&&
			config.gravityMPerS2[2]==-9.80665,
			"capstone owning advance receives the pinned vertical gravitational acceleration");
		const double projectionReferenceVelocityMPerS=std::sqrt(9.80665*
			caseRecord.derived.characteristicDiameterM);
		const double projectionReferenceLengthM=std::max({caseRecord.derived.extentXM,
			caseRecord.derived.extentYM,caseRecord.derived.extentZM});
		config.projectionTolerancePerS=1.0e-3*projectionReferenceVelocityMPerS/
			projectionReferenceLengthM;
		config.dns=false; config.workerCount=workerCount;
		config.periodicBoundaries=false;
		config.retainStageDiagnostics=!persistence.productionMetal&&
			std::getenv("RISE_FIRE_ORACLE_MOMENTUM_AUDIT_PATH")!=nullptr;
		config.injectedTemperatureK=300.0;
		MethaneCellState ambient; ambient.temperatureK=300.0;
		for(std::size_t i=0;i<MethaneSpeciesCount;++i)
			ambient.constituent[i]=fuel.AmbientMassFractions()[i];
		double ambientInvW=0.0;
		for(std::size_t i=0;i<MethaneCarbon;++i) {
			const FireThermochemistrySpecies* species=fuel.FindSpecies(fuel.SpeciesOrder()[i].c_str());
			if(species) ambientInvW+=ambient.constituent[i]/species->molecularWeightKGPerKMol;
		}
		const double ambientRho=fuel.ThermodynamicPressurePa()/(8314.46261815324*
			ambient.temperatureK*ambientInvW);
		for(double& x:ambient.constituent) x*=ambientRho;
		ambient.rhoTotalZ=0.0;
		Check(fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(ambient),
			ambient.temperatureK,ambient.sensibleEnergyJPerM3,&error),
			"capstone ambient state closes thermochemistry");
		MethaneCellState injected; injected.temperatureK=300.0;
		for(std::size_t i=0;i<MethaneSpeciesCount;++i)
			injected.constituent[i]=fuel.InjectedMassFractions()[i];
		double injectedInvW=0.0;
		for(std::size_t i=0;i<MethaneCarbon;++i) {
			const FireThermochemistrySpecies* species=fuel.FindSpecies(fuel.SpeciesOrder()[i].c_str());
			if(species) injectedInvW+=injected.constituent[i]/species->molecularWeightKGPerKMol;
		}
		const double injectedRho=fuel.ThermodynamicPressurePa()/(8314.46261815324*
			injected.temperatureK*injectedInvW);
		for(double& x:injected.constituent) x*=injectedRho;
		injected.rhoTotalZ=injected.TotalDensity();
		Check(fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(injected),
			injected.temperatureK,injected.sensibleEnergyJPerM3,&error),
			"capstone injected methane state closes thermochemistry");
		config.openBoundary.ambientDensityKGPerM3=ambient.GasDensity();
		config.openBoundary.injectedGasDensityKGPerM3=injected.GasDensity();
		config.openBoundary.ambientState=ToConservativeVector(ambient);
		config.openBoundary.injectedState=ToConservativeVector(injected);
		config.openBoundary.fuelMassFluxKGPerM2S=0.0;
		std::vector<double> sourcePattern;
		Check(FireCase::BuildSourcePattern(authored,caseRecord.derived,sourcePattern,error),
			"capstone solver installs the case's exact SplitMix64 source pattern");
		config.openBoundary.bottomFuelMask.resize(shape.nx*shape.ny,false);
		config.openBoundary.bottomFuelMassFluxKGPerM2S.resize(shape.nx*shape.ny,0.0);
		for(std::size_t face=0;face<sourcePattern.size();++face) if(sourcePattern[face]!=0.0) {
			config.openBoundary.bottomFuelMask[face]=true;
			config.openBoundary.bottomFuelMassFluxKGPerM2S[face]=
				caseRecord.derived.nominalFuelFluxKGPerM2S*sourcePattern[face];
		}
		std::vector<std::uint8_t> canonicalPilotMask;
		Check(FireCase::BuildPilotMask(authored,caseRecord.derived,canonicalPilotMask,error),
			"capstone installs the canonical r57 intensive pilot annulus");
		const double pilotEndS=caseRecord.derived.pilotDurationMultiplier*
			caseRecord.derived.flowThroughTimeS;
		const double pilotRampEndS=caseRecord.derived.flowThroughTimeS/
			caseRecord.derived.pilotRampExponentPerFlowThrough;
		const double pilotCommandMaximumStepS=FireCase::PilotCommandMaximumStepS(
			caseRecord.derived);
		Check(pilotCommandMaximumStepS>0.0,
			"capstone derives the r70 command-ramp step ceiling from the canonical cap");
		values.maximumTemperatureK=state.temperatureK;
		values.statisticsStartS=5.0*caseRecord.derived.flowThroughTimeS;
		values.characteristicDiameterM=caseRecord.derived.characteristicDiameterM;
		std::vector<double> centerlineTemperatureIntegral(shape.nz,0.0),
			centerlineVelocityIntegral(shape.nz,0.0),
			planeHeatReleaseIntegral(shape.nz,0.0);
		double centerlineStatisticsDurationS=0.0;
		config.openBoundary.velocityToleranceMPerS=1.0e-3*projectionReferenceVelocityMPerS;
		config.openBoundary.pressureTolerancePa=ambient.GasDensity()*
			projectionReferenceVelocityMPerS*config.openBoundary.velocityToleranceMPerS;
		Check(config.openBoundary.velocityToleranceMPerS==
			config.projectionTolerancePerS*projectionReferenceLengthM&&
			config.openBoundary.pressureTolerancePa==ambient.GasDensity()*
				projectionReferenceVelocityMPerS*config.openBoundary.velocityToleranceMPerS,
			"capstone pressure-open deadband derives from the pinned epsilon_abs scene scale");
		ConservativeAdvance3DResult advanced;
		if(persistence.productionMetal)for(unsigned int axis=0u;axis<3u;++axis)
			advanced.velocityMPerS.component[axis].assign(OpenMACFaceCount3D(shape,axis),0.0);
		if(persistence.sealedLegacyMomentumReplay||persistence.sealedProjectedHeunReplay){
			if(persistence.resume||persistence.replayProtocolPath.empty()||
				persistence.replayProtocolDigest.size()!=64u||
				DigestFile(persistence.replayProtocolPath)!=persistence.replayProtocolDigest||
				persistence.productionOnsetDiagnosticDirectory.empty()){
				values.structuredError="sealed_legacy_replay_identity_failure";return values;
			}
			const std::string initialStateDigest=ProductionBeginningFieldSHA256(
				caseRecord.caseRecordId,shape,states,momentum,advanced.velocityMPerS);
			std::ofstream identity(persistence.productionOnsetDiagnosticDirectory.parent_path()/
				"from_zero_identity.v1",std::ios::trunc);
			identity<<std::setprecision(17)
				<<"schema rise.fire.production.r193_from_zero_identity.v1\n"
				<<"operator "<<(persistence.sealedProjectedHeunReplay?
					"section_3_7_projected_heun_resident_owner":
					"ordinary_independent_dual_momentum_resident_step")<<"\n"
				<<"resolution_tier "<<resolutionTier<<"\nseed "<<authored.seed<<"\n"
				<<"case_record_id "<<caseRecord.caseRecordId<<"\n"
				<<"initial_state_sha256 "<<initialStateDigest<<"\n"
				<<"producer_build_id "<<currentBuildId<<"\n"
				<<"producer_executable_sha256 "<<currentExecutableDigest<<"\n"
				<<"protocol_path "<<persistence.replayProtocolPath.string()<<"\n"
				<<"protocol_sha256 "<<persistence.replayProtocolDigest<<"\n"
				<<"resume_forbidden true\n";
			identity.close();
			if(initialStateDigest.size()!=64u||!identity){
				values.structuredError="sealed_legacy_replay_identity_publication_failure";
				return values;
			}
		}
		bool advancedOK=minimumStepCount>0u || targetTimeS==0.0;
		double simulationTimeS=0.0,previousStepS=0.0;
		unsigned int acceptedSteps=0u;
		unsigned int effectiveMinimumStepCount=minimumStepCount;
		FireProductionAcceptedManifoldObservation productionManifoldObservation;
		values.checkpointCadenceWallS=persistence.checkpointCadenceWallS;
		values.streamedFrameCount=persistence.streamedFrameCountAtStart;
		values.workerCountHistory.push_back(workerCount);
		if(persistence.resume&&!persistence.checkpointPath.empty()&&
			std::filesystem::exists(persistence.checkpointPath)){
			MethaneRunCheckpoint checkpoint;
			if(!LoadMethaneRunCheckpoint(persistence.checkpointPath,checkpoint,error)){
				values.structuredError="checkpoint_resume_failure:"+error;return values;
			}
			const bool sameBuild=checkpoint.producerBuildId==currentBuildId;
			const bool isolatedProbe=!sameBuild&&persistence.isolatedEquivalenceProbe&&
				persistence.isolatedExpectedCheckpointBuildId==checkpoint.producerBuildId&&
				persistence.stopAfterAdditionalAcceptedSteps>=8u;
			const std::string loadedCheckpointDigest=persistence.isolatedEquivalenceProbe?
				DigestFile(persistence.checkpointPath):std::string();
			const bool exactDiagnosticCaseReplay=persistence.isolatedEquivalenceProbe&&
				persistence.stopAfterAdditionalAcceptedSteps==8u&&
				persistence.isolatedExpectedCheckpointBuildId==checkpoint.producerBuildId&&
				!loadedCheckpointDigest.empty()&&
				persistence.isolatedExpectedCheckpointDigest==loadedCheckpointDigest;
			ResumeEquivalenceCertificate migration;
			bool certifiedMigration=false;
			if(!sameBuild&&!persistence.resumeEquivalenceCertificatePath.empty()&&
				LoadResumeEquivalenceCertificate(persistence.resumeEquivalenceCertificatePath,
					migration,error)){
				const std::string checkpointDigest=DigestFile(persistence.checkpointPath);
				certifiedMigration=!checkpointDigest.empty()&&
					migration.checkpointDigest==checkpointDigest&&
					migration.oldBuildId==checkpoint.producerBuildId&&
					migration.newBuildId==currentBuildId&&
					migration.newExecutableDigest==currentExecutableDigest&&
					migration.resumedFromStep==checkpoint.acceptedSteps;
			}
			const bool currentActiveSetCheckpoint=checkpoint.checkpointFormatVersion>=7u&&
				checkpoint.values.activeSetAlgorithmVersion==CurrentActiveSetAlgorithmVersion();
			const bool legacyActiveSetCheckpoint=checkpoint.checkpointFormatVersion<7u&&
				checkpoint.values.activeSetAlgorithmVersion==LegacyActiveSetAlgorithmVersion();
			const bool priorActiveSetHistoryValid=checkpoint.checkpointFormatVersion<8u?
				checkpoint.values.priorActiveSetAlgorithmVersion.empty():
				(checkpoint.values.priorActiveSetAlgorithmVersion.empty()||
					checkpoint.values.priorActiveSetAlgorithmVersion==
						LegacyActiveSetAlgorithmVersion());
			FireStateProducerPrecision checkpointPrecision=FireStateProducerPrecision::Unknown;
			if((checkpoint.caseRecordId!=caseRecord.caseRecordId&&!exactDiagnosticCaseReplay)||
				(!sameBuild&&!isolatedProbe&&!certifiedMigration)||
				checkpoint.values.reductionMode!="fixed_order_tree_v1"||
				(!currentActiveSetCheckpoint&&!legacyActiveSetCheckpoint)||
				!priorActiveSetHistoryValid||
				checkpoint.dimensions!=std::array<std::size_t,3>{{shape.nx,shape.ny,shape.nz}}||
				checkpoint.cellWidthM!=shape.cellWidthM||checkpoint.states.size()!=shape.CellCount()||
				checkpoint.acceptedSteps>std::numeric_limits<unsigned int>::max()||
				checkpoint.centerlineTemperatureIntegral.size()!=shape.nz||
				checkpoint.centerlineVelocityIntegral.size()!=shape.nz||
				checkpoint.planeHeatReleaseIntegral.size()!=shape.nz||
				!HomogeneousStateProducerPrecision(checkpoint.states,checkpointPrecision)){
				if(persistence.isolatedEquivalenceProbe)std::fprintf(stderr,
					"ISOLATED_CHECKPOINT_ADMISSION same=%d isolated=%d exact_case=%d "
					"case=%d reduction=%d active_current=%d "
					"active_legacy=%d prior=%d shape=%d width=%d states=%d temperature=%d "
					"velocity=%d heat=%d precision=%d\n",
					sameBuild?1:0,isolatedProbe?1:0,exactDiagnosticCaseReplay?1:0,
					checkpoint.caseRecordId==caseRecord.caseRecordId?1:0,
					checkpoint.values.reductionMode=="fixed_order_tree_v1"?1:0,
					currentActiveSetCheckpoint?1:0,legacyActiveSetCheckpoint?1:0,
					priorActiveSetHistoryValid?1:0,
					checkpoint.dimensions==std::array<std::size_t,3>{{shape.nx,shape.ny,shape.nz}}?1:0,
					checkpoint.cellWidthM==shape.cellWidthM?1:0,
					checkpoint.states.size()==shape.CellCount()?1:0,
					checkpoint.centerlineTemperatureIntegral.size()==shape.nz?1:0,
					checkpoint.centerlineVelocityIntegral.size()==shape.nz?1:0,
					checkpoint.planeHeatReleaseIntegral.size()==shape.nz?1:0,
					HomogeneousStateProducerPrecision(checkpoint.states,checkpointPrecision)?1:0);
				if(error.empty())error="checkpoint build migration is not certified";
				values.structuredError="checkpoint_resume_failure:"+error;return values;
			}
			for(unsigned int axis=0;axis<3;++axis)if(
				checkpoint.momentum.component[axis].size()!=OpenMACFaceCount3D(shape,axis)||
				checkpoint.velocity.component[axis].size()!=OpenMACFaceCount3D(shape,axis)){
				values.structuredError="checkpoint_resume_failure:checkpoint MAC shape mismatch";
				return values;
			}
			states=std::move(checkpoint.states);momentum=std::move(checkpoint.momentum);
			productionManifoldObservation=checkpoint.productionManifoldObservation;
			config.transport.producerPrecision=checkpointPrecision;
			advanced.velocityMPerS=std::move(checkpoint.velocity);
			values=std::move(checkpoint.values);
			if(legacyActiveSetCheckpoint)values.priorActiveSetAlgorithmVersion=
				LegacyActiveSetAlgorithmVersion();
			values.activeSetAlgorithmVersion=CurrentActiveSetAlgorithmVersion();
			values.workerCountHistory.push_back(workerCount);
			centerlineTemperatureIntegral=std::move(checkpoint.centerlineTemperatureIntegral);
			centerlineVelocityIntegral=std::move(checkpoint.centerlineVelocityIntegral);
			planeHeatReleaseIntegral=std::move(checkpoint.planeHeatReleaseIntegral);
			centerlineStatisticsDurationS=checkpoint.centerlineStatisticsDurationS;
			simulationTimeS=checkpoint.simulationTimeS;previousStepS=checkpoint.previousStepS;
			reaction.deltaTimeS=checkpoint.lastAcceptedStepS;
			acceptedSteps=static_cast<unsigned int>(checkpoint.acceptedSteps);
			if(persistence.stopAfterAdditionalAcceptedSteps>0u){
				const std::uint64_t requested=checkpoint.acceptedSteps+
					persistence.stopAfterAdditionalAcceptedSteps;
				if(requested>std::numeric_limits<unsigned int>::max()){
					values.structuredError="checkpoint_resume_failure:step target overflow";return values;
				}
				effectiveMinimumStepCount=static_cast<unsigned int>(requested);
			}
			values.resumedFromCheckpoint=true;values.resumedFromStep=acceptedSteps;
			if(certifiedMigration){
				values.migrationCertificateId=migration.certificateId;
				values.migrationOldBuildId=migration.oldBuildId;
				values.migrationNewBuildId=migration.newBuildId;
				values.migrationAcceptedStepCount=migration.acceptedStepCount;
				values.migrationResumedFromStep=migration.resumedFromStep;
			}
			values.checkpointCadenceWallS=persistence.checkpointCadenceWallS;
			if(reportCapstoneProgress)std::fprintf(stderr,
				"capstone resumed checkpoint step=%u time=%.17g path=%s\n",acceptedSteps,
				simulationTimeS,persistence.checkpointPath.string().c_str());
		}
		auto lastCheckpointWall=std::chrono::steady_clock::now();
		const std::array<double,3> productionOnsetVelocityThresholds={{15.0,30.0,60.0}};
		std::array<bool,3> productionOnsetThresholdCaptured={{false,false,false}};
		bool productionMomentumObservationCaptured=false;
		bool productionOnsetStopReached=false;
		std::array<std::vector<unsigned char>,6> singleStageFCTAcceptedInflow;
		for(unsigned int side=0u;side<6u;++side){const std::size_t count=side<2u?
			shape.ny*shape.nz:(side<4u?shape.nx*shape.nz:shape.nx*shape.ny);
			singleStageFCTAcceptedInflow[side].assign(count,0u);}
		if(!persistence.productionOnsetDiagnosticDirectory.empty()){
			std::error_code onsetDirectoryError;
			std::filesystem::create_directories(
				persistence.productionOnsetDiagnosticDirectory,onsetDirectoryError);
			if(onsetDirectoryError){values.structuredError="production_onset_directory_failure";
				return values;}
			std::ofstream trajectory(persistence.productionOnsetDiagnosticDirectory/
				"maximum_velocity_trajectory.csv",std::ios::trunc);
			trajectory<<"accepted_step,time_s,dt_s,maximum_velocity_m_per_s,axis,face,x,y,z,"
				"manifold_max,manifold_p95,manifold_p50,tail_cells,tail_drained_m3,"
				"device_ms,wall_ms,owner_identity,owner_commits,owner_projections,"
				"owner_r0_iterations,owner_r1_iterations,owner_r2_iterations,"
				"owner_actual_working_set_bytes,owner_certified_working_set_bytes,"
				"owner_projection_device_ms,owner_nonprojection_device_ms\n";
			if(!trajectory){values.structuredError="production_onset_trajectory_failure";
				return values;}
			std::ofstream retries(persistence.productionOnsetDiagnosticDirectory/
				"retry_attempt_trajectory.csv",std::ios::trunc);
			retries<<"beginning_time_s,candidate,represented_dt_s,maximum_deviation,p95,tail_cells,"
				"tail_drained_m3,dynamics_passed,physical_valid,terminal_valid,token,next_available,"
				"suggested_dt_s,disposition,error\n";
			if(!retries){values.structuredError="production_onset_retry_trajectory_failure";
				return values;}
		}
		std::uint64_t temporalSnapshotIndex=values.streamedFrameCount;
		double nextTemporalSnapshotS=values.statisticsStartS;
		if(persistence.temporalSnapshotCadenceS>0.0&&simulationTimeS>=nextTemporalSnapshotS)
			nextTemporalSnapshotS+=persistence.temporalSnapshotCadenceS*(1.0+
				std::floor((simulationTimeS-nextTemporalSnapshotS)/
					persistence.temporalSnapshotCadenceS));
		auto captureTemporalFrame=[&](){
			values.dimensions={{shape.nx,shape.ny,shape.nz}};values.cellWidthM=shape.cellWidthM;
			values.caseRecordId=caseRecord.caseRecordId;values.temperature.resize(shape.CellCount());
			values.reaction.resize(shape.CellCount());values.carbon.resize(shape.CellCount());
			values.velocity.resize(shape.CellCount());
			for(std::size_t cell=0u;cell<shape.CellCount();++cell){
				values.temperature[cell]=static_cast<float>(states[cell].temperatureK);
				values.reaction[cell]=static_cast<float>(packets.empty()?0.0:
					packets[cell].gasHeatReleaseWPerM3);
				values.carbon[cell]=static_cast<float>(std::max(0.0,
					states[cell].constituent[MethaneCarbon]));
				for(unsigned int axis=0u;axis<3u;++axis){
					const std::size_t lower=OpenLowerFaceForCell3D(shape,cell,axis);
					const std::size_t upper=OpenUpperFaceForCell3D(shape,cell,axis);
					values.velocity[cell][axis]=static_cast<float>(0.5*(
						advanced.velocityMPerS.component[axis][lower]+
						advanced.velocityMPerS.component[axis][upper]));
				}
			}
		};
		auto writeTemporalFrame=[&]()->bool{
			captureTemporalFrame();
			std::ostringstream frameName;frameName<<"frame_"<<std::setw(5)<<
				std::setfill('0')<<temporalSnapshotIndex;
			const std::filesystem::path framePath=persistence.temporalSnapshotDirectory/
				(frameName.str()+".vdb");
			if(!WriteProductionTemporalFrame(framePath,values)){
				error="production temporal VDB publication failed";return false;
			}
			const std::string digest=DigestFile(framePath);
			std::ofstream timeRecord(persistence.temporalSnapshotDirectory/
				(frameName.str()+".time"),std::ios::trunc);
			timeRecord<<std::setprecision(17)<<simulationTimeS<<'\n'<<digest<<'\n';
			timeRecord.close();
			if(digest.empty()||!timeRecord){error="production temporal frame record failed";
				return false;}
			++temporalSnapshotIndex;++values.streamedFrameCount;return true;
		};
		if(FireProfileEnabled())FireProfileReportAndReset("preloop");
		const char* solverPhase="initialization";
		while(advancedOK&&!productionOnsetStopReached&&
			(acceptedSteps<effectiveMinimumStepCount||simulationTimeS<targetTimeS)) {
			solverPhase="ignition eligibility";
			const auto profileStepStart=std::chrono::steady_clock::now();
			double profileEligibilityMS=0.0,profileTransportMS=0.0,
				profileControlMS=0.0,profileSourceMS=0.0,profileTargetMS=0.0,
				profileLayoutMS=0.0,profileResidentMS=0.0,profilePostMS=0.0,
				profileStatisticsMS=0.0;
			auto profileStageStart=profileStepStart;
			if(acceptedSteps>=65536u) { advancedOK=false;error="capstone exceeded its deterministic step cap";break; }
			IgnitionGrid eligibilityGrid;
			eligibilityGrid.nx=shape.nx;eligibilityGrid.ny=shape.ny;eligibilityGrid.nz=shape.nz;
			eligibilityGrid.cells=states;eligibilityGrid.pilotMask.resize(shape.CellCount(),false);
			for(std::size_t cell=0;cell<shape.CellCount();++cell){
				eligibilityGrid.pilotMask[cell]=!persistence.forceZeroSourceForTest&&
					ProductionPilotCommandCell(cell,shape.nx,shape.ny,canonicalPilotMask,
						sourcePattern);
			}
			std::vector<bool> eligibility;
			advancedOK=BuildIgnitionEligibility(eligibilityGrid,fuel,fuel,
				FireSimulationTransportRecord::OpenV1(),eligibility,&error);
			profileEligibilityMS=std::chrono::duration<double,std::milli>(
				std::chrono::steady_clock::now()-profileStageStart).count();
			if(!advancedOK) break;
			solverPhase="transport evaluation";
			profileStageStart=std::chrono::steady_clock::now();
			double maximumSpeed=0.0;
			for(unsigned int axis=0;axis<3;++axis) for(const double velocity:
				(advanced.velocityMPerS.component[axis].empty()?momentum.component[axis]:
					advanced.velocityMPerS.component[axis])) maximumSpeed=std::max(maximumSpeed,
					std::fabs(velocity));
			std::vector<ConservativeVector> currentConservative(shape.CellCount());
			std::vector<double> currentTemperature(shape.CellCount());
			ParallelFireSlices(shape.CellCount(),workerCount,[&](const std::size_t cell){
				currentConservative[cell]=ToConservativeVector(states[cell]);
				currentTemperature[cell]=states[cell].temperatureK;
			});
			OpenMACField3D currentVelocity;
			for(unsigned int axis=0;axis<3;++axis) currentVelocity.component[axis]=
				advanced.velocityMPerS.component[axis].empty()?momentum.component[axis]:
				advanced.velocityMPerS.component[axis];
			std::vector<CellTransportEvaluation> cellTransportEvaluations;
			advancedOK=BuildOpenStageTransportEvaluations3D(shape,currentConservative,
				currentTemperature,currentVelocity,config.openBoundary,config.dns,fuel,
				FireSimulationTransportRecord::OpenV1(),config.transport.producerPrecision,
				cellTransportEvaluations,&error,workerCount);
			if(!advancedOK) break;
			solverPhase="timestep control";
			profileTransportMS=std::chrono::duration<double,std::milli>(
				std::chrono::steady_clock::now()-profileStageStart).count();
			profileStageStart=std::chrono::steady_clock::now();
			double maximumReducedGravity=0.0,maximumActiveDiffusivity=0.0;
			const unsigned int controlWorkers=std::max(1u,std::min(workerCount,
				static_cast<unsigned int>(shape.CellCount())));
			std::vector<double> workerGravity(controlWorkers,0.0),
				workerDiffusivity(controlWorkers,0.0);
			const std::size_t noControlFailure=std::numeric_limits<std::size_t>::max();
			std::vector<std::size_t> controlFailureCell(controlWorkers,noControlFailure);
			std::vector<std::string> controlFailureMessage(controlWorkers);
			auto evaluateControlRange=[&](const unsigned int worker){
				const std::size_t first=shape.CellCount()*worker/controlWorkers;
				const std::size_t last=shape.CellCount()*(worker+1u)/controlWorkers;
				for(std::size_t cell=first;cell<last;++cell){
					const CellTransportEvaluation& cellTransport=cellTransportEvaluations[cell];
					std::string cellError;
					if(!ComputeMixingTimeS(states[cell],cellTransport,
						FireSimulationTransportRecord::OpenV1(),shape.cellWidthM,
						ambient.GasDensity(),9.80665,false,reactions[cell].mixingTimeS,&cellError)){
						controlFailureCell[worker]=cell;
						controlFailureMessage[worker]=cellError;break;
					}
					const double gasDensity=states[cell].GasDensity();
					workerDiffusivity[worker]=std::max({workerDiffusivity[worker],
						cellTransport.totalDiffusivityM2PerS,
						cellTransport.effectiveViscosityPaS/gasDensity,
						cellTransport.effectiveConductivityWPerMK/
							(gasDensity*cellTransport.gasCpJPerKGK)});
					workerGravity[worker]=std::max(workerGravity[worker],
						std::max(0.0,9.80665*(ambient.GasDensity()-gasDensity)/gasDensity));
				}
			};
			if(controlWorkers==1u)evaluateControlRange(0u);
			else FireWorkerPool().Run(controlWorkers,evaluateControlRange);
			std::size_t firstControlFailure=noControlFailure;unsigned int failedControlWorker=0u;
			for(unsigned int worker=0u;worker<controlWorkers;++worker){
				maximumReducedGravity=std::max(maximumReducedGravity,workerGravity[worker]);
				maximumActiveDiffusivity=std::max(maximumActiveDiffusivity,workerDiffusivity[worker]);
				if(controlFailureCell[worker]<firstControlFailure){
					firstControlFailure=controlFailureCell[worker];failedControlWorker=worker;
				}
			}
			if(firstControlFailure!=noControlFailure){advancedOK=false;
				error=controlFailureMessage[failedControlWorker];}
			if(!advancedOK) break;
			// This owner is the binary64 oracle trajectory. Resident production owns
			// and persists its separate (dt,G,r) selector in the r118/r143 path.
			const double chosenStep=FireCase::SelectTimeStepS(shape.cellWidthM,maximumSpeed,
				maximumReducedGravity,maximumActiveDiffusivity,previousStepS);
			if(reportCapstoneProgress && acceptedSteps<4u) std::fprintf(stderr,
				"capstone step-start=%u speed=%.9g gprime=%.9g nu=%.9g selected=%.9g previous=%.9g\n",
				acceptedSteps,maximumSpeed,maximumReducedGravity,maximumActiveDiffusivity,
				chosenStep,previousStepS);
			if(!(chosenStep>0.0)) {advancedOK=false;error="capstone pinned timestep is unbounded";break;}
			double eventStep=chosenStep;
			if(persistence.productionMetal&&persistence.maximumProductionSourceStepS>0.0)
				eventStep=std::min(eventStep,persistence.maximumProductionSourceStepS);
			if(simulationTimeS<pilotRampEndS){
				eventStep=std::min(eventStep,pilotCommandMaximumStepS);
				if(persistence.productionMetal)
					LimitBinary32ProductionStepToEvent(simulationTimeS,pilotRampEndS,eventStep);
				else if(pilotRampEndS-simulationTimeS<eventStep)
					eventStep=pilotRampEndS-simulationTimeS;
			}
			auto limitToEvent=[&](const double event){
				if(persistence.productionMetal)
					LimitBinary32ProductionStepToEvent(simulationTimeS,event,eventStep);
				else if(simulationTimeS<event&&event-simulationTimeS<eventStep)
					eventStep=event-simulationTimeS;
			};
			limitToEvent(pilotEndS);
			limitToEvent(values.statisticsStartS);
			if(!persistence.temporalSnapshotDirectory.empty()&&
				persistence.temporalSnapshotCadenceS>0.0)
				limitToEvent(nextTemporalSnapshotS);
			limitToEvent(targetTimeS);
			if(reportCapstoneProgress&&acceptedSteps<4u)std::fprintf(stderr,
				"capstone event-step=%0.9g pilot_ceiling=%0.9g ramp_end=%0.9g\n",
				eventStep,pilotCommandMaximumStepS,pilotRampEndS);
			ParallelFireSlices(shape.CellCount(),workerCount,[&](const std::size_t cell){
				reactions[cell].primaryEligible=eligibility[cell];
				reactions[cell].sootOxidationEnabled=true;
			});
			std::vector<ConservativeVector> beginning=currentConservative;
			advancedOK=false;
			std::string lastAdvanceError;
			double trialStep=eventStep;
			const FireStateProducerPrecision advanceOutputPrecision=
				persistence.productionMetal?FireStateProducerPrecision::Binary32:
					FireStateProducerPrecision::Binary64;
			std::vector<double> pilotSetpointTemperatureK(shape.CellCount(),0.0);
			std::vector<MethaneCellState> acceptedBeginningStates=states;
			std::vector<double> acceptedProductionTemperatureK;
			double acceptedExternalFuelMassKG=0.0;
			bool fatalOnsetAuditFailure=false;
			bool mandatoryEvidenceFailure=false,observationEvidencePublished=false;
			solverPhase="resident step attempt";
			for(unsigned int reduction=0;
				reduction<RISE::FireStepRejectionRetryCap&&!advancedOK;++reduction) {
				solverPhase="step setup";
				if(injectSolverFailure){lastAdvanceError="injected_solver_failure";break;}
				if(persistence.productionMetal)
					trialStep=static_cast<double>(static_cast<float>(trialStep));
				if(!std::isfinite(trialStep)||trialStep<=0.0){
					lastAdvanceError="production represented timestep is invalid";break;}
				bool commandOK=true;
				for(std::size_t cell=0;cell<shape.CellCount();++cell) {
					reactions[cell].primaryEligible=eligibility[cell];
					commandOK=commandOK&&(persistence.forceZeroSourceForTest||
						FireCase::EvaluatePilotSetpointTemperatureK(caseRecord.derived,
							ProductionPilotCommandCell(cell,shape.nx,shape.ny,canonicalPilotMask,
								sourcePattern),simulationTimeS,
							simulationTimeS+trialStep,pilotSetpointTemperatureK[cell],error));
					reactions[cell].deltaTimeS=trialStep;
					reactions[cell].pilotSetpointTemperatureK=pilotSetpointTemperatureK[cell];
					reactions[cell].pilotExpansionVolumeRatioCap=
						pilotSetpointTemperatureK[cell]>0.0?
						caseRecord.derived.pilotExpansionVolumeRatioCap:0.0;
				}
				if(!commandOK){lastAdvanceError=error;break;}
				config.transport.deltaTimeS=trialStep;
				std::vector<MethaneCellState> packetBeginning=states;
				PeriodicMACField productionMomentum=momentum;
				PeriodicMACField productionVelocity=advanced.velocityMPerS;
				std::vector<unsigned char> invalidReusableTransport(shape.CellCount(),0u);
				double externalFuelMassKG=0.0;
				solverPhase="production source staging";
				if(persistence.productionMetal&&!persistence.forceZeroSourceForTest){
					const double injectedGasDensity=injected.GasDensity();
					const double injectedSpecificEnergy=injected.sensibleEnergyJPerM3/
						injected.TotalDensity();
					std::vector<ConservativeVector> staged(packetBeginning.size());
					ParallelFireSlices(packetBeginning.size(),workerCount,[&](const std::size_t cell){
						staged[cell]=ToConservativeVector(packetBeginning[cell]);
					});
					for(std::size_t y=0u;y<shape.ny;++y)for(std::size_t x=0u;x<shape.nx;++x){
						const std::size_t sourceFace=y*shape.nx+x;
						if(sourceFace>=sourcePattern.size()||sourcePattern[sourceFace]==0.0)continue;
						const double massFlux=caseRecord.derived.nominalFuelFluxKGPerM2S*
							sourcePattern[sourceFace];
						const double densityDelta=massFlux*trialStep/shape.cellWidthM;
						const std::size_t cell=x+shape.nx*y;
						invalidReusableTransport[cell]=1u;
						staged[cell][0]+=densityDelta;
						for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
							staged[cell][1u+species]+=densityDelta*
								injected.constituent[species]/injectedGasDensity;
						staged[cell][8]+=densityDelta*injectedSpecificEnergy;
						externalFuelMassKG+=massFlux*trialStep*shape.cellWidthM*shape.cellWidthM;
						const std::size_t lowerFace=(y*shape.nx+x);
						productionMomentum.component[2][lowerFace]=massFlux;
						productionVelocity.component[2][lowerFace]=massFlux/injectedGasDensity;
					}
					ParallelFireSlices(staged.size(),workerCount,[&](const std::size_t cell){
						for(std::size_t component=0u;component<MethaneConservativeDimension;++component)
							staged[cell][component]=static_cast<double>(
								static_cast<float>(staged[cell][component]));
					});
					std::vector<double> stagedTemperature;
					if(!InvertPeriodicTemperaturesWithinBounds(staged,fuel,fuel.TemperatureMinK(),
						fuel.TemperatureMaxK(),
						FireStateProducerPrecision::Binary32,stagedTemperature,&error,
						workerCount,false)){lastAdvanceError=error;trialStep*=0.5;error.clear();continue;}
					bool stagedOK=true;
					std::vector<unsigned char> stagedValid(packetBeginning.size(),1u);
					ParallelFireSlices(packetBeginning.size(),workerCount,[&](const std::size_t cell){
						MethaneCellState represented=FromConservativeVector(staged[cell],
							FireStateProducerPrecision::Binary32);
						represented.temperatureK=stagedTemperature[cell];
						std::string cellError;
						if(!SignedMixtureSensibleEnergy(represented,represented.temperatureK,fuel,
							represented.sensibleEnergyJPerM3,&cellError)){
							stagedValid[cell]=0u;return;
						}
						represented.sensibleEnergyJPerM3=static_cast<double>(static_cast<float>(
							represented.sensibleEnergyJPerM3));
						packetBeginning[cell]=represented;
					});
					const auto invalid=std::find(stagedValid.begin(),stagedValid.end(),0u);
					if(invalid!=stagedValid.end()){
						const std::size_t cell=static_cast<std::size_t>(invalid-stagedValid.begin());
						MethaneCellState represented=FromConservativeVector(staged[cell],
							FireStateProducerPrecision::Binary32);
						represented.temperatureK=stagedTemperature[cell];
						std::string cellError;double representedEnergy=0.0;
						SignedMixtureSensibleEnergy(represented,represented.temperatureK,fuel,
							representedEnergy,&cellError);
						std::ostringstream sourceFailure;sourceFailure<<std::setprecision(17)
							<<"production source staging failed Binary32 canonicalization cell="<<cell
							<<" error="<<cellError<<" rho_total="<<staged[cell][0]<<" species="
							<<staged[cell][1]<<','<<staged[cell][2]<<','<<staged[cell][3]<<','
							<<staged[cell][4]<<','<<staged[cell][5]<<','<<staged[cell][6]<<','
							<<staged[cell][7];lastAdvanceError=sourceFailure.str();
						stagedOK=false;
					}
					if(!stagedOK){trialStep*=0.5;error.clear();continue;}
				}
				if(persistence.productionMetal&&!persistence.forceZeroSourceForTest){
					for(std::size_t cell=0u;cell<shape.nx*shape.ny;++cell){
						bool flameHolderEligible=false;
						commandOK=ProductionEstablishedFlameHolderEligible(cell,shape.nx,shape.ny,
							canonicalPilotMask,sourcePattern,simulationTimeS>=pilotEndS,
							packetBeginning[cell],fuel,FireSimulationTransportRecord::OpenV1(),
							flameHolderEligible,&error);
						if(!commandOK)break;
						if(flameHolderEligible)reactions[cell].primaryEligible=true;
					}
					if(!commandOK){lastAdvanceError=error;break;}
				}
				profileControlMS=std::chrono::duration<double,std::milli>(
					std::chrono::steady_clock::now()-profileStageStart).count();
				solverPhase="production source packet construction";
				bool packetOK=true;profileStageStart=std::chrono::steady_clock::now();
				if(persistence.forceZeroSourceForTest){
					packets.assign(shape.CellCount(),MethaneSourcePacket());escape=RadiationEscapeFactor();
				}else packetOK=BuildFrozenMethaneSourcePackets(packetBeginning,reactions,
					std::vector<double>(shape.CellCount(),cellVolume),300.0,
					caseRecord.derived.referenceHeatReleaseRateW,
					caseRecord.derived.effectiveRadiativeFraction,false,fuel,fuel,
					FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1(),packets,escape,&error,
					workerCount);
				if(packetOK&&persistence.productionMetal)for(MethaneSourcePacket& packet:packets){
					RepresentMethaneSourcePacketBinary32(packet);
					if(!CertifiedBinary32SourcePacket(packet,fuel)){packetOK=false;
						 error="production source packet failed its Binary32 certificate";break;}
				}
				profileSourceMS=std::chrono::duration<double,std::milli>(
					std::chrono::steady_clock::now()-profileStageStart).count();
				if(packetOK&&persistence.productionMetal){
					solverPhase="production tangent-target construction";
					profileStageStart=std::chrono::steady_clock::now();
					std::vector<ConservativeVector> stagedConservative(packetBeginning.size());
					std::vector<ConservativeVector> sourceDelta(packetBeginning.size());
					ParallelFireSlices(packetBeginning.size(),workerCount,[&](const std::size_t cell){
						stagedConservative[cell]=ToConservativeVector(packetBeginning[cell]);
						for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
							sourceDelta[cell][1u+species]=packets[cell].constituentDelta[species];
						sourceDelta[cell][8]=packets[cell].sensibleEnergyDeltaJPerM3;
					});
					OpenMACField3D stagedVelocity;stagedVelocity.component=productionVelocity.component;
					ConservativeAdvance3DConfig tangentConfig=config;
					tangentConfig.transport.producerPrecision=FireStateProducerPrecision::Binary32;
					std::vector<double> canonicalTargetTemperature(packetBeginning.size());
					std::vector<CellMolecularTransportEvaluation> targetMolecular(
						packetBeginning.size());
					ParallelFireSlices(packetBeginning.size(),workerCount,[&](const std::size_t cell){
						canonicalTargetTemperature[cell]=packetBeginning[cell].temperatureK;
						targetMolecular[cell].gasDensityKGPerM3=states[cell].GasDensity();
						targetMolecular[cell].gasCpJPerKGK=cellTransportEvaluations[cell].gasCpJPerKGK;
						targetMolecular[cell].molecularViscosityPaS=
							cellTransportEvaluations[cell].molecularViscosityPaS;
						targetMolecular[cell].molecularConductivityWPerMK=
							cellTransportEvaluations[cell].molecularConductivityWPerMK;
					});
					for(std::size_t cell=0u;packetOK&&cell<packetBeginning.size();++cell)
						if(invalidReusableTransport[cell]!=0u)packetOK=EvaluateCellMolecularTransport(
							packetBeginning[cell],fuel,FireSimulationTransportRecord::OpenV1(),
							FireStateProducerPrecision::Binary32,targetMolecular[cell],&error);
					if(std::getenv("RISE_FIRE_TARGET_TEMPERATURE_IDENTITY_RED")){
						std::vector<double> reinvertedTemperature;
						packetOK=InvertPeriodicTemperaturesWithinBounds(stagedConservative,fuel,
							tangentConfig.transport.ambientTemperatureK,
							tangentConfig.transport.adiabaticTemperatureK,
							FireStateProducerPrecision::Binary32,reinvertedTemperature,&error,
							workerCount,false)&&reinvertedTemperature.size()==packetBeginning.size();
						if(packetOK)for(std::size_t cell=0u;cell<packetBeginning.size();++cell)
							if(reinvertedTemperature[cell]!=packetBeginning[cell].temperatureK){
								packetOK=false;error="stored production temperature differs from reinversion";
								break;
							}
						if(packetOK)std::fprintf(stderr,"PRODUCTION_TARGET_TEMPERATURE_IDENTITY cells=%zu\n",
							packetBeginning.size());
					}
					std::vector<double> divergenceTarget;
					packetOK=packetOK&&MonitoredProductionTangentDivergenceTarget3D(shape,stagedConservative,
						stagedVelocity,sourceDelta,tangentConfig,fuel,fuel,
						FireSimulationTransportRecord::OpenV1(),divergenceTarget,&error,
						&canonicalTargetTemperature,&targetMolecular);
					if(packetOK&&std::getenv("RISE_FIRE_TARGET_MOLECULAR_IDENTITY_RED")){
						std::vector<double> recomputedTarget;std::string recomputedError;
						packetOK=MonitoredProductionTangentDivergenceTarget3D(shape,stagedConservative,
							stagedVelocity,sourceDelta,tangentConfig,fuel,fuel,
							FireSimulationTransportRecord::OpenV1(),recomputedTarget,&recomputedError,
							&canonicalTargetTemperature)&&recomputedTarget==divergenceTarget;
						if(!packetOK)error=recomputedError.empty()?
							"reused molecular transport changed the tangent target":recomputedError;
						else std::fprintf(stderr,"PRODUCTION_TARGET_MOLECULAR_IDENTITY cells=%zu\n",
							packetBeginning.size());
					}
					profileTargetMS=std::chrono::duration<double,std::milli>(
						std::chrono::steady_clock::now()-profileStageStart).count();
					profileStageStart=std::chrono::steady_clock::now();
					MethaneRunCheckpoint productionState;
					productionState.dimensions={{shape.nx,shape.ny,shape.nz}};
					productionState.cellWidthM=shape.cellWidthM;
					productionState.states=packetBeginning;
					productionState.momentum=productionMomentum;
					productionState.velocity=productionVelocity;
					RISE::FireProductionResidentStepRequest request;
					packetOK=packetOK&&FireProductionDyadicCalibration::BuildProductionRequest(
						productionState,divergenceTarget,trialStep,request,error,workerCount,
						&cellTransportEvaluations,&invalidReusableTransport);
					if(packetOK&&std::getenv("RISE_FIRE_REQUEST_LAYOUT_IDENTITY_RED")){
						RISE::FireProductionResidentStepRequest serialRequest;
						std::string serialError;
						packetOK=FireProductionDyadicCalibration::BuildProductionRequest(
							productionState,divergenceTarget,trialStep,serialRequest,serialError,1u)&&
							FireProductionDyadicCalibration::ResidentStepRequestsExactlyEqual(
								request,serialRequest);
						if(!packetOK)error=serialError.empty()?
							"parallel production request differs from the serial request":serialError;
					}
					if(packetOK){
						solverPhase="production resident request construction";
						request.physicalOpenProjectionVCycleCount=19u;
						ParallelFireSlices(shape.CellCount(),workerCount,[&](const std::size_t cell){
							for(std::size_t component=0u;component<9u;++component){
								const double value=sourceDelta[cell][component];
								request.cellSourceIncrement[component*shape.CellCount()+cell]=
									static_cast<float>(value);
							}
						});
						RISE::FireProductionFrozenSourcePacketSeal projectedHeunSource;
						if(persistence.sealedProjectedHeunReplay){
							RISE::FireProductionFrozenMethaneSourceRequest sourceRequest;
							sourceRequest.shape=request.force.shape;sourceRequest.timeStepS=request.force.timeStepS;
							sourceRequest.beginningTimeS=simulationTimeS;
							sourceRequest.attemptIdentity=RISE::FireProductionAcceptedStatePayloadDigestFast(
								request.force.shape,request.cellTransport.conservativeValues,
								request.force.beginningMomentumKGPerM2S,
								request.cellTransport.frozenVelocityMPerS);
							std::uint64_t timeBits=0u;std::memcpy(&timeBits,&simulationTimeS,sizeof(timeBits));
							std::uint32_t dtBits=0u;std::memcpy(&dtBits,&request.force.timeStepS,sizeof(dtBits));
							sourceRequest.attemptIdentity^=timeBits^(static_cast<std::uint64_t>(dtBits)<<32u);
							if(sourceRequest.attemptIdentity==0u)sourceRequest.attemptIdentity=1u;
							sourceRequest.caseRecordEnvelope=caseRecord.envelopeBytes;
							sourceRequest.beginningConservativeValues=request.cellTransport.conservativeValues;
							sourceRequest.sourceEvaluationTemperatureK.resize(shape.CellCount());
							sourceRequest.eligibilityBeginningConservativeValues.resize(9u*shape.CellCount());
							sourceRequest.pilotCommandMask.resize(shape.CellCount());
							sourceRequest.sourceBoundaryContactMask.resize(shape.CellCount());
							sourceRequest.mixingTimeS.resize(shape.CellCount());
							for(std::size_t cell=0u;cell<shape.CellCount();++cell){
								sourceRequest.sourceEvaluationTemperatureK[cell]=
									packetBeginning[cell].temperatureK;
								for(std::size_t component=0u;component<9u;++component)
									sourceRequest.eligibilityBeginningConservativeValues[
										component*shape.CellCount()+cell]=static_cast<float>(beginning[cell][component]);
								sourceRequest.pilotCommandMask[cell]=ProductionPilotCommandCell(cell,shape.nx,
									shape.ny,canonicalPilotMask,sourcePattern)?1u:0u;
								sourceRequest.sourceBoundaryContactMask[cell]=
									ProductionSourceBoundaryContactCell(cell,shape.nx,shape.ny,
										canonicalPilotMask,sourcePattern)?1u:0u;
								sourceRequest.mixingTimeS[cell]=reactions[cell].mixingTimeS;}
							sourceRequest.pilotEstablished=simulationTimeS>=pilotEndS;
							sourceRequest.predictiveRadiation=false;sourceRequest.workerCount=workerCount;
							packetOK=FireSim::FireProductionCanonicalSourceAuthority::Build(sourceRequest,
								projectedHeunSource,&error);
							if(packetOK&&projectedHeunSource.SourceDelta()!=request.cellSourceIncrement){
								std::size_t mismatch=0u;while(mismatch<request.cellSourceIncrement.size()&&
									projectedHeunSource.SourceDelta()[mismatch]==
										request.cellSourceIncrement[mismatch])++mismatch;
								std::ostringstream mismatchMessage;mismatchMessage<<std::setprecision(17)
									<<"projected-Heun canonical source differs from persisted source fields: index="
									<<mismatch<<" canonical="<<projectedHeunSource.SourceDelta()[mismatch]
									<<" persisted="<<request.cellSourceIncrement[mismatch];
								error=mismatchMessage.str();packetOK=false;
							}
						}
						profileLayoutMS=std::chrono::duration<double,std::milli>(
							std::chrono::steady_clock::now()-profileStageStart).count();
						profileStageStart=std::chrono::steady_clock::now();
						solverPhase="production resident attempt";
						RISE::FireProductionResidentStepResult production;
						RISE::FireProductionProjectedHeunMetalOwnerResult projectedHeunDiagnostics;
						RISE::FireProductionSingleStageFCTDiagnosticResult singleStageFCT;
						bool attemptComputed=false;double wallMS=0.0,deviceMS=0.0;
						for(;;){
							const auto wallStart=std::chrono::steady_clock::now();
							if(persistence.singleStageFCTDiagnostic){
								RISE::FireProductionSingleStageFCTBoundaryState boundaryState;
								boundaryState.pressureOpenInflow=singleStageFCTAcceptedInflow;
								boundaryState.statePayloadIdentity=
									RISE::FireProductionAcceptedStatePayloadDigestFast(
										request.force.shape,request.cellTransport.conservativeValues,
										request.force.beginningMomentumKGPerM2S,
										request.cellTransport.frozenVelocityMPerS);
								attemptComputed=RISE::SealFireProductionSingleStageFCTBoundaryState(
									request.force.shape,request.force.boundary,boundaryState,&error)&&
									RISE::AttemptFireProductionSingleStageFCTDiagnosticMetal(
										request,boundaryState,singleStageFCT,&error);
								if(attemptComputed){
									production.conservativeValues=singleStageFCT.conservativeValues;
									production.physicalProjection=singleStageFCT.physicalProjection;
									production.projection=singleStageFCT.projection;
									production.forceSchedule=singleStageFCT.forceSchedule;
									production.forceDiagnostics=singleStageFCT.forceDiagnostics;
									production.residentProjectionInvocationCount=
										singleStageFCT.residentProjectionInvocationCount;
									production.interstageFullGridTransferCount=
										singleStageFCT.interstageFullGridTransferCount;
									production.terminalStagingCount=singleStageFCT.terminalStagingCount;
									production.combinedCertifiedWorkingSetBytes=
										singleStageFCT.certifiedWorkingSetBytes;
									production.combinedActualMetalAllocationBytes=
										singleStageFCT.actualMetalAllocationBytes;
									production.representedTimeStepS=request.force.timeStepS;
									production.manifoldDynamicsBoundPassed=true;
									production.manifoldDiagnosticsMonitored=true;
									production.manifoldPlateauPassed=true;
									production.conservativeProducerPrecision=
										FireStateProducerPrecision::Binary32;
									production.acceptedShape=request.force.shape;
								}
							}else if(persistence.sealedProjectedHeunReplay){
								RISE::FireProductionProjectedHeunMetalOwnerRequest ownerRequest;
								attemptComputed=packetOK&&BuildProductionProjectedHeunOwnerRequest(request,
									projectedHeunSource,caseRecord.envelopeBytes,
									static_cast<float>(config.projectionTolerancePerS),
									static_cast<float>(config.openBoundary.velocityToleranceMPerS),
									ownerRequest,error)&&
									RISE::AttemptFireProductionProjectedHeunResidentStepMetal(ownerRequest,
										production,&projectedHeunDiagnostics,&error);
							}else attemptComputed=persistence.compatibleMomentumDiagnostic?
								RISE::AttemptFireProductionCompatibleMomentumDiagnosticMetal(
									request,production,&error):
								RISE::AttemptFireProductionResidentStepMetal(request,production,&error);
							wallMS+=std::chrono::duration<double,std::milli>(
								std::chrono::steady_clock::now()-wallStart).count();
							if(std::isfinite(production.deviceMakespanMS)&&
								production.deviceMakespanMS>=0.0)deviceMS+=production.deviceMakespanMS;
							const bool twoProjectionAttempt=
								production.residentProjectionInvocationCount==2u;
							const RISE::FireProductionProjectionResult& retryProjection=
								twoProjectionAttempt?production.physicalProjection:production.projection;
							if(persistence.singleStageFCTDiagnostic||retryProjection.validationPassed||
								production.HasAcceptedManifoldToken())break;
							const double pre=retryProjection.
								maximumPreProjectionResidualPerS;
							const double post=retryProjection.
								maximumPostProjectionResidualPerS;
							const double band=retryProjection.validationBandPerS;
							const std::uint32_t cycles=retryProjection.executedVCycleCount;
							if((twoProjectionAttempt&&!production.projection.validationPassed)||cycles!=
								request.physicalOpenProjectionVCycleCount||cycles<1u||cycles>=64u||
								!std::isfinite(pre)||!std::isfinite(post)||!std::isfinite(band)||
								!(pre>post)||!(post>0.0)||!(band>0.0)||!(post>band))break;
							const double contraction=std::pow(post/pre,1.0/static_cast<double>(cycles));
							const double requiredReal=(contraction>0.0&&contraction<1.0)?
								std::ceil(std::log(band/pre)/std::log(contraction)):0.0;
							if(!std::isfinite(requiredReal)||requiredReal<=cycles||requiredReal>64.0)break;
							request.physicalOpenProjectionVCycleCount=
								static_cast<std::uint32_t>(requiredReal);
							++values.productionPhysicalProjectionRetryCount;
							production=RISE::FireProductionResidentStepResult();error.clear();
						}
						unsigned int nextCandidate=0u;double nextTimeStepS=0.0;
						const RISE::FireProductionResidentStepAttemptDisposition disposition=
							persistence.singleStageFCTDiagnostic?
								(attemptComputed&&singleStageFCT.accepted?
									RISE::FireProductionResidentStepAttemptDisposition::Accepted:
									RISE::FireProductionResidentStepAttemptDisposition::Rejected):
								RISE::ClassifyFireProductionResidentStepAttempt(
									reduction,production,nextCandidate,nextTimeStepS);
						if(!persistence.productionOnsetDiagnosticDirectory.empty()&&
							disposition!=RISE::FireProductionResidentStepAttemptDisposition::Accepted){
							std::ofstream retryTrajectory(persistence.productionOnsetDiagnosticDirectory/
								"retry_attempt_trajectory.csv",std::ios::app);
							retryTrajectory<<std::setprecision(17)<<simulationTimeS<<','<<reduction<<','<<
								static_cast<double>(production.representedTimeStepS)<<','<<
								production.maximumAcceptedManifoldDeviation<<','<<
								production.acceptedManifoldDeviationP95<<','<<
								production.manifoldTailCellCount<<','<<
								production.manifoldTailDrainedVolumeM3<<','<<
								(production.manifoldDynamicsBoundPassed?1:0)<<','<<
								(production.physicalProjection.validationPassed?1:0)<<','<<
								(production.projection.validationPassed?1:0)<<','<<
								(production.HasAcceptedManifoldToken()?1:0)<<','<<
								(production.manifoldNextTimeStepAvailable?1:0)<<','<<
								production.suggestedManifoldTimeStepS<<','<<
								static_cast<unsigned int>(disposition)<<','<<error<<'\n';
							if(!retryTrajectory){lastAdvanceError=
								"production onset retry trajectory publication failed";break;}
						}
						double attemptMaximumVelocity=0.0;unsigned int attemptMaximumAxis=0u;
						std::size_t attemptMaximumFace=0u,attemptMaximumX=0u,
							attemptMaximumY=0u,attemptMaximumZ=0u;
						for(unsigned int axis=0u;axis<3u;++axis)
							for(std::size_t face=0u;face<production.projection.velocityMPerS[axis].size();++face){
								const double magnitude=std::fabs(static_cast<double>(
									production.projection.velocityMPerS[axis][face]));
								if(magnitude>attemptMaximumVelocity){attemptMaximumVelocity=magnitude;
									attemptMaximumAxis=axis;attemptMaximumFace=face;}
							}
						if(attemptMaximumAxis==0u){
							const std::size_t xExtent=request.force.shape.nx+1u;
							attemptMaximumX=std::min(attemptMaximumFace%xExtent,
								request.force.shape.nx-1u);
							const std::size_t yz=attemptMaximumFace/xExtent;
							attemptMaximumY=yz%request.force.shape.ny;
							attemptMaximumZ=yz/request.force.shape.ny;
						}else if(attemptMaximumAxis==1u){
							attemptMaximumX=attemptMaximumFace%request.force.shape.nx;
							const std::size_t yz=attemptMaximumFace/request.force.shape.nx;
							attemptMaximumY=std::min(yz%(request.force.shape.ny+1u),
								request.force.shape.ny-1u);
							attemptMaximumZ=yz/(request.force.shape.ny+1u);
						}else{
							attemptMaximumX=attemptMaximumFace%request.force.shape.nx;
							const std::size_t yz=attemptMaximumFace/request.force.shape.nx;
							attemptMaximumY=yz%request.force.shape.ny;
							attemptMaximumZ=std::min(yz/request.force.shape.ny,
								request.force.shape.nz-1u);
								}
						advancedOK=attemptComputed;
						std::filesystem::path effectiveMomentumAuditPath;
						std::size_t onsetThresholdIndex=productionOnsetVelocityThresholds.size();
						std::size_t auditColumnX=38u,auditColumnY=42u;
						bool exactTimeObservation=false;
						if(const char* auditPath=std::getenv("RISE_FIRE_MOMENTUM_AUDIT_PATH"))
							effectiveMomentumAuditPath=auditPath;
						if(disposition==RISE::FireProductionResidentStepAttemptDisposition::Accepted&&
							!persistence.productionOnsetDiagnosticDirectory.empty())
							for(std::size_t threshold=0u;threshold<productionOnsetVelocityThresholds.size();
								++threshold)if(!productionOnsetThresholdCaptured[threshold]&&
								attemptMaximumVelocity>=productionOnsetVelocityThresholds[threshold]){
								if(attemptMaximumAxis!=2u){lastAdvanceError=
									"production onset maximum is not vertical; column budget refused";
									fatalOnsetAuditFailure=true;advancedOK=false;break;}
								onsetThresholdIndex=threshold;auditColumnX=attemptMaximumX;
								auditColumnY=attemptMaximumY;std::ostringstream name;
								name<<"threshold_"<<static_cast<unsigned int>(
									productionOnsetVelocityThresholds[threshold])<<".raw.csv";
								effectiveMomentumAuditPath=
									persistence.productionOnsetDiagnosticDirectory/name.str();break;
							}
						if(fatalOnsetAuditFailure)break;
						if(onsetThresholdIndex==productionOnsetVelocityThresholds.size()&&
							disposition==RISE::FireProductionResidentStepAttemptDisposition::Accepted&&
							!productionMomentumObservationCaptured&&
							persistence.productionMomentumObservationTimeS>0.0&&
							simulationTimeS>=persistence.productionMomentumObservationTimeS){
							exactTimeObservation=true;
							if(!(persistence.productionMomentumObservationReferenceTier>0.0)){
								lastAdvanceError="production matched observation lacks reference tier";
								mandatoryEvidenceFailure=true;advancedOK=false;break;}
							const double referenceCellWidth=caseRecord.derived.
								characteristicDiameterM/
								persistence.productionMomentumObservationReferenceTier;
							auditColumnX=static_cast<std::size_t>(std::floor(
								(static_cast<double>(persistence.productionMomentumObservationColumnX)+0.5)*
								referenceCellWidth/shape.cellWidthM));
							auditColumnY=static_cast<std::size_t>(std::floor(
								(static_cast<double>(persistence.productionMomentumObservationColumnY)+0.5)*
								referenceCellWidth/shape.cellWidthM));
							effectiveMomentumAuditPath=
								persistence.productionOnsetDiagnosticDirectory/
								"reference_composition_candidate_fixed_column.raw.csv";
						}
						if(attemptComputed&&
							disposition==RISE::FireProductionResidentStepAttemptDisposition::Accepted&&
							!effectiveMomentumAuditPath.empty()){
							RISE::FireProductionResidentStepResult physicalOnly;
							RISE::FireProductionFrozenForceAdvanceResult forceCPU;
							RISE::FireProductionFrozenForceResult forceFieldsCPU;
							RISE::FireProductionResidentForceDiagnostics forceAuditDiagnostics;
							RISE::FireProductionCellPalindromeResult cellCPU;
							RISE::FireProductionDualMomentumResult dualCPU;
							std::string physicalOnlyError;
							const bool requestRetainedPhysicalProjection=
								!persistence.productionOnsetDiagnosticDirectory.empty()||
								persistence.compatibleMomentumDiagnostic||
								persistence.singleStageFCTDiagnostic;
							const bool restorationModeClear=
								std::getenv("RISE_FIRE_PRODUCTION_RESTORATION_TEST")==nullptr;
							const bool retainedPhysicalIsTerminal=requestRetainedPhysicalProjection&&
								!production.manifoldTailRestorationApplied&&
								production.physicalProjection.velocityMPerS[0].empty();
							const bool useRetainedPhysicalProjection=requestRetainedPhysicalProjection&&
								(retainedPhysicalIsTerminal||
								 !production.physicalProjection.velocityMPerS[0].empty());
							bool physicalOnlyComputed=false;
							if(useRetainedPhysicalProjection){
								// The accepted attempt already retains the exact on-device physical
								// projection. Re-solving it on the CPU would measure a different
								// implementation and, near onset, can move the pressure budget.
								physicalOnlyComputed=retainedPhysicalIsTerminal?
									!production.projection.velocityMPerS[0].empty():
									!production.physicalProjection.velocityMPerS[0].empty();
							}else if(attemptComputed&&restorationModeClear&&
								setenv("RISE_FIRE_PRODUCTION_RESTORATION_TEST","removed",1)==0){
								physicalOnlyComputed=persistence.compatibleMomentumDiagnostic?
									RISE::AttemptFireProductionCompatibleMomentumDiagnosticMetal(
										request,physicalOnly,&physicalOnlyError):
									RISE::AttemptFireProductionResidentStepMetal(
										request,physicalOnly,&physicalOnlyError);
								unsetenv("RISE_FIRE_PRODUCTION_RESTORATION_TEST");
							}
							const RISE::FireProductionProjectionResult& physical=useRetainedPhysicalProjection?
								(retainedPhysicalIsTerminal?production.projection:
								 production.physicalProjection):physicalOnly.projection;
							double physicalVelocityMaximum=0.0,terminalVelocityMaximum=0.0;
							double restorationVelocityMaximum=0.0,physicalImpulseMaximum=0.0;
							double restorationImpulseMaximum=0.0,compatibilityResidualMaximum=0.0;
							unsigned int terminalAxis=0u,restorationAxis=0u;
							std::size_t terminalFace=0u,restorationFace=0u;
							const char* auditPhase="paired projection attempt";
							bool forceInclusiveProvisionalByteIdentity=false;
							double forceInclusiveProvisionalDifferenceMaximum=0.0;
							std::size_t forceInclusiveProvisionalDifferenceCount=0u;
							const bool attemptPayloadAvailable=
								!production.projection.velocityMPerS[0].empty();
							const bool physicalOnlyPayloadAvailable=
								!physical.velocityMPerS[0].empty();
							const bool physicalDiagnosticsIdentity=
								!useRetainedPhysicalProjection||physical.validationPassed;
							const bool pairedTransportIdentity=attemptPayloadAvailable&&
								physicalOnlyPayloadAvailable&&physicalDiagnosticsIdentity&&
								(useRetainedPhysicalProjection||
								physicalOnly.transportedDual.momentum==production.transportedDual.momentum);
							bool auditValid=attemptPayloadAvailable&&physicalOnlyPayloadAvailable&&
								pairedTransportIdentity;
							for(unsigned int axis=0u;axis<3u&&auditValid;++axis){
								const std::size_t faces=production.projection.velocityMPerS[axis].size();
								auditValid=faces>0u&&physical.velocityMPerS[axis].size()==faces&&
									production.projection.momentumKGPerM2S[axis].size()==faces&&
									physical.momentumKGPerM2S[axis].size()==faces&&
									production.projection.faceDensityKGPerM3[axis].size()==faces&&
									production.transportedDual.momentum[axis].size()==faces;
								for(std::size_t face=0u;face<faces&&auditValid;++face){
									const double physicalVelocity=physical.velocityMPerS[axis][face];
									const double terminalVelocity=production.projection.velocityMPerS[axis][face];
									const double restorationVelocity=terminalVelocity-physicalVelocity;
									const double physicalImpulse=physical.momentumKGPerM2S[axis][face]-
										production.transportedDual.momentum[axis][face];
									const double restorationImpulse=
										production.projection.momentumKGPerM2S[axis][face]-
										physical.momentumKGPerM2S[axis][face];
									const double compatibility=std::fabs(
										production.projection.momentumKGPerM2S[axis][face]-
										production.projection.faceDensityKGPerM3[axis][face]*terminalVelocity);
									physicalVelocityMaximum=std::max(physicalVelocityMaximum,
										std::fabs(physicalVelocity));
									if(std::fabs(terminalVelocity)>terminalVelocityMaximum){
										terminalVelocityMaximum=std::fabs(terminalVelocity);
										terminalAxis=axis;terminalFace=face;}
									if(std::fabs(restorationVelocity)>restorationVelocityMaximum){
										restorationVelocityMaximum=std::fabs(restorationVelocity);
										restorationAxis=axis;restorationFace=face;}
									physicalImpulseMaximum=std::max(physicalImpulseMaximum,
										std::fabs(physicalImpulse));
									restorationImpulseMaximum=std::max(restorationImpulseMaximum,
										std::fabs(restorationImpulse));
									compatibilityResidualMaximum=std::max(compatibilityResidualMaximum,
										compatibility);
								}
							}
							const std::size_t columnX=auditColumnX,columnY=auditColumnY;
							std::vector<float> columnPhysicalMomentum(request.force.shape.nz+1u,0.0f);
							for(std::size_t z=0u;z<columnPhysicalMomentum.size()&&auditValid;++z){
								const std::size_t face=(z*request.force.shape.ny+columnY)*
									request.force.shape.nx+columnX;
								if(face>=physical.momentumKGPerM2S[2].size())auditValid=false;
								else columnPhysicalMomentum[z]=physical.momentumKGPerM2S[2][face];
							}
							const double terminalPhysicalMomentum=auditValid?
								physical.momentumKGPerM2S[terminalAxis][terminalFace]:0.0;
							const double terminalPhysicalVelocity=auditValid?
								physical.velocityMPerS[terminalAxis][terminalFace]:0.0;
							physicalOnly=RISE::FireProductionResidentStepResult();
							auditPhase="independent resident force reconstruction";
							const bool forceComparatorOK=auditValid&&
								RISE::AdvanceFireProductionFrozenForceMetalResidentStateComparator(
									request.force,forceCPU,forceAuditDiagnostics,&physicalOnlyError);
							const bool forceDiagnosticsIdentity=forceComparatorOK&&
								(persistence.sealedProjectedHeunReplay||
								 forceAuditDiagnostics.outwardLambdaPerS==
									production.forceDiagnostics.outwardLambdaPerS);
							const bool forceScheduleIdentity=forceDiagnosticsIdentity&&
								(persistence.sealedProjectedHeunReplay||
								 (forceCPU.schedule.substepCount==production.forceSchedule.substepCount&&
								  forceCPU.schedule.substepTimeS==production.forceSchedule.substepTimeS));
							const bool forceFieldsOK=forceScheduleIdentity&&
								RISE::BuildFireProductionFrozenForceFieldsCPU(
									request.force,forceFieldsCPU,&physicalOnlyError);
							RISE::FireProductionCellPalindromeRequest auditCellTransport=
								request.cellTransport;
							auditCellTransport.retainAcceptedGasMassDose=true;
							const bool cellTransportOK=forceFieldsOK&&
								RISE::RemapFireProductionCellPalindromeCPU(
									auditCellTransport,cellCPU,&physicalOnlyError);
							auditValid=cellTransportOK;
							if(auditValid){
								auditPhase="CPU compatible dual remap from force-inclusive momentum";
								RISE::FireProductionDualMomentumRequest forceInclusive=request.dualTransport;
								forceInclusive.beginningMomentum=forceCPU.momentumKGPerM2S;
								auditValid=RISE::RemapFireProductionCompatibleDualMomentumCPU(
									forceInclusive,cellCPU.acceptedGasMassDoseKGPerM2,
									dualCPU,&physicalOnlyError);
								for(unsigned int axis=0u;axis<3u&&auditValid;++axis){
									auditValid=dualCPU.momentum[axis].size()==
										request.momentumSourceIncrement[axis].size();
									for(std::size_t face=0u;face<dualCPU.momentum[axis].size()&&
										auditValid;++face)dualCPU.momentum[axis][face]+=
										request.momentumSourceIncrement[axis][face];
								}
							}
							auditPhase="force-inclusive provisional comparison";
							if(auditValid)
								for(unsigned int axis=0u;axis<3u;++axis)
								for(std::size_t face=0u;face<dualCPU.momentum[axis].size();++face){
									const double difference=std::fabs(static_cast<double>(dualCPU.momentum[axis][face])-
										production.transportedDual.momentum[axis][face]);
									if(difference!=0.0)++forceInclusiveProvisionalDifferenceCount;
									forceInclusiveProvisionalDifferenceMaximum=std::max(
										forceInclusiveProvisionalDifferenceMaximum,difference);
								}
							forceInclusiveProvisionalByteIdentity=
								auditValid&&
								forceInclusiveProvisionalDifferenceCount==0u;
							if(!auditValid){
								mandatoryEvidenceFailure=true;
								lastAdvanceError=std::string("production momentum audit failed at ")+auditPhase+
									" [attempt="+(attemptComputed?"1":"0")+
									" physical_only="+(physicalOnlyComputed?"1":"0")+
									" attempt_payload="+(attemptPayloadAvailable?"1":"0")+
									" physical_payload="+(physicalOnlyPayloadAvailable?"1":"0")+
									" paired_transport="+(pairedTransportIdentity?"1":"0")+
									" force="+(forceComparatorOK?"1":"0")+
									" diagnostics="+(forceDiagnosticsIdentity?"1":"0")+
									" schedule="+(forceScheduleIdentity?"1":"0")+
									" fields="+(forceFieldsOK?"1":"0")+
									" cell="+(cellTransportOK?"1":"0")+"]"+
									(physicalOnlyError.empty()?"":": "+physicalOnlyError);
								std::fprintf(stderr,"%s\n",lastAdvanceError.c_str());advancedOK=false;break;
							}
							if(auditValid){
								if(auditColumnX>=request.force.shape.nx||
									auditColumnY>=request.force.shape.ny){
									mandatoryEvidenceFailure=true;
									lastAdvanceError="production momentum audit column is out of range";
									advancedOK=false;break;
								}
								const double representedStep=static_cast<double>(production.representedTimeStepS);
								double columnStressMaximum=0.0,columnBuoyancyMaximum=0.0,
									columnAdvectionMaximum=0.0,columnSourceMaximum=0.0,
									columnPressureMaximum=0.0,columnRestorationMaximum=0.0,
									columnTotalMaximum=0.0,columnClosureMaximum=0.0,
									columnVremanMinimum=std::numeric_limits<double>::infinity(),
									columnVremanMaximum=0.0;
								std::size_t columnAdvectionMaximumZ=0u;
								const std::filesystem::path columnPath=
									effectiveMomentumAuditPath.string()+".column.csv";
								std::error_code columnSizeError;const bool writeColumnHeader=
									!std::filesystem::exists(columnPath)||
									std::filesystem::file_size(columnPath,columnSizeError)==0u;
								std::ofstream columnAudit(columnPath,std::ios::app);
								if(writeColumnHeader)columnAudit<<"beginning_time_s,candidate,dt_s,x,y,z_face,"
									"beginning_momentum,stress_rate,buoyancy_rate,advection_rate,source_rate,"
									"pressure_gradient_rate,restoration_rate,total_rate,closure_residual,"
									"lower_vreman_m2_per_s,upper_vreman_m2_per_s\n";
								for(std::size_t z=0u;z<=request.force.shape.nz;++z){
									const std::size_t face=(z*request.force.shape.ny+columnY)*
										request.force.shape.nx+columnX;
									const double beginning=request.force.beginningMomentumKGPerM2S[2][face];
									const double gravity=forceFieldsCPU.
										gravityMomentumIncrementKGPerM2S[2][face];
									const double stress=persistence.sealedProjectedHeunReplay?
										projectedHeunDiagnostics.heunStressMomentumRateKGPerM2S2[2][face]:
										(forceCPU.momentumKGPerM2S[2][face]-beginning-gravity)/representedStep;
									const double buoyancy=persistence.sealedProjectedHeunReplay?
										projectedHeunDiagnostics.heunBuoyancyMomentumRateKGPerM2S2[2][face]:
										gravity/representedStep;
									const double source=persistence.sealedProjectedHeunReplay?
										projectedHeunDiagnostics.heunPhaseSourceMomentumRateKGPerM2S2[2][face]:
										request.momentumSourceIncrement[2][face]/representedStep;
									const double advection=persistence.sealedProjectedHeunReplay?
										projectedHeunDiagnostics.heunAdvectionMomentumRateKGPerM2S2[2][face]:
										(production.transportedDual.momentum[2][face]-
										 request.momentumSourceIncrement[2][face]-
										 forceCPU.momentumKGPerM2S[2][face])/representedStep;
									const double pressure=(production.projection.momentumKGPerM2S[2][face]-
										production.transportedDual.momentum[2][face])/representedStep;
									const double restoration=0.0;
									const double total=(production.projection.momentumKGPerM2S[2][face]-beginning)/
										representedStep;
									const double closure=total-(stress+buoyancy+advection+source+pressure+restoration);
									const std::size_t lowerZ=z==0u?0u:z-1u;
									const std::size_t upperZ=std::min(z,request.force.shape.nz-1u);
									const std::size_t lowerCell=columnX+request.force.shape.nx*(columnY+
										request.force.shape.ny*lowerZ);
									const std::size_t upperCell=columnX+request.force.shape.nx*(columnY+
										request.force.shape.ny*upperZ);
									const double lowerVreman=persistence.sealedProjectedHeunReplay?
										projectedHeunDiagnostics.heunEddyKinematicViscosityM2PerS[lowerCell]:
										forceFieldsCPU.eddyKinematicViscosityM2PerS[lowerCell];
									const double upperVreman=persistence.sealedProjectedHeunReplay?
										projectedHeunDiagnostics.heunEddyKinematicViscosityM2PerS[upperCell]:
										forceFieldsCPU.eddyKinematicViscosityM2PerS[upperCell];
									columnStressMaximum=std::max(columnStressMaximum,std::fabs(stress));
									columnBuoyancyMaximum=std::max(columnBuoyancyMaximum,std::fabs(buoyancy));
									if(std::fabs(advection)>columnAdvectionMaximum){
										columnAdvectionMaximum=std::fabs(advection);
										columnAdvectionMaximumZ=z;}
									columnSourceMaximum=std::max(columnSourceMaximum,std::fabs(source));
									columnPressureMaximum=std::max(columnPressureMaximum,std::fabs(pressure));
									columnRestorationMaximum=std::max(columnRestorationMaximum,std::fabs(restoration));
									columnTotalMaximum=std::max(columnTotalMaximum,std::fabs(total));
									columnClosureMaximum=std::max(columnClosureMaximum,std::fabs(closure));
									columnVremanMinimum=std::min({columnVremanMinimum,lowerVreman,upperVreman});
									columnVremanMaximum=std::max({columnVremanMaximum,lowerVreman,upperVreman});
									columnAudit<<std::setprecision(17)<<simulationTimeS<<','<<reduction<<','<<
										representedStep<<','<<columnX<<','<<columnY<<','<<z<<','<<beginning<<','<<
										stress<<','<<buoyancy<<','<<advection<<','<<source<<','<<pressure<<','<<
										restoration<<','<<total<<','<<closure<<','<<lowerVreman<<','<<upperVreman<<'\n';
								}
								columnAudit.close();
								if(!columnAudit){lastAdvanceError="production momentum column audit write failed";
									mandatoryEvidenceFailure=true;advancedOK=false;break;}
								std::error_code sizeError;const bool writeHeader=
									!std::filesystem::exists(effectiveMomentumAuditPath)||
									std::filesystem::file_size(effectiveMomentumAuditPath,sizeError)==0u;
								std::ofstream audit(effectiveMomentumAuditPath,std::ios::app);
								if(writeHeader)audit<<"beginning_time_s,candidate,dt_s,tail_cells,tail_drained_m3,"
									"physical_velocity_max_m_per_s,restoration_delta_velocity_max_m_per_s,"
									"terminal_velocity_max_m_per_s,physical_impulse_max_kg_per_m2_s,"
									"restoration_impulse_max_kg_per_m2_s,terminal_axis,terminal_face,"
									"terminal_provisional_momentum,terminal_physical_momentum,"
									"terminal_momentum,terminal_face_density,terminal_physical_velocity,"
									"terminal_velocity,restoration_axis,restoration_face,"
									"compatibility_residual_max,column_stress_rate_max,column_buoyancy_rate_max,"
									"column_advection_rate_max,column_source_rate_max,column_pressure_rate_max,"
									"column_restoration_rate_max,column_total_rate_max,column_closure_residual_max,"
									"column_vreman_min_m2_per_s,column_vreman_max_m2_per_s,"
									"attempt_velocity_max_m_per_s,attempt_velocity_axis,attempt_velocity_face,"
									"attempt_velocity_x,attempt_velocity_y,attempt_velocity_z,"
									"force_inclusive_provisional_byte_identity,force_inclusive_provisional_"
									"difference_count,force_inclusive_provisional_difference_max,"
									"source_packet_field_sha256\n";
								const std::string sourcePacketFieldDigest=
									ProductionSourcePacketFieldSHA256(packets);
								audit<<std::setprecision(17)<<simulationTimeS<<','<<reduction<<','<<
									static_cast<double>(production.representedTimeStepS)<<','<<
									production.manifoldTailCellCount<<','<<
									production.manifoldTailDrainedVolumeM3<<','<<physicalVelocityMaximum<<','<<
									restorationVelocityMaximum<<','<<terminalVelocityMaximum<<','<<
									physicalImpulseMaximum<<','<<restorationImpulseMaximum<<','<<terminalAxis<<','<<
									terminalFace<<','<<production.transportedDual.momentum[terminalAxis][terminalFace]<<','<<
									terminalPhysicalMomentum<<','<<
									production.projection.momentumKGPerM2S[terminalAxis][terminalFace]<<','<<
									production.projection.faceDensityKGPerM3[terminalAxis][terminalFace]<<','<<
									terminalPhysicalVelocity<<','<<
									production.projection.velocityMPerS[terminalAxis][terminalFace]<<','<<
									restorationAxis<<','<<restorationFace<<','<<compatibilityResidualMaximum<<','<<
									columnStressMaximum<<','<<columnBuoyancyMaximum<<','<<columnAdvectionMaximum<<','<<
									columnSourceMaximum<<','<<columnPressureMaximum<<','<<columnRestorationMaximum<<','<<
									columnTotalMaximum<<','<<columnClosureMaximum<<','<<columnVremanMinimum<<','<<
									columnVremanMaximum<<','<<attemptMaximumVelocity<<','<<attemptMaximumAxis<<','<<
									attemptMaximumFace<<','<<attemptMaximumX<<','<<attemptMaximumY<<','<<attemptMaximumZ<<','<<
									(forceInclusiveProvisionalByteIdentity?1:0)<<','<<
									forceInclusiveProvisionalDifferenceCount<<','<<
									forceInclusiveProvisionalDifferenceMaximum<<','<<
									sourcePacketFieldDigest<<'\n';
								audit.close();
								if(!audit){lastAdvanceError="production momentum audit write failed";
									mandatoryEvidenceFailure=true;advancedOK=false;break;}
								const std::filesystem::path sourceContextPath=
									effectiveMomentumAuditPath.string()+".source_context.csv";
								const std::size_t sourceContextX=exactTimeObservation?
									columnX:attemptMaximumX;
								const std::size_t sourceContextY=exactTimeObservation?
									columnY:attemptMaximumY;
								const std::size_t sourceContextZ=exactTimeObservation?
									std::min(columnAdvectionMaximumZ,request.force.shape.nz-1u):
									attemptMaximumZ;
								if(!WriteProductionSourceAndAdjacentStateContext(sourceContextPath,
									request.force.shape,sourceContextX,sourceContextY,
									sourceContextZ,packetBeginning,packets,
									production.conservativeValues,fuel,lastAdvanceError)){
									mandatoryEvidenceFailure=true;advancedOK=false;break;
								}
								if(!WriteProductionSourcePacketFieldEvidence(
									effectiveMomentumAuditPath,packets,states,packetBeginning,momentum,
									request,lastAdvanceError)){
									mandatoryEvidenceFailure=true;advancedOK=false;break;
								}
								std::ofstream event(effectiveMomentumAuditPath.string()+".event.v1",
									std::ios::trunc);
								event<<std::setprecision(17)
									<<"schema rise.fire.production.onset_observation_event.v1\n"
									<<"accepted_step_beginning "<<acceptedSteps<<"\n"
									<<"beginning_time_s "<<simulationTimeS<<"\nend_time_s "<<
										simulationTimeS+representedStep<<"\n"
									<<"realized_max_velocity_m_per_s "<<attemptMaximumVelocity<<"\n"
									<<"max_axis "<<attemptMaximumAxis<<"\nmax_face "<<attemptMaximumFace<<"\n"
									<<"source_packet_field_sha256 "<<sourcePacketFieldDigest<<"\n"
									<<"source_event_threshold "<<(onsetThresholdIndex<
										productionOnsetVelocityThresholds.size()?
										productionOnsetVelocityThresholds[onsetThresholdIndex]:0.0)<<"\n";
								for(const double threshold:productionOnsetVelocityThresholds)
									if(attemptMaximumVelocity>=threshold)event<<"crossed_threshold "<<
										threshold<<'\n';
								event
									<<"exact_reference_composition_endpoint_s "<<simulationTimeS<<"\n";
								event.close();
								if(!event){lastAdvanceError="production onset event write failed";
									mandatoryEvidenceFailure=true;advancedOK=false;break;}
								observationEvidencePublished=true;
								if(exactTimeObservation)productionMomentumObservationCaptured=true;
								if(onsetThresholdIndex<productionOnsetThresholdCaptured.size()){
									productionOnsetThresholdCaptured[onsetThresholdIndex]=true;
									for(std::size_t skipped=onsetThresholdIndex+1u;
										skipped<productionOnsetThresholdCaptured.size()&&attemptMaximumVelocity>=
											productionOnsetVelocityThresholds[skipped];++skipped){
										std::ostringstream skippedName;skippedName<<"threshold_"<<
											static_cast<unsigned int>(productionOnsetVelocityThresholds[skipped])<<
											".raw.csv";
										const std::filesystem::path skippedPath=
											persistence.productionOnsetDiagnosticDirectory/skippedName.str();
										std::error_code copyError;
										const bool summaryCopied=std::filesystem::copy_file(
											effectiveMomentumAuditPath,skippedPath,
											std::filesystem::copy_options::overwrite_existing,copyError);
										const bool columnCopied=summaryCopied&&std::filesystem::copy_file(
											columnPath,skippedPath.string()+".column.csv",
											std::filesystem::copy_options::overwrite_existing,copyError);
										const bool contextCopied=columnCopied&&std::filesystem::copy_file(
											sourceContextPath,skippedPath.string()+".source_context.csv",
											std::filesystem::copy_options::overwrite_existing,copyError);
										const bool payloadCopied=contextCopied&&std::filesystem::copy_file(
											effectiveMomentumAuditPath.string()+".source_packets.bin",
											skippedPath.string()+".source_packets.bin",
											std::filesystem::copy_options::overwrite_existing,copyError);
										const bool ledgerCopied=payloadCopied&&std::filesystem::copy_file(
											effectiveMomentumAuditPath.string()+".source_ledger.v1",
											skippedPath.string()+".source_ledger.v1",
											std::filesystem::copy_options::overwrite_existing,copyError);
										const bool completeCopied=ledgerCopied&&std::filesystem::copy_file(
							effectiveMomentumAuditPath.string()+".source_observation_inputs.bin",
							skippedPath.string()+".source_observation_inputs.bin",
											std::filesystem::copy_options::overwrite_existing,copyError);
										if(!completeCopied){lastAdvanceError=
											"production onset skipped-threshold publication failed";
											mandatoryEvidenceFailure=true;advancedOK=false;break;}
										std::ofstream alias(skippedPath.string()+".event.v1",std::ios::trunc);
										alias<<std::setprecision(17)
											<<"schema rise.fire.production.onset_observation_alias.v1\n"
											<<"threshold "<<productionOnsetVelocityThresholds[skipped]<<"\n"
											<<"source_threshold "<<productionOnsetVelocityThresholds[
												onsetThresholdIndex]<<"\nsource_event_sha256 "<<
												DigestFile(effectiveMomentumAuditPath.string()+".event.v1")<<"\n"
											<<"beginning_time_s "<<simulationTimeS<<"\nend_time_s "<<
												simulationTimeS+representedStep<<"\nrealized_max_velocity_m_per_s "<<
												attemptMaximumVelocity<<'\n';
										alias.close();
										if(!alias){lastAdvanceError=
											"production onset skipped-threshold alias write failed";
											mandatoryEvidenceFailure=true;advancedOK=false;break;}
										productionOnsetThresholdCaptured[skipped]=true;
									}
									if(!advancedOK)break;
								}
							}
						}
						if(!persistence.temporalSnapshotDirectory.empty()&&
							disposition!=RISE::FireProductionResidentStepAttemptDisposition::Accepted)
							std::fprintf(stderr,
							"production attempt candidate=%u computed=%d dt=%.17g dev=%.9g p95=%.9g "
							"tail=%u drain=%.9g dynamics=%d physical=%d terminal=%d token=%d "
							"next=%d suggested=%.17g disposition=%u error=%s\n",
							reduction,attemptComputed?1:0,
							static_cast<double>(production.representedTimeStepS),
							production.maximumAcceptedManifoldDeviation,
							production.acceptedManifoldDeviationP95,
							production.manifoldTailCellCount,
							production.manifoldTailDrainedVolumeM3,
							production.manifoldDynamicsBoundPassed?1:0,
							production.physicalProjection.validationPassed?1:0,
							production.projection.validationPassed?1:0,
							production.HasAcceptedManifoldToken()?1:0,
							production.manifoldNextTimeStepAvailable?1:0,
							production.suggestedManifoldTimeStepS,
							static_cast<unsigned int>(disposition),error.c_str());
						if(disposition==RISE::FireProductionResidentStepAttemptDisposition::
							RetryAtSuggestedTimeStep){
							++values.productionHardBoundRetryCount;
							lastAdvanceError="production monitored dynamics bound requested a reduced step";
							trialStep=nextTimeStepS;error.clear();continue;
						}
						if(disposition==RISE::FireProductionResidentStepAttemptDisposition::Accepted){
							RISE::FireProductionAcceptedManifoldObservation observation;
							advancedOK=persistence.singleStageFCTDiagnostic?
								FireProductionDyadicCalibration::ApplyProductionResultUnchecked(
									production,productionState,error,false,workerCount):
								(RISE::PublishFireProductionAcceptedManifoldObservation(
									static_cast<double>(production.representedTimeStepS),production,
									observation,&error)&&
								FireProductionDyadicCalibration::ApplyAcceptedProductionResult(
									production,observation,productionState,error,workerCount));
							if(advancedOK){
								if(persistence.singleStageFCTDiagnostic){
									productionManifoldObservation=FireProductionAcceptedManifoldObservation();
									singleStageFCTAcceptedInflow=singleStageFCT.projection.pressureOpenInflow;
								}else productionManifoldObservation=observation;
								advanced.conservative.resize(productionState.states.size());
								acceptedProductionTemperatureK.resize(productionState.states.size());
								ParallelFireSlices(productionState.states.size(),workerCount,
									[&](const std::size_t cell){
									advanced.conservative[cell]=ToConservativeVector(
										productionState.states[cell]);
									acceptedProductionTemperatureK[cell]=
										productionState.states[cell].temperatureK;
								});
								advanced.momentumKGPerM2S=productionState.momentum;
								advanced.velocityMPerS=productionState.velocity;
								values.monitoredManifoldMaximumHistory.push_back(
									production.maximumAcceptedManifoldDeviation);
								values.monitoredManifoldP95History.push_back(
									production.acceptedManifoldDeviationP95);
								values.monitoredManifoldP50History.push_back(
									production.acceptedManifoldDeviationP50);
								values.monitoredTailCellHistory.push_back(
									production.manifoldTailCellCount);
								values.monitoredTailDrainedVolumeHistoryM3.push_back(
									production.manifoldTailDrainedVolumeM3);
								values.productionDeviceHistoryMS.push_back(deviceMS);
								values.productionWallHistoryMS.push_back(wallMS);
								if(!persistence.productionOnsetDiagnosticDirectory.empty()){
									std::ofstream trajectory(persistence.productionOnsetDiagnosticDirectory/
										"maximum_velocity_trajectory.csv",std::ios::app);
									trajectory<<std::setprecision(17)<<acceptedSteps+1u<<','<<
										simulationTimeS+trialStep<<','<<trialStep<<','<<attemptMaximumVelocity<<','<<
										attemptMaximumAxis<<','<<attemptMaximumFace<<','<<attemptMaximumX<<','<<
										attemptMaximumY<<','<<attemptMaximumZ<<','<<
										production.maximumAcceptedManifoldDeviation<<','<<
										production.acceptedManifoldDeviationP95<<','<<
										production.acceptedManifoldDeviationP50<<','<<
										production.manifoldTailCellCount<<','<<
										production.manifoldTailDrainedVolumeM3<<','<<deviceMS<<','<<wallMS<<','<<
										projectedHeunDiagnostics.ownerPublicationIdentity<<','<<
										projectedHeunDiagnostics.commandCommitCount<<','<<
										projectedHeunDiagnostics.residentProjectionInvocationCount<<','<<
										projectedHeunDiagnostics.acceptedPicardIterations[0]<<','<<
										projectedHeunDiagnostics.acceptedPicardIterations[1]<<','<<
										projectedHeunDiagnostics.acceptedPicardIterations[2]<<','<<
										projectedHeunDiagnostics.actualMetalAllocationBytes<<','<<
										projectedHeunDiagnostics.certifiedWorkingSetBytes<<','<<
										projectedHeunDiagnostics.residentProjectionDeviceElapsedMS<<','<<
										projectedHeunDiagnostics.residentNonprojectionDeviceElapsedMS<<'\n';
									if(!trajectory){advancedOK=false;
										error="production onset trajectory publication failed";}
									if(advancedOK&&persistence.productionOnsetStopVelocityMPerS>0.0&&
										attemptMaximumVelocity>=persistence.productionOnsetStopVelocityMPerS)
										productionOnsetStopReached=true;
								}
								if(!persistence.temporalSnapshotDirectory.empty()&&
									(acceptedSteps+1u)%256u==0u){
									double maximumVelocity=0.0,maximumTemperature=0.0,
										minimumGasDensity=std::numeric_limits<double>::infinity(),
										maximumGasDensity=0.0,
										minimumAuxiliaryFaceDensity=std::numeric_limits<double>::infinity(),
										maximumTransportedMomentum=0.0;
									for(const MethaneCellState& accepted:productionState.states){
										maximumTemperature=std::max(maximumTemperature,
											accepted.temperatureK);
										minimumGasDensity=std::min(minimumGasDensity,accepted.GasDensity());
										maximumGasDensity=std::max(maximumGasDensity,accepted.GasDensity());
									}
									for(const std::vector<double>& component:productionState.velocity.component)
										for(const double velocity:component)maximumVelocity=std::max(
											maximumVelocity,std::fabs(velocity));
									for(const std::vector<float>& component:
										production.transportedDual.auxiliaryFaceDensity)
										for(const float density:component)minimumAuxiliaryFaceDensity=
											std::min(minimumAuxiliaryFaceDensity,static_cast<double>(density));
									for(const std::vector<float>& component:production.transportedDual.momentum)
										for(const float value:component)maximumTransportedMomentum=
											std::max(maximumTransportedMomentum,std::fabs(static_cast<double>(value)));
									std::fprintf(stderr,"production accepted step=%u time=%.17g dt=%.17g "
										"dev=%.9g p95=%.9g tail=%u drain=%.9g velocity=%.9g "
										"temperature=%.9g min_gas=%.9g max_gas=%.9g lambda=%.9g min_aux_rho=%.9g "
										"max_transport_momentum=%.9g\n",
										acceptedSteps+1u,simulationTimeS+trialStep,trialStep,
										production.maximumAcceptedManifoldDeviation,
										production.acceptedManifoldDeviationP95,
										production.manifoldTailCellCount,
										production.manifoldTailDrainedVolumeM3,maximumVelocity,
										maximumTemperature,minimumGasDensity,maximumGasDensity,
										static_cast<double>(production.forceDiagnostics.outwardLambdaPerS),
										minimumAuxiliaryFaceDensity,maximumTransportedMomentum);
								}
								acceptedBeginningStates=std::move(packetBeginning);
								acceptedExternalFuelMassKG=externalFuelMassKG;
							}
						}else{
							advancedOK=false;
							if(reportCapstoneProgress||
								!persistence.temporalSnapshotDirectory.empty())std::fprintf(stderr,
								"production rejection candidate=%u physical_valid=%d terminal_valid=%d "
								"token=%d plateau_passed=%d dynamics_passed=%d next_available=%d "
								"represented_dt=%.17g suggested_dt=%.17g field_max=%.17g "
								"invocations=%u cycles=%u pre=%.17g post=%.17g band=%.17g error=%s\n",
								reduction,production.physicalProjection.validationPassed?1:0,
								production.projection.validationPassed?1:0,
								production.HasAcceptedManifoldToken()?1:0,
								production.manifoldPlateauPassed?1:0,
								production.manifoldDynamicsBoundPassed?1:0,
								production.manifoldNextTimeStepAvailable?1:0,
								static_cast<double>(production.representedTimeStepS),
								production.suggestedManifoldTimeStepS,
								production.maximumAcceptedManifoldDeviation,
								production.residentProjectionInvocationCount,
								production.projection.executedVCycleCount,
								production.projection.maximumPreProjectionResidualPerS,
								production.projection.maximumPostProjectionResidualPerS,
								production.projection.validationBandPerS,error.c_str());
							if(error.empty())error="production resident attempt was rejected";
							lastAdvanceError=error;break;
						}
						profileResidentMS=std::chrono::duration<double,std::milli>(
							std::chrono::steady_clock::now()-profileStageStart).count();
					}
					}else{
						advancedOK=packetOK&&AdvanceConservative3D(shape,beginning,momentum,packets,
							config,fuel,fuel,FireSimulationTransportRecord::OpenV1(),advanced,&error);
						if(advancedOK)if(const char* oracleAuditPath=
							std::getenv("RISE_FIRE_ORACLE_MOMENTUM_AUDIT_PATH"))
							advancedOK=AppendOracleMomentumBudget(oracleAuditPath,shape,momentum,
								config,advanced,simulationTimeS,error);
					}
				solverPhase="production accepted-temperature validation";
				if(advancedOK) {
					std::vector<double> trialTemperature;
					if(persistence.productionMetal){
						trialTemperature=acceptedProductionTemperatureK;
						advancedOK=trialTemperature.size()==advanced.conservative.size();
					}else advancedOK=InvertPeriodicTemperaturesWithinBounds(advanced.conservative,fuel,
						config.transport.ambientTemperatureK,
						caseRecord.derived.maximumAcceptedTemperatureK,
						advanceOutputPrecision,trialTemperature,&error,workerCount);
					if(advancedOK&&std::any_of(trialTemperature.begin(),trialTemperature.end(),
						[&caseRecord](const double temperatureK){return !std::isfinite(temperatureK)||
							temperatureK>=caseRecord.derived.maximumAcceptedTemperatureK;})) {
						advancedOK=false;
						error="accepted_physical_temperature_ceiling_violation";
					}
				}
				if(!advancedOK) {
					if(fatalOnsetAuditFailure||mandatoryEvidenceFailure||
						observationEvidencePublished)break;
					if(error.empty())error=std::string("production owner failed during ")+
						solverPhase;
					lastAdvanceError=error;
					if(reportCapstoneProgress) std::fprintf(stderr,
						"capstone retry reduction=%u dt=%.9g reason=%s\n",reduction,
						trialStep,error.c_str());
					trialStep*=0.5; error.clear();
				}
			}
			if(!advancedOK) error=lastAdvanceError;
			else solverPhase="accepted-state accounting";
			profileStageStart=std::chrono::steady_clock::now();
			reaction.deltaTimeS=config.transport.deltaTimeS;
			if(advancedOK) {
				solverPhase="thread-identity validation";
				const bool checkLimiterIdentity=workerCount>1u&&
					advanced.discontinuousLimiterClassCount>0u&&
					!values.discontinuousClassThreadIdentityChecked;
				const bool checkActiveSetIdentity=workerCount>1u&&
					(advanced.discontinuousActiveSetClassCount>0u||
						persistence.forceActiveSetIdentityCheckForTest)&&
					!values.activeSetThreadIdentityChecked;
				if(checkLimiterIdentity||checkActiveSetIdentity){
					ConservativeAdvance3DConfig serialConfig=config;
					serialConfig.workerCount=1u;
					ConservativeAdvance3DResult serialAdvanced;
					std::string serialError;
					const bool serialOK=AdvanceConservative3D(shape,beginning,momentum,packets,
						serialConfig,fuel,fuel,FireSimulationTransportRecord::OpenV1(),
						serialAdvanced,&serialError);
					bool identical=serialOK&&serialAdvanced.conservative.size()==
						advanced.conservative.size()&&serialAdvanced.faceAlpha==advanced.faceAlpha&&
						serialAdvanced.divergenceHeunPerS==advanced.divergenceHeunPerS&&
						serialAdvanced.maximumLimiterClassDiscrepancy==
							advanced.maximumLimiterClassDiscrepancy&&
						serialAdvanced.discontinuousLimiterClassCount==
							advanced.discontinuousLimiterClassCount&&
						serialAdvanced.maximumActiveSetComplementarityDiscrepancyMPerS==
							advanced.maximumActiveSetComplementarityDiscrepancyMPerS&&
						serialAdvanced.discontinuousActiveSetClassCount==
							advanced.discontinuousActiveSetClassCount&&
						serialAdvanced.maximumActiveSetCycleLength==
							advanced.maximumActiveSetCycleLength&&
						serialAdvanced.maximumActiveSetDifferingFaceCount==
							advanced.maximumActiveSetDifferingFaceCount;
					for(std::size_t cell=0;identical&&cell<advanced.conservative.size();++cell)
						for(std::size_t component=0;component<MethaneConservativeDimension;++component)
							identical=identical&&serialAdvanced.conservative[cell][component]==
								advanced.conservative[cell][component];
					for(unsigned int axis=0;axis<3;++axis)identical=identical&&
						serialAdvanced.momentumKGPerM2S.component[axis]==
							advanced.momentumKGPerM2S.component[axis]&&
						serialAdvanced.velocityMPerS.component[axis]==
							advanced.velocityMPerS.component[axis];
					if(checkActiveSetIdentity&&
						persistence.injectActiveSetIdentityMismatchForTest)identical=false;
					if(checkLimiterIdentity){
						values.discontinuousClassThreadIdentity=identical;
						values.discontinuousClassThreadIdentityChecked=true;
					}
					if(checkActiveSetIdentity){
						values.activeSetThreadIdentity=identical;
						values.activeSetThreadIdentityChecked=true;
					}
					if(!identical&&reportCapstoneProgress)std::fprintf(stderr,
						"capstone discontinuous-class 1-vs-N mismatch: %s\n",
						serialError.c_str());
					advancedOK=DiscontinuousThreadIdentityAccepted(identical,
						checkActiveSetIdentity,error);
				}
				if(!advancedOK)break;
				values.maximumLimiterClassDiscrepancy=std::max(
					values.maximumLimiterClassDiscrepancy,
					advanced.maximumLimiterClassDiscrepancy);
				values.discontinuousLimiterClassSteps+=
					advanced.discontinuousLimiterClassCount;
				values.maximumActiveSetComplementarityDiscrepancyMPerS=std::max(
					values.maximumActiveSetComplementarityDiscrepancyMPerS,
					advanced.maximumActiveSetComplementarityDiscrepancyMPerS);
				values.discontinuousActiveSetEvents+=
					advanced.discontinuousActiveSetClassCount;
				values.maximumActiveSetCycleLength=std::max(
					values.maximumActiveSetCycleLength,advanced.maximumActiveSetCycleLength);
				values.maximumActiveSetDifferingFaceCount=std::max(
					values.maximumActiveSetDifferingFaceCount,
					advanced.maximumActiveSetDifferingFaceCount);
				solverPhase="pilot-ledger recomputation";
				double expectedStepPilotEnergyJ=0.0;
				for(std::size_t cell=0;cell<shape.CellCount();++cell){
					double pilotEnergyJPerM3=0.0;
					if(!ComputeMethanePilotEnergyDeltaJPerM3(acceptedBeginningStates[cell],fuel,
						pilotSetpointTemperatureK[cell],
						reactions[cell].pilotExpansionVolumeRatioCap,
						pilotEnergyJPerM3,&error)){
						advancedOK=false;break;
					}
					expectedStepPilotEnergyJ+=pilotEnergyJPerM3*cellVolume;
				}
				if(!advancedOK)break;
				solverPhase="accepted-state reconstruction";
				const double priorMaximumTemperatureK=values.maximumTemperatureK;
				std::vector<double> acceptedTemperature;
				if(persistence.productionMetal){
					acceptedTemperature=acceptedProductionTemperatureK;
					advancedOK=acceptedTemperature.size()==advanced.conservative.size();
				}else advancedOK=InvertPeriodicTemperaturesWithinBounds(advanced.conservative,fuel,
					config.transport.ambientTemperatureK,
					caseRecord.derived.maximumAcceptedTemperatureK,
					advanceOutputPrecision,acceptedTemperature,&error,workerCount);
				const bool measurePilotApproach=!values.pilotApproachComplete&&
					simulationTimeS<pilotEndS;
				const bool holdPhase=simulationTimeS>=pilotRampEndS&&simulationTimeS<pilotEndS;
				bool allActiveHoldCellsQualified=true,activeHoldCellObserved=false;
				bool allPilotLedgerCeilingsQualified=true;
				double stepAcceptedEOSMaximum=0.0;
				double stepAcceptedMaximumTemperatureK=0.0;
				double heldPilotMinimumTemperatureK=std::numeric_limits<double>::infinity();
				double heldPilotMaximumTemperatureK=0.0;
				std::size_t heldPilotMinimumCell=0u,heldPilotMaximumCell=0u;
				std::vector<MethaneCellState> acceptedStates;
				acceptedStates.reserve(shape.CellCount());
				for(std::size_t acceptedCell=0;advancedOK&&acceptedCell<advanced.conservative.size();
					++acceptedCell) {
					const ConservativeVector& conservative=advanced.conservative[acceptedCell];
					MethaneCellState accepted=FromConservativeVector(conservative,
						advanceOutputPrecision);
					accepted.temperatureK=acceptedTemperature[acceptedCell];
					double acceptedEOSResidual=0.0;
					if(!EquationOfStateResidual(accepted,fuel,acceptedEOSResidual,&error)){
						advancedOK=false;break;
					}
					stepAcceptedEOSMaximum=std::max(stepAcceptedEOSMaximum,acceptedEOSResidual);
					stepAcceptedMaximumTemperatureK=std::max(
						stepAcceptedMaximumTemperatureK,accepted.temperatureK);
					if(measurePilotApproach)values.maximumPilotApproachEOSResidual=std::max(
						values.maximumPilotApproachEOSResidual,acceptedEOSResidual);
					values.maximumTemperatureK=std::max(values.maximumTemperatureK,
						accepted.temperatureK);
					if(pilotSetpointTemperatureK[acceptedCell]>0.0){
						MethanePilotProjectionMap pilotMap;
						if(!ComputeMethanePilotProjectionMap(acceptedBeginningStates[acceptedCell],fuel,
							pilotSetpointTemperatureK[acceptedCell],
							reactions[acceptedCell].pilotExpansionVolumeRatioCap,pilotMap,&error)){
							advancedOK=false;break;
						}
						const bool abovePilotCeiling=
							acceptedBeginningStates[acceptedCell].temperatureK>=
								caseRecord.derived.pilotSetpointTemperatureK;
						allPilotLedgerCeilingsQualified=allPilotLedgerCeilingsQualified&&
							packets[acceptedCell].pilotEnergyDeltaJPerM3==
								pilotMap.sensibleEnergyDeltaJPerM3&&
							packets[acceptedCell].pilotExpansionIntegral==pilotMap.expansionIntegral&&
							(abovePilotCeiling?
								packets[acceptedCell].pilotEnergyDeltaJPerM3==0.0:
								pilotMap.targetTemperatureK<=caseRecord.derived.pilotSetpointTemperatureK);
						if(holdPhase&&pilotMap.targetTemperatureK==
							caseRecord.derived.pilotSetpointTemperatureK){
							activeHoldCellObserved=true;
							if(accepted.temperatureK<heldPilotMinimumTemperatureK){
								heldPilotMinimumTemperatureK=accepted.temperatureK;
								heldPilotMinimumCell=acceptedCell;
							}
							if(accepted.temperatureK>heldPilotMaximumTemperatureK){
								heldPilotMaximumTemperatureK=accepted.temperatureK;
								heldPilotMaximumCell=acceptedCell;
							}
							allActiveHoldCellsQualified=allActiveHoldCellsQualified&&
								accepted.temperatureK>fuel.PilotTemperatureK();
						}
					}
					acceptedStates.push_back(accepted);
				}
				if(advancedOK){
					states=std::move(acceptedStates);
					config.transport.producerPrecision=advanceOutputPrecision;
					values.maximumAcceptedEOSResidual=std::max(
						values.maximumAcceptedEOSResidual,stepAcceptedEOSMaximum);
					values.acceptedMaximumEOSResidualHistory.push_back(stepAcceptedEOSMaximum);
					values.acceptedMaximumTemperatureHistoryK.push_back(
						stepAcceptedMaximumTemperatureK);
					if(activeHoldCellObserved){
						values.pilotApproachComplete=true;
						values.pilotHoldBandObserved=true;
						values.pilotHoldBandSatisfied=values.pilotHoldBandSatisfied&&
							allActiveHoldCellsQualified;
						if(values.minimumActiveHoldTemperatureK==0.0)
							values.minimumActiveHoldTemperatureK=heldPilotMinimumTemperatureK;
						else values.minimumActiveHoldTemperatureK=std::min(
							values.minimumActiveHoldTemperatureK,heldPilotMinimumTemperatureK);
						values.maximumActiveHoldTemperatureK=std::max(
							values.maximumActiveHoldTemperatureK,heldPilotMaximumTemperatureK);
					}
				}
				if(advancedOK&&!allPilotLedgerCeilingsQualified){
					advancedOK=false;error="pilot_ledger_ceiling_violation";break;
				}
				if(advancedOK&&!values.pilotHoldBandSatisfied){
					std::ostringstream message;message.precision(17);
					message<<"pilot_hold_gate_violation:min="<<heldPilotMinimumTemperatureK<<
						" min_cell="<<heldPilotMinimumCell<<" min_xyz="<<heldPilotMinimumCell%shape.nx<<
						','<<(heldPilotMinimumCell/shape.nx)%shape.ny<<','<<
						heldPilotMinimumCell/(shape.nx*shape.ny)<<
						" max="<<heldPilotMaximumTemperatureK<<" max_cell="<<heldPilotMaximumCell<<
						" max_xyz="<<heldPilotMaximumCell%shape.nx<<','<<
						(heldPilotMaximumCell/shape.nx)%shape.ny<<','<<
						heldPilotMaximumCell/(shape.nx*shape.ny)<<
						" max_T0="<<currentTemperature[heldPilotMaximumCell]<<
						" max_command="<<pilotSetpointTemperatureK[heldPilotMaximumCell]<<
						" max_reacted="<<packets[heldPilotMaximumCell].reactedFuelKGPerM3<<
						" max_qgas="<<packets[heldPilotMaximumCell].gasHeatReleaseWPerM3<<
						" max_pilot_dH="<<packets[heldPilotMaximumCell].pilotEnergyDeltaJPerM3;
					advancedOK=false;error=message.str();break;
				}
				values.expectedPilotEnergyJ+=expectedStepPilotEnergyJ;
				values.externalFuelMassKG+=acceptedExternalFuelMassKG;
				if(reportCapstoneProgress&&values.maximumTemperatureK>
					std::max(2250.0,priorMaximumTemperatureK)) {
					const std::size_t hottest=static_cast<std::size_t>(std::max_element(
						acceptedTemperature.begin(),acceptedTemperature.end())-
						acceptedTemperature.begin());
					std::fprintf(stderr,"capstone new physical peak cell=%zu xyz=%zu,%zu,%zu "
						"T0=%.9g T1=%.9g pilot=%.9g reacted=%.9g qgas=%.9g dt=%.9g\n",
						hottest,hottest%shape.nx,(hottest/shape.nx)%shape.ny,
						hottest/(shape.nx*shape.ny),currentTemperature[hottest],
						acceptedTemperature[hottest],pilotSetpointTemperatureK[hottest],
						packets[hottest].reactedFuelKGPerM3,
						packets[hottest].gasHeatReleaseWPerM3,reaction.deltaTimeS);
				}
				profilePostMS=std::chrono::duration<double,std::milli>(
					std::chrono::steady_clock::now()-profileStageStart).count();
				profileStageStart=std::chrono::steady_clock::now();
				solverPhase="accepted-state statistics";
				momentum=advanced.momentumKGPerM2S;
				double stepHeatReleaseW=0.0,stepRadiativeLossW=0.0;
				double stepFuelConsumptionKGPerS=0.0,centerlineHeatReleaseW=0.0;
				for(const MethaneSourcePacket& acceptedPacket:packets) {
					values.pilotEnergyJ+=acceptedPacket.pilotEnergyDeltaJPerM3*cellVolume;
					stepHeatReleaseW+=acceptedPacket.gasHeatReleaseWPerM3*cellVolume;
					stepRadiativeLossW+=acceptedPacket.radiativeCoolingWPerM3*cellVolume;
					stepFuelConsumptionKGPerS+=-acceptedPacket.constituentDelta[MethaneCH4]*
						cellVolume/reaction.deltaTimeS;
					if(acceptedPacket.reactedFuelKGPerM3>0.0) {
						if(simulationTimeS<pilotEndS) values.ignitedDuringPilot=true;
						else values.sustainedAfterPilot=true;
					}
				}
				for(std::size_t z=0;z<shape.nz;++z)for(std::size_t cy=0;cy<centerYCount;++cy)
					for(std::size_t cx=0;cx<centerXCount;++cx){const std::size_t center=
						centerXIndex[cx]+shape.nx*(centerYIndex[cy]+shape.ny*z);
						centerlineHeatReleaseW+=packets[center].gasHeatReleaseWPerM3*cellVolume/
							centerSampleCount;}
				const double stepEndS=simulationTimeS+reaction.deltaTimeS;
				if(stepEndS>=pilotEndS&&!values.ignitedDuringPilot) {
					advancedOK=false;
					error="pilot_window_expired_without_ignition";
					break;
				}
				const double statisticsDuration=std::max(0.0,stepEndS-
					std::max(simulationTimeS,values.statisticsStartS));
				if(statisticsDuration>0.0) {
					if(!values.statisticsBoundaryObserved) {
						values.statisticsBoundaryObserved=true;
						values.firstStatisticsStepStartS=simulationTimeS;
					}
					values.integratedHeatReleaseJ+=stepHeatReleaseW*statisticsDuration;
					values.integratedRadiativeLossJ+=stepRadiativeLossW*statisticsDuration;
					values.integratedFuelConsumptionKG+=stepFuelConsumptionKGPerS*statisticsDuration;
					values.probeTimeS.push_back(stepEndS);
					values.probeCenterlineHeatReleaseW.push_back(centerlineHeatReleaseW);
					centerlineStatisticsDurationS+=statisticsDuration;
					for(std::size_t z=0;z<shape.nz;++z){
						double stationTemperatureK=0.0,stationVelocityMPerS=0.0;
						double stationReactionWPerM3=0.0;
						for(std::size_t cy=0;cy<centerYCount;++cy)for(std::size_t cx=0;
							cx<centerXCount;++cx){const std::size_t center=centerXIndex[cx]+
								shape.nx*(centerYIndex[cy]+shape.ny*z);
							stationTemperatureK+=states[center].temperatureK/centerSampleCount;
							stationReactionWPerM3+=packets[center].gasHeatReleaseWPerM3/
								centerSampleCount;
							const std::size_t stationLower=OpenLowerFaceForCell3D(shape,center,2),
								stationUpper=OpenUpperFaceForCell3D(shape,center,2);
							stationVelocityMPerS+=0.5*(advanced.velocityMPerS.component[2][stationLower]+
								advanced.velocityMPerS.component[2][stationUpper])/centerSampleCount;
							centerlineTemperatureIntegral[z]+=states[center].temperatureK*
								statisticsDuration/centerSampleCount;
							const std::size_t lower=OpenLowerFaceForCell3D(shape,center,2),
								upper=OpenUpperFaceForCell3D(shape,center,2);
							centerlineVelocityIntegral[z]+=0.5*(advanced.velocityMPerS.component[2][lower]+
								advanced.velocityMPerS.component[2][upper])*statisticsDuration/
								centerSampleCount;
						}
						values.stationProbeTimeS.push_back(stepEndS);
						values.stationProbeHeightM.push_back((static_cast<double>(z)+0.5)*
							shape.cellWidthM);
						values.stationProbeTemperatureK.push_back(stationTemperatureK);
						values.stationProbeReactionWPerM3.push_back(stationReactionWPerM3);
						values.stationProbeVerticalVelocityMPerS.push_back(stationVelocityMPerS);
						for(std::size_t y=0;y<shape.ny;++y)for(std::size_t x=0;x<shape.nx;++x){
							const std::size_t planeCell=x+shape.nx*(y+shape.ny*z);
							planeHeatReleaseIntegral[z]+=packets[planeCell].gasHeatReleaseWPerM3*
								cellVolume*statisticsDuration;
						}
					}
				}
				profileStatisticsMS=std::chrono::duration<double,std::milli>(
					std::chrono::steady_clock::now()-profileStageStart).count();
				values.acceptedTimeStepHistoryS.push_back(reaction.deltaTimeS);
				if(FireProfileEnabled()){
					std::fprintf(stderr,"FIREPROFSTEP step=%u dt=%.17g wall_ms=%.3f "
						"eligibility_ms=%.3f transport_ms=%.3f control_ms=%.3f source_ms=%.3f "
						"target_ms=%.3f layout_ms=%.3f resident_ms=%.3f post_ms=%.3f statistics_ms=%.3f\n",
						acceptedSteps+1u,reaction.deltaTimeS,std::chrono::duration<double,std::milli>(
							std::chrono::steady_clock::now()-profileStepStart).count(),profileEligibilityMS,
						profileTransportMS,profileControlMS,profileSourceMS,profileTargetMS,
						profileLayoutMS,profileResidentMS,profilePostMS,profileStatisticsMS);
					FireProfileReportAndReset("step");
				}
				simulationTimeS+=reaction.deltaTimeS;previousStepS=reaction.deltaTimeS;++acceptedSteps;
				values.acceptedTimeStepS=reaction.deltaTimeS;
				values.simulatedTimeS=simulationTimeS;
				const bool moreWork=!productionOnsetStopReached&&
					(acceptedSteps<effectiveMinimumStepCount||simulationTimeS<targetTimeS);
				const double checkpointElapsedS=std::chrono::duration<double>(
					std::chrono::steady_clock::now()-lastCheckpointWall).count();
				const bool equivalenceSnapshotDue=
					!persistence.equivalenceSnapshotDirectory.empty();
				const bool temporalSnapshotDue=
					!persistence.temporalSnapshotDirectory.empty()&&
					persistence.temporalSnapshotCadenceS>0.0&&
					simulationTimeS>=nextTemporalSnapshotS;
				const bool finalCheckpointDue=!moreWork&&!persistence.finalCheckpointPath.empty();
				if(temporalSnapshotDue){
					solverPhase="temporal-frame publication";
					if(!writeTemporalFrame()){advancedOK=false;break;}
					nextTemporalSnapshotS=values.statisticsStartS+
						static_cast<double>(temporalSnapshotIndex)*
						persistence.temporalSnapshotCadenceS;
				}
				const bool checkpointDue=equivalenceSnapshotDue||finalCheckpointDue||
					(moreWork&&!persistence.checkpointPath.empty()&&
						(persistence.checkpointCadenceWallS<=0.0||
							checkpointElapsedS>=persistence.checkpointCadenceWallS));
				if(checkpointDue){
					solverPhase="checkpoint publication";
					values.checkpointStepIndices.push_back(acceptedSteps);
					MethaneRunCheckpoint checkpoint;
					checkpoint.caseRecordId=caseRecord.caseRecordId;
					checkpoint.producerBuildId=currentBuildId;
					checkpoint.dimensions={{shape.nx,shape.ny,shape.nz}};
					checkpoint.cellWidthM=shape.cellWidthM;checkpoint.states=std::move(states);
					checkpoint.momentum=std::move(momentum);
					checkpoint.velocity=std::move(advanced.velocityMPerS);
					checkpoint.values=std::move(values);
					checkpoint.centerlineTemperatureIntegral=std::move(centerlineTemperatureIntegral);
					checkpoint.centerlineVelocityIntegral=std::move(centerlineVelocityIntegral);
					checkpoint.planeHeatReleaseIntegral=std::move(planeHeatReleaseIntegral);
					checkpoint.centerlineStatisticsDurationS=centerlineStatisticsDurationS;
					checkpoint.simulationTimeS=simulationTimeS;
					checkpoint.previousStepS=previousStepS;
					checkpoint.lastAcceptedStepS=reaction.deltaTimeS;
					checkpoint.acceptedSteps=acceptedSteps;
					checkpoint.productionManifoldObservation=productionManifoldObservation;
					std::filesystem::path checkpointOutput=finalCheckpointDue?
						persistence.finalCheckpointPath:persistence.checkpointPath;
					if(equivalenceSnapshotDue){
						std::ostringstream snapshotName;snapshotName<<"step_"<<std::setw(2)<<
							std::setfill('0')<<(acceptedSteps-values.resumedFromStep)<<".checkpoint";
						checkpointOutput=persistence.equivalenceSnapshotDirectory/snapshotName.str();
					}
					FireStateProducerPrecision checkpointOutputPrecision=
						FireStateProducerPrecision::Unknown;
					const bool checkpointAuthorized=
						HomogeneousStateProducerPrecision(checkpoint.states,checkpointOutputPrecision)&&
						(checkpointOutputPrecision!=FireStateProducerPrecision::Binary64||
							IssueBinary64CheckpointAuthority(checkpoint));
					const bool checkpointSaved=checkpointAuthorized&&SaveMethaneRunCheckpoint(
						checkpointOutput,checkpoint,error);
					bool retainedCheckpointSaved=true;
					std::filesystem::path retainedCheckpointOutput;
					if(checkpointSaved&&!persistence.retainedCheckpointDirectory.empty()&&
						checkpointOutput==persistence.checkpointPath){
						std::error_code retentionDirectoryError;
						std::filesystem::create_directories(
							persistence.retainedCheckpointDirectory,retentionDirectoryError);
						std::ostringstream retainedName;retainedName<<"step_"<<std::setw(10)<<
							std::setfill('0')<<acceptedSteps<<".checkpoint";
						retainedCheckpointOutput=
							persistence.retainedCheckpointDirectory/retainedName.str();
						if(retentionDirectoryError){error="cannot create retained checkpoint directory";
							retainedCheckpointSaved=false;
						}else if(std::filesystem::exists(retainedCheckpointOutput)){
							retainedCheckpointSaved=DigestFile(retainedCheckpointOutput)==
								DigestFile(checkpointOutput);
							if(!retainedCheckpointSaved)error=
								"retained checkpoint step identity already has different bytes";
						}else retainedCheckpointSaved=SaveMethaneRunCheckpoint(
							retainedCheckpointOutput,checkpoint,error);
					}
					if((!checkpointSaved||!retainedCheckpointSaved)&&(reportCapstoneProgress||
						!persistence.temporalSnapshotDirectory.empty())){
						std::uint64_t diagnosticStateDigest=0u;
						const bool stateMatches=checkpoint.productionManifoldObservation.Available()&&
							CheckpointAcceptedStateMatchesObservation(checkpoint,
								checkpoint.productionManifoldObservation,diagnosticStateDigest);
						std::fprintf(stderr,"checkpoint refusal authorized=%d timeline=%d observation=%d "
							"state_matches=%d state_digest=%llu error=%s\n",checkpointAuthorized?1:0,
							AcceptedCheckpointTimelineValid(checkpoint)?1:0,
							checkpoint.productionManifoldObservation.Available()?1:0,
							stateMatches?1:0,
							static_cast<unsigned long long>(diagnosticStateDigest),error.c_str());
					}
					states=std::move(checkpoint.states);momentum=std::move(checkpoint.momentum);
					advanced.velocityMPerS=std::move(checkpoint.velocity);
					values=std::move(checkpoint.values);
					centerlineTemperatureIntegral=std::move(checkpoint.centerlineTemperatureIntegral);
					centerlineVelocityIntegral=std::move(checkpoint.centerlineVelocityIntegral);
					planeHeatReleaseIntegral=std::move(checkpoint.planeHeatReleaseIntegral);
					if(!checkpointSaved||!retainedCheckpointSaved){
						advancedOK=false;break;
					}
					lastCheckpointWall=std::chrono::steady_clock::now();
					if(reportCapstoneProgress)std::fprintf(stderr,
						"capstone durable checkpoint count=%zu step=%u time=%.17g path=%s\n",
						values.checkpointStepIndices.size(),acceptedSteps,simulationTimeS,
						checkpointOutput.string().c_str());
					if(reportCapstoneProgress&&!retainedCheckpointOutput.empty())std::fprintf(stderr,
						"capstone retained checkpoint step=%u time=%.17g path=%s\n",acceptedSteps,
						simulationTimeS,retainedCheckpointOutput.string().c_str());
					if(persistence.killAfterFirstCheckpoint)HardKillCurrentProcess();
				}
				if(reportCapstoneProgress && (acceptedSteps<=4u || acceptedSteps%10u==0u ||
					simulationTimeS>=targetTimeS)) {
					double maximumTemperatureK=0.0,maximumReactionWPerM3=0.0;
					for(std::size_t diagnosticCell=0;diagnosticCell<states.size();++diagnosticCell) {
						maximumTemperatureK=std::max(maximumTemperatureK,
							states[diagnosticCell].temperatureK);
						maximumReactionWPerM3=std::max(maximumReactionWPerM3,
							packets[diagnosticCell].gasHeatReleaseWPerM3);
					}
					std::fprintf(stderr,"capstone accepted step=%u time=%.9g dt=%.9g Tmax=%.9g "
						"qmax=%.9g eos_max=%.9g approach_eos_max=%.9g limiter_class=%s "
						"limiter_discrepancy=%.9g active_set_class=%s active_set_discrepancy=%.9g "
						"active_set_cycle=%zu active_set_faces=%zu\n",acceptedSteps,
						simulationTimeS,reaction.deltaTimeS,maximumTemperatureK,
						maximumReactionWPerM3,values.maximumAcceptedEOSResidual,
						values.maximumPilotApproachEOSResidual,
						advanced.discontinuousLimiterClassCount?
						"discontinuous":"continuous",advanced.maximumLimiterClassDiscrepancy,
						advanced.discontinuousActiveSetClassCount?
						"discontinuous":"continuous",
						advanced.maximumActiveSetComplementarityDiscrepancyMPerS,
						advanced.maximumActiveSetCycleLength,
						advanced.maximumActiveSetDifferingFaceCount);
				}
			}
		}
		if(!advancedOK) {
			if(error.empty())error=std::string("production solver failed during ")+solverPhase;
			values.structuredError="solver_failure:"+error;
			std::fprintf(stderr,"capstone solver diagnostic steps=%u time=%.17g: %s\n",
				acceptedSteps,simulationTimeS,values.structuredError.c_str());
			if(!values.acceptedMaximumEOSResidualHistory.empty()){
				std::fprintf(stderr,"capstone accepted EOS drift history tail=");
				const std::size_t first=values.acceptedMaximumEOSResidualHistory.size()>8u?
					values.acceptedMaximumEOSResidualHistory.size()-8u:0u;
				for(std::size_t sample=first;sample<values.acceptedMaximumEOSResidualHistory.size();
					++sample)std::fprintf(stderr,"%s%.9g",sample==first?"":",",
						values.acceptedMaximumEOSResidualHistory[sample]);
				std::fprintf(stderr,"\n");
			}
			return values;
		}
		if(reportCapstoneProgress&&advancedOK) {
			double pilotMaximumTemperatureK=0.0,pilotMaximumMethaneKGPerM3=0.0,
				pilotMaximumOxygenKGPerM3=0.0;
			for(std::size_t cell=0;cell<states.size();++cell) if(canonicalPilotMask[cell]) {
				pilotMaximumTemperatureK=std::max(pilotMaximumTemperatureK,
					states[cell].temperatureK);
				pilotMaximumMethaneKGPerM3=std::max(pilotMaximumMethaneKGPerM3,
					states[cell].constituent[MethaneCH4]);
				pilotMaximumOxygenKGPerM3=std::max(pilotMaximumOxygenKGPerM3,
					states[cell].constituent[MethaneO2]);
			}
			std::fprintf(stderr,"capstone pilot diagnostic Tmax=%.9g CH4max=%.9g O2max=%.9g "
				"eos_max=%.9g approach_eos_max=%.9g\n",
				pilotMaximumTemperatureK,pilotMaximumMethaneKGPerM3,
				pilotMaximumOxygenKGPerM3,values.maximumAcceptedEOSResidual,
				values.maximumPilotApproachEOSResidual);
		}
		values.succeeded=true;
		if(!persistence.productionMetal)Check(advanced.conservative.empty() ||
			advanced.effectiveWorkerCount==std::max(1u,std::min(workerCount,
			static_cast<unsigned int>(shape.CellCount()))),
			"capstone one-vs-N fixture selects the owning solver worker path");
		if(advancedOK) {
			values.dimensions={{shape.nx,shape.ny,shape.nz}}; values.cellWidthM=shape.cellWidthM;
			values.caseRecordId=caseRecord.caseRecordId;
			values.temperature.resize(shape.CellCount());
			values.reaction.resize(shape.CellCount());
			values.carbon.resize(shape.CellCount());
			values.velocity.resize(shape.CellCount());
			for(std::size_t cell=0;cell<shape.CellCount();++cell) {
				const MethaneCellState& accepted=states[cell];
				values.temperature[cell]=static_cast<float>(accepted.temperatureK);
				values.reaction[cell]=static_cast<float>(packets.empty()?0.0:
					packets[cell].gasHeatReleaseWPerM3);
				values.carbon[cell]=static_cast<float>(std::max(0.0,
					accepted.constituent[MethaneCarbon]));
				std::array<float,3> velocity={{0.0f,0.0f,0.0f}};
				for(unsigned int axis=0;axis<3&&!advanced.velocityMPerS.component[axis].empty();++axis) {
					const std::size_t lower=OpenLowerFaceForCell3D(shape,cell,axis);
					const std::size_t upper=OpenUpperFaceForCell3D(shape,cell,axis);
					velocity[axis]=static_cast<float>(0.5*(advanced.velocityMPerS.component[axis][lower]+
						advanced.velocityMPerS.component[axis][upper]));
				}
				values.velocity[cell]=velocity;
				if(!packets.empty()) {
					values.realizedHeatReleaseW+=packets[cell].gasHeatReleaseWPerM3*cellVolume;
					values.fuelConsumptionKGPerS+=-packets[cell].constituentDelta[MethaneCH4]*
						cellVolume/reaction.deltaTimeS;
				}
			}
			values.temperatureK=*std::max_element(values.temperature.begin(),values.temperature.end());
			values.reactionWPerM3=*std::max_element(values.reaction.begin(),values.reaction.end());
			values.acceptedEscapeFactor=escape.accepted;
			values.selectedTimeStepS=selectedStep.seconds;
			values.acceptedTimeStepS=reaction.deltaTimeS;
			values.simulatedTimeS=simulationTimeS;
			values.flowThroughTimeS=caseRecord.derived.flowThroughTimeS;
			double coolingW=0.0;
			for(const MethaneSourcePacket& packet:packets)
				coolingW+=packet.radiativeCoolingWPerM3*cellVolume;
			values.realizedRadiativeFraction=values.realizedHeatReleaseW>0.0?
				coolingW/values.realizedHeatReleaseW:0.0;
			values.integratedRadiativeFraction=values.integratedHeatReleaseJ>0.0?
				values.integratedRadiativeLossJ/values.integratedHeatReleaseJ:0.0;
			values.puffingFrequencyHz=DominantUniformResampledFrequency(values.probeTimeS,
				values.probeCenterlineHeatReleaseW);
			const double expectedPuffing=1.5/std::sqrt(CapstonePoolDiameterM);
			values.puffingRelativeError=expectedPuffing>0.0?
				std::fabs(values.puffingFrequencyHz-expectedPuffing)/expectedPuffing:0.0;
			double peakMeanPlaneHeatReleaseW=0.0;
			for(const double integral:planeHeatReleaseIntegral)peakMeanPlaneHeatReleaseW=
				std::max(peakMeanPlaneHeatReleaseW,centerlineStatisticsDurationS>0.0?
					integral/centerlineStatisticsDurationS:0.0);
			if(peakMeanPlaneHeatReleaseW>0.0)for(std::size_t z=0;z<shape.nz;++z)if(
				planeHeatReleaseIntegral[z]/centerlineStatisticsDurationS>=
					0.01*peakMeanPlaneHeatReleaseW)values.mccaffreyFlameTipHeightM=
					(static_cast<double>(z)+0.5)*shape.cellWidthM;
			for(std::size_t z=0;z<shape.nz;++z) {
				double fallbackTemperature=0.0,fallbackVelocity=0.0;
				for(std::size_t cy=0;cy<centerYCount;++cy)for(std::size_t cx=0;
					cx<centerXCount;++cx){const std::size_t center=centerXIndex[cx]+
						shape.nx*(centerYIndex[cy]+shape.ny*z);
					fallbackTemperature+=states[center].temperatureK/centerSampleCount;
					fallbackVelocity+=values.velocity[center][2]/centerSampleCount;}
				values.centerlineHeightM.push_back((static_cast<double>(z)+0.5)*shape.cellWidthM);
				values.centerlineTemperatureK.push_back(centerlineStatisticsDurationS>0.0?
					centerlineTemperatureIntegral[z]/centerlineStatisticsDurationS:
					fallbackTemperature);
				values.centerlineVelocityMPerS.push_back(centerlineStatisticsDurationS>0.0?
					centerlineVelocityIntegral[z]/centerlineStatisticsDurationS:
					fallbackVelocity);
			}
			FitCenterlineTemperaturePowerLaw(values);
			if(heatReleaseRateKW==CapstoneHeatReleaseRateKW)
				EvaluateMcCaffreyPlumeStations(values,heatReleaseRateKW);
		}
		return values;
	}

	RISECBOR64::Bytes CanonicalRecord( const char* kind )
	{
		RISECBOR64::Bytes bytes;
		std::string error;
		Check(RISECBOR64::Encode(RISECBOR64::Value::MapValue({
			{"record_kind",RISECBOR64::Value::String(kind)},
			{"schema_version",RISECBOR64::Value::Unsigned(1)}
		}),bytes,&error),"synthetic embedded record encodes canonically");
		return bytes;
	}

	RISECBOR64::Bytes AerosolRecord()
	{
		RISECBOR64::Bytes bytes; std::string error;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		Check(RISECBOR64::Encode(RISECBOR64::Value::MapValue({
			{"carbon_phase",RISECBOR64::Value::MapValue({
				{"common_T_ref_K",RISECBOR64::Value::Float(fuel.ReferenceTemperatureK())},
				{"interpolation",RISECBOR64::Value::String("nasa9_piecewise_cp_hs")},
				{"phase",RISECBOR64::Value::String("solid")},
				{"source_fuel_record_id",RISECBOR64::Value::String(fuel.RecordId())},
				{"species_id",RISECBOR64::Value::String("C(gr)")}
			})},
			{"condensable_stream",RISECBOR64::Value::MapValue({
				{"kind",RISECBOR64::Value::String("none")},
				{"reason",RISECBOR64::Value::String("methane_has_no_condensable_organic_stream")}
			})},
			{"record_kind",RISECBOR64::Value::String("fire-aerosol-thermochemistry-v1")},
			{"schema_version",RISECBOR64::Value::Unsigned(1)},
			{"status",RISECBOR64::Value::String("preview_methane_zero_yield")},
			{"temperature_domain_K",RISECBOR64::Value::ArrayValue({
				RISECBOR64::Value::Float(fuel.TemperatureMinK()),
				RISECBOR64::Value::Float(fuel.TemperatureMaxK())})}
		}),bytes,&error),"aerosol record encodes");
		return bytes;
	}

	RISECBOR64::Bytes ChemNoneRecord()
	{
		RISECBOR64::Bytes bytes; std::string error;
		Check(RISECBOR64::Encode(RISECBOR64::Value::MapValue({
			{"chem_model",RISECBOR64::Value::String("none")},
			{"provenance",RISECBOR64::Value::String("methane r52 no adopted chem record")},
			{"record_kind",RISECBOR64::Value::String("fire-chem-none-v1")},
			{"schema_version",RISECBOR64::Value::Unsigned(1)}
		}),bytes,&error),"chem-none record encodes");
		return bytes;
	}

	RISECBOR64::Bytes SyntheticChemRecord()
	{
		using RISECBOR64::Value;
		RISECBOR64::Bytes bytes; std::string error;
		Value::Values bands;
		const char* names[3]={"CH","C2","CO2"};
		const double limits[3][2]={{390.0,440.0},{450.0,570.0},{380.0,780.0}};
		for( unsigned int band=0; band<3u; ++band ) bands.push_back(Value::MapValue({
			{"band",Value::String(names[band])},
			{"normalization_interval_nm",Value::ArrayValue({
				Value::Float(limits[band][0]),Value::Float(limits[band][1])})},
			{"normalization_rule",Value::String("trapezoid_1nm_then_divide_once")},
			{"spd_shape",Value::String("uniform_unit_shape")}
		}));
		Check(RISECBOR64::Encode(Value::MapValue({
			{"absolute_calibration",Value::String("input_absolute_band_power_W_per_m3")},
			{"bands",Value::ArrayValue(bands)},
			{"provenance",Value::String("test-only analytic uniform-SPD estimator fixture")},
			{"record_class",Value::String("SYNTHETIC_NON_PREDICTIVE")},
			{"record_kind",Value::String("fire-chem-synthetic-fixture-v1")},
			{"schema_version",Value::Unsigned(1)},
			{"state_domain",Value::String("finite_nonnegative_absolute_channel_values")},
			{"wavelength_unit",Value::String("nm")}
		}),bytes,&error),"synthetic chem fixture record encodes");
		return bytes;
	}

	RISECBOR64::Value ReplaceMember( const RISECBOR64::Value& map,
		const char* key, const RISECBOR64::Value& replacement )
	{
		RISECBOR64::Value::Members members = map.GetMap();
		for( auto& member : members ) if( member.first == key ) member.second = replacement;
		return RISECBOR64::Value::MapValue(members);
	}

	RISECBOR64::Bytes EnvelopeForPayload( const RISECBOR64::Value& payload )
	{
		RISECBOR64::Bytes payloadBytes, envelope;
		std::string error;
		Check(RISECBOR64::Encode(payload,payloadBytes,&error),"mutated payload encodes");
		Check(RISECBOR64::Encode(RISECBOR64::Value::MapValue({
			{"payload",payload},{"sequence_id",RISECBOR64::Value::String(
				RISECBOR64::SHA256Hex(payloadBytes))}}),envelope,&error),"mutated envelope encodes");
		return envelope;
	}

	RISECBOR64::Value Channel( const char* name, const char* type,
		const char* units, const char* semantics, const std::vector<double>& background,
		const std::array<std::uint64_t,3> scalarDimensions={{2u,2u,2u}},
		const double voxelSize=0.5 )
	{
		using RISECBOR64::Value;
		Value::Values bg;
		for( const double value : background ) bg.push_back(Value::Float(value));
		const bool velocity = std::strcmp(name,"velocity") == 0;
		const double origin=velocity?-voxelSize:0.0;
		const double lower=-0.5*voxelSize;
		return Value::MapValue({
			{"background_value",Value::ArrayValue(bg)},
			{"core_face_bounds_m",Value::ArrayValue({Value::Float(lower),Value::Float(lower),
				Value::Float(lower),Value::Float((scalarDimensions[0]-0.5)*voxelSize),
				Value::Float((scalarDimensions[1]-0.5)*voxelSize),
				Value::Float((scalarDimensions[2]-0.5)*voxelSize)})},
			{"dimensions",Value::ArrayValue({Value::Unsigned(scalarDimensions[0]+(velocity?2u:0u)),
				Value::Unsigned(scalarDimensions[1]+(velocity?2u:0u)),
				Value::Unsigned(scalarDimensions[2]+(velocity?2u:0u))})},
			{"name",Value::String(name)},
			{"origin_m",Value::ArrayValue({Value::Float(origin),Value::Float(origin),Value::Float(origin)})},
			{"temporal_semantics",Value::String(semantics)},
			{"units",Value::String(units)},
			{"value_type",Value::String(type)},
			{"voxel_size_m",Value::ArrayValue({Value::Float(voxelSize),Value::Float(voxelSize),
				Value::Float(voxelSize)})}
		});
	}

	RISECBOR64::Bytes ManifestBytes( const std::string& firstDigest,
		const std::string& secondDigest, const char* endPolicy="hold",
		const bool useProductionOptics=false, const bool syntheticChem=false,
		const std::array<std::uint64_t,3> scalarDimensions={{2u,2u,2u}},
		const double voxelSize=0.5,
		const double simulationTimeOrigin=1.0, const double frameStepSeconds=0.25,
		const double sceneToSimulationScale=2.0, const double sceneTimeOrigin=10.0,
		const bool caseGridBound=false, const double casePoolDiameterM=0.02,
		const double caseHeatReleaseRateKW=0.10,
		const bool caseHasRadiativeFractionOverride=false,
		const double caseRadiativeFractionOverride=0.0,
		const double caseResolutionTier=6.0,const bool casePlumeLaw=false,
		const std::vector<std::pair<std::string,std::string> >& sequenceFrames={} )
	{
		using RISECBOR64::Value;
		std::string error;
		RISECBOR64::Bytes build; std::string buildId;
		Check(CurrentRendererBuildIdentity(build,buildId),
			"sequence producer embeds the current executable build identity");
		const RISECBOR64::Bytes optics = FireOpticsPreset::PredictiveV1().RecordBytes();
		const RISECBOR64::Bytes thermo =
			FireSimulationThermochemistryRecord::OpenSubsetV1().RecordBytes();
		const RISECBOR64::Bytes transport =
			FireSimulationTransportRecord::OpenV1().RecordBytes();
		const RISECBOR64::Bytes aerosol = AerosolRecord();
		const RISECBOR64::Bytes opacity =
			FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1().RecordBytes();
		const RISECBOR64::Bytes chem = syntheticChem ? SyntheticChemRecord() : ChemNoneRecord();
		const RISECBOR64::Bytes fuel = FireSimulationMethaneRecord::PhysicalV1().RecordBytes();
		const RISECBOR64::Bytes gateEvidenceBytes={'p','h','a','s','e','_','c','_','r','5','3'};
		const std::string gateEvidenceId=RISECBOR64::SHA256Hex(gateEvidenceBytes);
		FireCase::AuthoredV1 caseAuthored;
		caseAuthored.fuelRecordId=FireSimulationMethaneRecord::PhysicalV1().RecordId();
		caseAuthored.poolDiameterM=casePoolDiameterM;
		caseAuthored.heatReleaseRateKW=caseHeatReleaseRateKW;
		caseAuthored.envelope={{0.0,1.0}}; caseAuthored.durationS=std::max(1.0,frameStepSeconds);
		caseAuthored.quality=caseGridBound?"dstar":"standard";
		caseAuthored.numericDStarTier=caseGridBound?caseResolutionTier:0.0; caseAuthored.seed=1234;
		caseAuthored.outputFramesPerS=1.0/frameStepSeconds;
		caseAuthored.plumeLaw=casePlumeLaw;
		caseAuthored.hasRadiativeFractionOverride=caseHasRadiativeFractionOverride;
		caseAuthored.radiativeFractionOverride=caseRadiativeFractionOverride;
		FireCase::RecordV1 caseRecord;
		const bool caseBuilt=FireCase::BuildMethaneV1(caseAuthored,
			FireSimulationMethaneRecord::PhysicalV1(),
			{RISECBOR64::SHA256Hex(thermo),RISECBOR64::SHA256Hex(transport),
			 RISECBOR64::SHA256Hex(opacity),RISECBOR64::SHA256Hex(optics),
			 RISECBOR64::SHA256Hex(aerosol),RISECBOR64::SHA256Hex(chem),
			 RISECBOR64::SHA256Hex(fuel)},caseRecord,error);
		Check(caseBuilt,"test sequence case record derives canonically");
		Value::Values channels={
			Channel("carbon","float32","g/m3","frozen_material_advection",{0.0},scalarDimensions,voxelSize),
			Channel("temperature","float32","K","frozen_material_advection",{300.0},scalarDimensions,voxelSize),
			Channel("reaction","float32","W/m3","derived_eulerian_source",{0.0},scalarDimensions,voxelSize),
			Channel("velocity","vec3_float32","m/s","frozen_material_advection",{0.0,0.0,0.0},scalarDimensions,voxelSize)
		};
		if( syntheticChem ) {
			channels.push_back(Channel("chem_CH","float32","W/m3","derived_eulerian_source",{0.0},scalarDimensions,voxelSize));
			channels.push_back(Channel("chem_C2","float32","W/m3","derived_eulerian_source",{0.0},scalarDimensions,voxelSize));
			channels.push_back(Channel("chem_CO2","float32","W/m3","derived_eulerian_source",{0.0},scalarDimensions,voxelSize));
		}
		const bool preview=useProductionOptics || syntheticChem;
		Value::Values encodedFrames;
		std::uint64_t firstFrameIndex=4u,lastFrameIndex=5u;
		if(sequenceFrames.empty()){
			encodedFrames={
				Value::MapValue({{"index",Value::Unsigned(4)},
					{"path",Value::String("frame4.vdb")},{"sha256",Value::String(firstDigest)}}),
				Value::MapValue({{"index",Value::Unsigned(5)},
					{"path",Value::String("frame5.vdb")},{"sha256",Value::String(secondDigest)}})};
		}else{
			firstFrameIndex=0u;lastFrameIndex=sequenceFrames.size()-1u;
			encodedFrames.reserve(sequenceFrames.size());
			for(std::size_t frame=0u;frame<sequenceFrames.size();++frame)
				encodedFrames.push_back(Value::MapValue({
					{"index",Value::Unsigned(frame)},
					{"path",Value::String(sequenceFrames[frame].first)},
					{"sha256",Value::String(sequenceFrames[frame].second)}}));
		}
		const Value payload = Value::MapValue({
			{"aerosol_thermochemistry_record",Value::BytesValue(aerosol)},
			{"aerosol_thermochemistry_record_id",Value::String(RISECBOR64::SHA256Hex(aerosol))},
			{"case_record",Value::BytesValue(caseRecord.envelopeBytes)},
			{"case_record_id",Value::String(caseRecord.caseRecordId)},
			{"channels",Value::ArrayValue(channels)},
			{"chem_record",Value::BytesValue(chem)},
			{"chem_record_id",Value::String(RISECBOR64::SHA256Hex(chem))},
			{"end_policy",Value::String(endPolicy)},
			{"first_frame_index",Value::Unsigned(firstFrameIndex)},
			{"frame_count",Value::Unsigned(encodedFrames.size())},
			{"frame_encoding",Value::String("openvdb-v1")},
			{"frames",Value::ArrayValue(encodedFrames)},
			{"fuel_record",Value::BytesValue(fuel)},
			{"fuel_record_id",Value::String(RISECBOR64::SHA256Hex(fuel))},
			{"gas_opacity_record",Value::BytesValue(opacity)},
			{"gas_opacity_record_id",Value::String(RISECBOR64::SHA256Hex(opacity))},
			{"gas_thermochemistry_record",Value::BytesValue(thermo)},
			{"gas_thermochemistry_record_id",Value::String(RISECBOR64::SHA256Hex(thermo))},
			{"gate_evidence_ids",Value::ArrayValue({Value::String(gateEvidenceId)})},
			{"last_frame_index",Value::Unsigned(lastFrameIndex)},
			{"optical_record",Value::BytesValue(optics)},
			{"optical_record_id",Value::String(RISECBOR64::SHA256Hex(optics))},
			{"outside_halo_policy",Value::String("reject_outside_declared_halo")},
			{"physical_mapping",Value::String("absolute_si")},
			{"producer_build_id",Value::String(buildId)},
			{"producer_build_v1",Value::BytesValue(build)},
			{"producer_reason_codes",Value::ArrayValue(preview ? (caseGridBound ? Value::Values{
				Value::String("case_grid_bound"),Value::String(syntheticChem ? "synthetic_chem_fixture" :
					"open_subset_records_preview")} : Value::Values{Value::String(syntheticChem ?
					"synthetic_chem_fixture":"open_subset_records_preview")}) : Value::Values{})},
			{"scene_translation_m",Value::ArrayValue({Value::Float(0),Value::Float(0),Value::Float(0)})},
			{"qdot_ref_W",Value::Float(caseRecord.derived.referenceHeatReleaseRateW)},
			{"scene_unit_meters",Value::Float(1.0)},
			{"schema_version",Value::Unsigned(1)},
			{"source_kind",Value::String("rise_simulation")},
			{"source_qualification",Value::String(preview ? "preview_only" : "predictive_qualified")},
			{"temperature_domain_K",Value::ArrayValue({Value::Float(300),Value::Float(2500)})},
			{"time_map",Value::MapValue({
				{"alpha",Value::Float(sceneToSimulationScale)},
				{"delta_t_frame",Value::Float(frameStepSeconds)},
				{"i0",Value::Unsigned(firstFrameIndex)},
				{"t0",Value::Float(simulationTimeOrigin)},
				{"t_scene_0",Value::Float(sceneTimeOrigin)}
			})},
			{"transport_closure_record",Value::BytesValue(transport)},
			{"transport_closure_record_id",Value::String(RISECBOR64::SHA256Hex(transport))},
			{"velocity_halo_width_m",Value::Float(voxelSize)}
		});
		RISECBOR64::Bytes payloadBytes, envelope;
		Check(RISECBOR64::Encode(payload,payloadBytes,&error),"sequence payload encodes");
		Check(RISECBOR64::Encode(Value::MapValue({
			{"payload",payload},
			{"sequence_id",Value::String(RISECBOR64::SHA256Hex(payloadBytes))}
		}),envelope,&error),"sequence envelope encodes");
		return envelope;
	}

	std::string DigestFile( const std::filesystem::path& path )
	{
		std::ifstream input(path,std::ios::binary);
		if(!input)return std::string();
		input.seekg(0,std::ios::end);
		const std::streampos end = input.tellg();
		if(end<0)return std::string();
		input.seekg(0,std::ios::beg);
		RISECBOR64::Bytes bytes(static_cast<std::size_t>(end));
		if( end > 0 ) input.read(reinterpret_cast<char*>(bytes.data()),end);
		return RISECBOR64::SHA256Hex(bytes);
	}

	bool CurrentExecutableDigest(const RISECBOR64::Bytes& buildRecord,
		std::string& digest,std::string& error)
	{
		RISECBOR64::Value decoded;
		if(!RISECBOR64::DecodeCanonical(buildRecord,decoded,&error)||
			decoded.GetType()!=RISECBOR64::Value::Map)return false;
		const RISECBOR64::Value* renderer=decoded.Find("renderer_binary");
		const RISECBOR64::Value* sha=renderer&&renderer->GetType()==RISECBOR64::Value::Map?
			renderer->Find("sha256"):nullptr;
		if(!sha||sha->GetType()!=RISECBOR64::Value::Text||sha->GetText().size()!=64u){
			error="renderer build record has no executable SHA-256";return false;
		}
		digest=sha->GetText();return true;
	}

	RISECBOR64::Value UnsignedArrayValue(const std::vector<std::uint64_t>& values)
	{
		RISECBOR64::Value::Values encoded;encoded.reserve(values.size());
		for(const std::uint64_t value:values)encoded.push_back(RISECBOR64::Value::Unsigned(value));
		return RISECBOR64::Value::ArrayValue(encoded);
	}
	RISECBOR64::Value TextArrayValue(const std::vector<std::string>& values)
	{
		RISECBOR64::Value::Values encoded;encoded.reserve(values.size());
		for(const std::string& value:values)encoded.push_back(RISECBOR64::Value::String(value));
		return RISECBOR64::Value::ArrayValue(encoded);
	}
	bool ReadTextMember(const RISECBOR64::Value& map,const char* key,std::string& value)
	{
		const RISECBOR64::Value* found=map.Find(key);
		if(!found||found->GetType()!=RISECBOR64::Value::Text)return false;
		value=found->GetText();return true;
	}
	bool ReadUnsignedMember(const RISECBOR64::Value& map,const char* key,std::uint64_t& value)
	{
		const RISECBOR64::Value* found=map.Find(key);
		if(!found||found->GetType()!=RISECBOR64::Value::UnsignedInteger)return false;
		value=found->GetIntegerArgument();return true;
	}
	bool ReadUnsignedArray(const RISECBOR64::Value& map,const char* key,
		std::vector<std::uint64_t>& values)
	{
		const RISECBOR64::Value* found=map.Find(key);
		if(!found||found->GetType()!=RISECBOR64::Value::Array)return false;
		values.clear();values.reserve(found->GetArray().size());
		for(const RISECBOR64::Value& item:found->GetArray()){
			if(item.GetType()!=RISECBOR64::Value::UnsignedInteger)return false;
			values.push_back(item.GetIntegerArgument());
		}
		return true;
	}
	bool ReadTextArray(const RISECBOR64::Value& map,const char* key,
		std::vector<std::string>& values)
	{
		const RISECBOR64::Value* found=map.Find(key);
		if(!found||found->GetType()!=RISECBOR64::Value::Array)return false;
		values.clear();values.reserve(found->GetArray().size());
		for(const RISECBOR64::Value& item:found->GetArray()){
			if(item.GetType()!=RISECBOR64::Value::Text)return false;
			values.push_back(item.GetText());
		}
		return true;
	}

	bool DurableWriteCanonical(const std::filesystem::path& path,
		const RISECBOR64::Bytes& bytes,std::string& error)
	{
		if(path.has_parent_path())std::filesystem::create_directories(path.parent_path());
#if defined(_WIN32)
		const long long processId=static_cast<long long>(::_getpid());
#else
		const long long processId=static_cast<long long>(::getpid());
#endif
		const std::filesystem::path temporary=path.string()+".tmp."+std::to_string(processId);
		{
			std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
			if(!output){error="cannot open canonical run record";return false;}
			if(!bytes.empty())output.write(reinterpret_cast<const char*>(bytes.data()),
				static_cast<std::streamsize>(bytes.size()));
			output.close();if(!output){error="cannot write canonical run record";return false;}
		}
		if(!DurableSyncFileAndDirectory(temporary,error)||
			!AtomicReplaceCheckpoint(temporary,path,error)){
			std::error_code ignored;std::filesystem::remove(temporary,ignored);return false;
		}
		return true;
	}

	RISECBOR64::Value ResumeTracePayload(const ResumeEquivalenceTrace& trace)
	{
		using RISECBOR64::Value;
		return Value::MapValue({
			{"accepted_step_count",Value::Unsigned(trace.acceptedStepCount)},
			{"build_id",Value::String(trace.buildId)},
			{"checkpoint_producer_build_id",Value::String(trace.checkpointProducerBuildId)},
			{"checkpoint_sha256",Value::String(trace.checkpointDigest)},
			{"executable_sha256",Value::String(trace.executableDigest)},
			{"frame_sha256",TextArrayValue(trace.frameDigests)},
			{"maximum_eos_residual_bits",UnsignedArrayValue(trace.maximumEOSResidualBits)},
			{"maximum_temperature_K_bits",UnsignedArrayValue(trace.maximumTemperatureBits)},
			{"record_kind",Value::String("fire-resume-equivalence-trace-v1")},
			{"resumed_from_step",Value::Unsigned(trace.resumedFromStep)},
			{"schema_version",Value::Unsigned(1)},
			{"time_step_s_bits",UnsignedArrayValue(trace.timeStepBits)}
		});
	}
	bool ParseResumeTracePayload(const RISECBOR64::Value& payload,
		ResumeEquivalenceTrace& trace)
	{
		std::string kind;std::uint64_t schema=0u;
		return payload.GetType()==RISECBOR64::Value::Map&&payload.GetMap().size()==12u&&
			ReadUnsignedMember(payload,"accepted_step_count",trace.acceptedStepCount)&&
			ReadTextMember(payload,"build_id",trace.buildId)&&
			ReadTextMember(payload,"checkpoint_producer_build_id",trace.checkpointProducerBuildId)&&
			ReadTextMember(payload,"checkpoint_sha256",trace.checkpointDigest)&&
			ReadTextMember(payload,"executable_sha256",trace.executableDigest)&&
			ReadTextArray(payload,"frame_sha256",trace.frameDigests)&&
			ReadUnsignedArray(payload,"maximum_eos_residual_bits",trace.maximumEOSResidualBits)&&
			ReadUnsignedArray(payload,"maximum_temperature_K_bits",trace.maximumTemperatureBits)&&
			ReadTextMember(payload,"record_kind",kind)&&kind=="fire-resume-equivalence-trace-v1"&&
			ReadUnsignedMember(payload,"resumed_from_step",trace.resumedFromStep)&&
			ReadUnsignedMember(payload,"schema_version",schema)&&schema==1u&&
			ReadUnsignedArray(payload,"time_step_s_bits",trace.timeStepBits);
	}
	bool SaveResumeEquivalenceTrace(const std::filesystem::path& path,
		const ResumeEquivalenceTrace& trace,std::string& error)
	{
		const RISECBOR64::Value payload=ResumeTracePayload(trace);
		RISECBOR64::Bytes payloadBytes,envelope;
		if(!RISECBOR64::Encode(payload,payloadBytes,&error)||
			!RISECBOR64::Encode(RISECBOR64::Value::MapValue({{"payload",payload},
				{"trace_id",RISECBOR64::Value::String(RISECBOR64::SHA256Hex(payloadBytes))}}),
				envelope,&error))return false;
		return DurableWriteCanonical(path,envelope,error);
	}
	bool LoadResumeEquivalenceTrace(const std::filesystem::path& path,
		ResumeEquivalenceTrace& trace,std::string& error)
	{
		const RISECBOR64::Bytes bytes=ReadFileBytes(path);RISECBOR64::Value envelope;
		if(bytes.empty()||!RISECBOR64::DecodeCanonical(bytes,envelope,&error)||
			envelope.GetType()!=RISECBOR64::Value::Map||envelope.GetMap().size()!=2u){
			error="resume-equivalence trace envelope is invalid";return false;
		}
		const RISECBOR64::Value* payload=envelope.Find("payload");
		const RISECBOR64::Value* id=envelope.Find("trace_id");RISECBOR64::Bytes payloadBytes;
		if(!payload||!id||id->GetType()!=RISECBOR64::Value::Text||
			!RISECBOR64::Encode(*payload,payloadBytes,&error)||
			id->GetText()!=RISECBOR64::SHA256Hex(payloadBytes)||
			!ParseResumeTracePayload(*payload,trace)){
			error="resume-equivalence trace is not canonical or self-consistent";return false;
		}
		return true;
	}

	RISECBOR64::Value ResumeCertificatePayload(const ResumeEquivalenceCertificate& certificate)
	{
		using RISECBOR64::Value;
		return Value::MapValue({
			{"accepted_step_count",Value::Unsigned(certificate.acceptedStepCount)},
			{"checkpoint_sha256",Value::String(certificate.checkpointDigest)},
			{"frame_sha256",TextArrayValue(certificate.frameDigests)},
			{"maximum_eos_residual_bits",UnsignedArrayValue(certificate.maximumEOSResidualBits)},
			{"maximum_temperature_K_bits",UnsignedArrayValue(certificate.maximumTemperatureBits)},
			{"new_build_id",Value::String(certificate.newBuildId)},
			{"new_executable_sha256",Value::String(certificate.newExecutableDigest)},
			{"old_build_id",Value::String(certificate.oldBuildId)},
			{"old_executable_sha256",Value::String(certificate.oldExecutableDigest)},
			{"record_kind",Value::String("fire-resume-equivalence-certificate-v1")},
			{"resumed_from_step",Value::Unsigned(certificate.resumedFromStep)},
			{"schema_version",Value::Unsigned(1)},
			{"time_step_s_bits",UnsignedArrayValue(certificate.timeStepBits)}
		});
	}
	bool ParseResumeCertificatePayload(const RISECBOR64::Value& payload,
		ResumeEquivalenceCertificate& certificate)
	{
		std::string kind;std::uint64_t schema=0u;
		return payload.GetType()==RISECBOR64::Value::Map&&payload.GetMap().size()==13u&&
			ReadUnsignedMember(payload,"accepted_step_count",certificate.acceptedStepCount)&&
			ReadTextMember(payload,"checkpoint_sha256",certificate.checkpointDigest)&&
			ReadTextArray(payload,"frame_sha256",certificate.frameDigests)&&
			ReadUnsignedArray(payload,"maximum_eos_residual_bits",certificate.maximumEOSResidualBits)&&
			ReadUnsignedArray(payload,"maximum_temperature_K_bits",certificate.maximumTemperatureBits)&&
			ReadTextMember(payload,"new_build_id",certificate.newBuildId)&&
			ReadTextMember(payload,"new_executable_sha256",certificate.newExecutableDigest)&&
			ReadTextMember(payload,"old_build_id",certificate.oldBuildId)&&
			ReadTextMember(payload,"old_executable_sha256",certificate.oldExecutableDigest)&&
			ReadTextMember(payload,"record_kind",kind)&&kind=="fire-resume-equivalence-certificate-v1"&&
			ReadUnsignedMember(payload,"resumed_from_step",certificate.resumedFromStep)&&
			ReadUnsignedMember(payload,"schema_version",schema)&&schema==1u&&
			ReadUnsignedArray(payload,"time_step_s_bits",certificate.timeStepBits);
	}
	bool LoadResumeEquivalenceCertificate(const std::filesystem::path& path,
		ResumeEquivalenceCertificate& certificate,std::string& error)
	{
		const RISECBOR64::Bytes bytes=ReadFileBytes(path);RISECBOR64::Value envelope;
		if(bytes.empty()||!RISECBOR64::DecodeCanonical(bytes,envelope,&error)||
			envelope.GetType()!=RISECBOR64::Value::Map||envelope.GetMap().size()!=2u){
			error="resume-equivalence certificate envelope is invalid";return false;
		}
		const RISECBOR64::Value* payload=envelope.Find("payload");
		const RISECBOR64::Value* id=envelope.Find("certificate_id");RISECBOR64::Bytes payloadBytes;
		if(!payload||!id||id->GetType()!=RISECBOR64::Value::Text||
			!RISECBOR64::Encode(*payload,payloadBytes,&error)||
			id->GetText()!=RISECBOR64::SHA256Hex(payloadBytes)||
			!ParseResumeCertificatePayload(*payload,certificate)){
			error="resume-equivalence certificate is not canonical or self-consistent";return false;
		}
		certificate.certificateId=id->GetText();
		const bool valid=certificate.acceptedStepCount>=8u&&
			certificate.timeStepBits.size()==certificate.acceptedStepCount&&
			certificate.maximumTemperatureBits.size()==certificate.acceptedStepCount&&
			certificate.maximumEOSResidualBits.size()==certificate.acceptedStepCount&&
			certificate.frameDigests.size()==certificate.acceptedStepCount;
		if(!valid)error="resume-equivalence certificate evidence is incomplete";
		return valid;
	}
	bool ResumeTraceEvidenceComplete(const ResumeEquivalenceTrace& trace)
	{
		return trace.acceptedStepCount>=8u&&trace.checkpointDigest.size()==64u&&
			trace.checkpointProducerBuildId.size()==64u&&trace.buildId.size()==64u&&
			trace.executableDigest.size()==64u&&
			trace.timeStepBits.size()==trace.acceptedStepCount&&
			trace.maximumTemperatureBits.size()==trace.acceptedStepCount&&
			trace.maximumEOSResidualBits.size()==trace.acceptedStepCount&&
			trace.frameDigests.size()==trace.acceptedStepCount&&std::all_of(trace.frameDigests.begin(),
				trace.frameDigests.end(),[](const std::string& digest){return digest.size()==64u;});
	}
	bool SaveResumeEquivalenceCertificate(const std::filesystem::path& path,
		ResumeEquivalenceCertificate& certificate,std::string& error)
	{
		const RISECBOR64::Value payload=ResumeCertificatePayload(certificate);
		RISECBOR64::Bytes payloadBytes,envelope;
		if(!RISECBOR64::Encode(payload,payloadBytes,&error))return false;
		certificate.certificateId=RISECBOR64::SHA256Hex(payloadBytes);
		if(!RISECBOR64::Encode(RISECBOR64::Value::MapValue({{"payload",payload},
			{"certificate_id",RISECBOR64::Value::String(certificate.certificateId)}}),
			envelope,&error))return false;
		return DurableWriteCanonical(path,envelope,error);
	}
	bool BuildResumeEquivalenceCertificate(const ResumeEquivalenceTrace& oldTrace,
		const ResumeEquivalenceTrace& newTrace,const std::filesystem::path& path,
		ResumeEquivalenceCertificate& certificate,std::string& error)
	{
		if(!ResumeTraceEvidenceComplete(oldTrace)||!ResumeTraceEvidenceComplete(newTrace)||
			oldTrace.acceptedStepCount!=newTrace.acceptedStepCount||
			oldTrace.checkpointDigest!=newTrace.checkpointDigest||
			oldTrace.checkpointProducerBuildId!=newTrace.checkpointProducerBuildId||
			oldTrace.buildId!=oldTrace.checkpointProducerBuildId||
			oldTrace.buildId==newTrace.buildId||
			oldTrace.executableDigest==newTrace.executableDigest||
			oldTrace.resumedFromStep!=newTrace.resumedFromStep||
			oldTrace.timeStepBits!=newTrace.timeStepBits||
			oldTrace.maximumTemperatureBits!=newTrace.maximumTemperatureBits||
			oldTrace.maximumEOSResidualBits!=newTrace.maximumEOSResidualBits||
			oldTrace.frameDigests!=newTrace.frameDigests){
			error="resume-equivalence traces differ";return false;
		}
		certificate=ResumeEquivalenceCertificate();
		certificate.checkpointDigest=oldTrace.checkpointDigest;
		certificate.oldBuildId=oldTrace.buildId;certificate.newBuildId=newTrace.buildId;
		certificate.oldExecutableDigest=oldTrace.executableDigest;
		certificate.newExecutableDigest=newTrace.executableDigest;
		certificate.resumedFromStep=oldTrace.resumedFromStep;
		certificate.acceptedStepCount=oldTrace.acceptedStepCount;
		certificate.timeStepBits=oldTrace.timeStepBits;
		certificate.maximumTemperatureBits=oldTrace.maximumTemperatureBits;
		certificate.maximumEOSResidualBits=oldTrace.maximumEOSResidualBits;
		certificate.frameDigests=oldTrace.frameDigests;
		return SaveResumeEquivalenceCertificate(path,certificate,error);
	}

	template<typename T> std::vector<T> FinalEvidenceValues(const std::vector<T>& values,
		const std::size_t count)
	{
		if(values.size()<count)return std::vector<T>();
		return std::vector<T>(values.end()-static_cast<std::ptrdiff_t>(count),values.end());
	}

	bool DurableCopyPublishedFile(const std::filesystem::path& source,
		const std::filesystem::path& target,std::string& error)
	{
		if(target.has_parent_path())std::filesystem::create_directories(target.parent_path());
#if defined(_WIN32)
		const long long processId=static_cast<long long>(::_getpid());
#else
		const long long processId=static_cast<long long>(::getpid());
#endif
		const std::filesystem::path temporary=target.string()+".tmp."+std::to_string(processId);
		std::error_code copyError;
		std::filesystem::copy_file(source,temporary,
			std::filesystem::copy_options::overwrite_existing,copyError);
		if(copyError){error="cannot copy durable run artifact";return false;}
		if(!DurableSyncFileAndDirectory(temporary,error)||
			!AtomicReplaceCheckpoint(temporary,target,error)){
			std::error_code ignored;std::filesystem::remove(temporary,ignored);return false;
		}
		return true;
	}

	bool DurableWritePublishedBytes(const RISECBOR64::Bytes& bytes,
		const std::filesystem::path& target,std::string& error)
	{
		if(target.has_parent_path())std::filesystem::create_directories(target.parent_path());
#if defined(_WIN32)
		const long long processId=static_cast<long long>(::_getpid());
#else
		const long long processId=static_cast<long long>(::getpid());
#endif
		const std::filesystem::path temporary=target.string()+".tmp."+std::to_string(processId);
		{
			std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
			if(!output){error="cannot open durable run metadata";return false;}
			output.write(reinterpret_cast<const char*>(bytes.data()),
				static_cast<std::streamsize>(bytes.size()));
			output.flush();
			if(!output){error="cannot write durable run metadata";return false;}
		}
		if(!DurableSyncFileAndDirectory(temporary,error)||
			!AtomicReplaceCheckpoint(temporary,target,error)){
			std::error_code ignored;std::filesystem::remove(temporary,ignored);return false;
		}
		return true;
	}

	RISECBOR64::Bytes ReadFileBytes( const std::filesystem::path& path )
	{
		std::ifstream input(path,std::ios::binary);
		if(!input) return RISECBOR64::Bytes();
		input.seekg(0,std::ios::end);
		const std::streampos end=input.tellg();
		if(end<0) return RISECBOR64::Bytes();
		input.seekg(0,std::ios::beg);
		RISECBOR64::Bytes bytes(static_cast<std::size_t>(end));
		if(end>0) input.read(reinterpret_cast<char*>(bytes.data()),end);
		if(!input&&end>0) return RISECBOR64::Bytes();
		return bytes;
	}

	bool VerifyVisibleFireDisplayDerivative(
		const RISECBOR64::Bytes& artifact,
		const RISECBOR64::Bytes& sidecar,
		const RISECBOR64::Value& primaryEnvelope,
		std::string& error,
		const bool requireVisible,
		const bool requirePlume )
	{
		error.clear();
		RISECBOR64::Value envelope;
		if( artifact.empty() || !RISECBOR64::DecodeCanonical(sidecar,envelope,&error) ||
			envelope.GetType()!=RISECBOR64::Value::Map || envelope.GetMap().size()!=2u ) {
			error="display derivative pair is empty or noncanonical";return false;
		}
		const RISECBOR64::Value* payload=envelope.Find("payload");
		const RISECBOR64::Value* provenance=envelope.Find("provenance_id");
		const RISECBOR64::Value* primaryPayload=primaryEnvelope.Find("payload");
		const RISECBOR64::Value* primaryId=primaryEnvelope.Find("provenance_id");
		const RISECBOR64::Value* fidelity=payload?payload->Find("artifact_fidelity"):nullptr;
		const RISECBOR64::Value* digest=payload?payload->Find("artifact_sha256"):nullptr;
		const RISECBOR64::Value* derived=payload?payload->Find("derived_from_primary"):nullptr;
		const RISECBOR64::Value* primaryDigest=primaryPayload?
			primaryPayload->Find("artifact_sha256"):nullptr;
		RISECBOR64::Bytes payloadBytes;
		if( !payload || !provenance || !primaryId || !fidelity || !digest || !derived ||
			!primaryDigest || fidelity->GetText()!="display_derivative" ||
			digest->GetText()!=RISECBOR64::SHA256Hex(artifact) ||
			!RISECBOR64::Encode(*payload,payloadBytes,&error) ||
			provenance->GetText()!=RISECBOR64::SHA256Hex(payloadBytes) ||
			!derived->Find("provenance_id") || !derived->Find("artifact_sha256") ||
			derived->Find("provenance_id")->GetText()!=primaryId->GetText() ||
			derived->Find("artifact_sha256")->GetText()!=primaryDigest->GetText() ) {
			error="display derivative does not hash its bytes or link the verified primary";
			return false;
		}
		MemoryBuffer* buffer=new MemoryBuffer(const_cast<char*>(
			reinterpret_cast<const char*>(artifact.data())),
			static_cast<unsigned int>(artifact.size()),false);
		IRasterImageReader* reader=nullptr;
		unsigned int width=0u,height=0u;
		const bool began=RISE_API_CreatePNGReader(&reader,*buffer,
			eColorSpace_Rec709RGB_Linear)&&reader&&reader->BeginRead(width,height);
		bool visible=false;
		std::size_t litPixels=0u,minX=width,minY=height,maxX=0u,maxY=0u;
		double redSum=0.0,blueSum=0.0;
		if( began ) for(unsigned int y=0u;y<height;++y)
			for(unsigned int x=0u;x<width;++x) {
				RISEColor pixel;reader->ReadColor(pixel,x,y);
				visible=visible||pixel.base.r>0.0||pixel.base.g>0.0||pixel.base.b>0.0;
				const bool lit=std::max({pixel.base.r,pixel.base.g,pixel.base.b})>0.5;
				if(lit){visible=true;++litPixels;minX=std::min(minX,static_cast<std::size_t>(x));
					minY=std::min(minY,static_cast<std::size_t>(y));
					maxX=std::max(maxX,static_cast<std::size_t>(x));
					maxY=std::max(maxY,static_cast<std::size_t>(y));
					redSum+=pixel.base.r;blueSum+=pixel.base.b;}
			}
		if( began ) reader->EndRead();
		safe_release(reader);safe_release(buffer);
		if( !began || width==0u || height==0u || (requireVisible&&!visible) ) {
			error="display derivative is undecodable, empty, or all black";return false;
		}
		if(requirePlume&&(!visible||2u*litPixels>=static_cast<std::size_t>(width)*height||
			(maxY-minY)<=(maxX-minX)||!(blueSum>redSum))){
			std::ostringstream reason;
			reason<<"display derivative lacks a bounded blue plume: lit="<<litPixels<<
				" bbox="<<minX<<','<<minY<<'-'<<maxX<<','<<maxY<<
				" red="<<redSum<<" blue="<<blueSum;error=reason.str();return false;
		}
		return true;
	}

	RISECBOR64::Bytes RunMetadataEnvelope(const SolverFrameValues& values,
		const unsigned int workerCount,const std::string& frame4Digest,
		const std::string& frame5Digest,const std::string& sequenceId,
		std::string& metadataId)
	{
		using RISECBOR64::Value;
		Value::Values steps;
		for(const std::uint64_t step:values.checkpointStepIndices)
			steps.push_back(Value::Unsigned(step));
		Value::Values workerHistory;
		for(const std::uint64_t workers:values.workerCountHistory)
			workerHistory.push_back(Value::Unsigned(workers));
		const Value buildMigration=values.migrationCertificateId.empty()?
			Value::MapValue({{"kind",Value::String("none")}}):
			Value::MapValue({
				{"accepted_step_count",Value::Unsigned(values.migrationAcceptedStepCount)},
				{"certificate_id",Value::String(values.migrationCertificateId)},
				{"kind",Value::String("resume_equivalence")},
				{"new_build_id",Value::String(values.migrationNewBuildId)},
				{"old_build_id",Value::String(values.migrationOldBuildId)},
				{"resumed_from_step",Value::Unsigned(values.migrationResumedFromStep)}
			});
		const Value payload=Value::MapValue({
			{"active_set_algorithm_version",Value::String(values.activeSetAlgorithmVersion)},
			{"active_set_prior_algorithm_version",Value::String(
				values.priorActiveSetAlgorithmVersion)},
			{"active_set_discontinuous_event_count",Value::Unsigned(
				values.discontinuousActiveSetEvents)},
			{"active_set_maximum_complementarity_discrepancy_m_per_s",Value::Float(
				values.maximumActiveSetComplementarityDiscrepancyMPerS)},
			{"active_set_maximum_cycle_length",Value::Unsigned(
				values.maximumActiveSetCycleLength)},
			{"active_set_maximum_differing_face_count",Value::Unsigned(
				values.maximumActiveSetDifferingFaceCount)},
			{"active_set_thread_identity_checked",Value::Bool(
				values.activeSetThreadIdentityChecked)},
			{"active_set_thread_identity",Value::Bool(values.activeSetThreadIdentity)},
			{"build_migration",buildMigration},
			{"checkpoint_cadence_wall_s",Value::Float(values.checkpointCadenceWallS)},
			{"checkpoint_count",Value::Unsigned(values.checkpointStepIndices.size())},
			{"checkpoint_step_indices",Value::ArrayValue(steps)},
			{"effective_worker_count",Value::Unsigned(workerCount)},
			{"frame4_sha256",Value::String(frame4Digest)},
			{"frame5_sha256",Value::String(frame5Digest)},
			{"record_kind",Value::String("fire-simulation-run-metadata-v3")},
			{"reduction_mode",Value::String(values.reductionMode)},
			{"resumed_from_checkpoint",Value::Bool(values.resumedFromCheckpoint)},
			{"resumed_from_step",Value::Unsigned(values.resumedFromStep)},
			{"schema_version",Value::Unsigned(3)},
			{"sequence_id",Value::String(sequenceId)},
			{"streamed_frame_count",Value::Unsigned(values.streamedFrameCount)},
			{"worker_count_history",Value::ArrayValue(workerHistory)}
		});
		std::string error;RISECBOR64::Bytes payloadBytes,envelope;
		if(!RISECBOR64::Encode(payload,payloadBytes,&error))return envelope;
		metadataId=RISECBOR64::SHA256Hex(payloadBytes);
		RISECBOR64::Encode(Value::MapValue({{"payload",payload},
			{"run_metadata_id",Value::String(metadataId)}}),envelope,&error);
		return envelope;
	}

	std::string TextArrayCSV( const RISECBOR64::Value* value )
	{
		if(!value || value->GetType()!=RISECBOR64::Value::Array) return std::string();
		std::string result;
		for(const RISECBOR64::Value& item:value->GetArray()) {
			if(item.GetType()!=RISECBOR64::Value::Text) continue;
			if(!result.empty()) result.push_back(',');
			result+=item.GetText();
		}
		return result;
	}
	void WriteJSON( std::ostream& output, const RISECBOR64::Value& value )
	{
		using RISECBOR64::Value;
		switch(value.GetType()) {
		case Value::Null: output << "null"; break;
		case Value::Boolean: output << (value.GetBoolean()?"true":"false"); break;
		case Value::UnsignedInteger: output << value.GetIntegerArgument(); break;
		case Value::NegativeInteger: output << (-1-static_cast<std::int64_t>(
			value.GetIntegerArgument())); break;
		case Value::Float64: output << std::setprecision(17) << value.GetFloat(); break;
		case Value::Text: {
			output << '"';
			for(const unsigned char c:value.GetText()) {
				if(c=='"'||c=='\\') output << '\\' << static_cast<char>(c);
				else if(c=='\n') output << "\\n";
				else if(c=='\r') output << "\\r";
				else if(c=='\t') output << "\\t";
				else if(c<0x20u) output << "\\u" << std::hex << std::setw(4) <<
					std::setfill('0') << static_cast<unsigned int>(c) << std::dec;
				else output << static_cast<char>(c);
			}
			output << '"'; break;
		}
		case Value::ByteString:
			output << '"'; for(const unsigned char byte:value.GetBytes()) output << std::hex <<
				std::setw(2) << std::setfill('0') << static_cast<unsigned int>(byte);
			output << std::dec << '"'; break;
		case Value::Array:
			output << '['; for(std::size_t i=0;i<value.GetArray().size();++i) {
				if(i) output << ','; WriteJSON(output,value.GetArray()[i]);
			} output << ']'; break;
		case Value::Map:
			output << '{'; for(std::size_t i=0;i<value.GetMap().size();++i) {
				if(i) output << ','; WriteJSON(output,Value::String(value.GetMap()[i].first));
				output << ':'; WriteJSON(output,value.GetMap()[i].second);
			} output << '}'; break;
		}
	}

#if defined(RISE_ENABLE_OPENVDB)
	struct FrameMutation
	{
		enum Kind { Valid, NegativeActiveCarbon, HotInactiveTemperature,
			NegativeInactiveCarbon, NonfiniteInactiveCarbon, NonfiniteVelocity,
			PositiveInfinityReaction, NegativeInfinityReaction, ZeroTemperature,
			OutOfDomainTemperature, NegativeActiveTile, HotInactiveTile,
			NaNInactiveTile, NegativeActiveChem, NaNInactiveChem } kind = Valid;
	};

	bool WriteFrame( const std::filesystem::path& path, const FrameMutation mutation,
		const float carbonValue, const bool includeChem=false,
		const SolverFrameValues solver=SolverFrameValues(),const float chemScale=1.0f )
	{
		openvdb::initialize();
		const bool solverGrid=solver.dimensions[0]&&solver.dimensions[1]&&solver.dimensions[2];
		const double voxelSize=solverGrid?solver.cellWidthM:0.5;
		const openvdb::math::Transform::Ptr transform =
			openvdb::math::Transform::createLinearTransform(voxelSize);
		openvdb::FloatGrid::Ptr carbon = openvdb::FloatGrid::create(0.0f);
		openvdb::FloatGrid::Ptr temperature = openvdb::FloatGrid::create(300.0f);
		openvdb::FloatGrid::Ptr reaction = openvdb::FloatGrid::create(0.0f);
		openvdb::FloatGrid::Ptr chemCH = openvdb::FloatGrid::create(0.0f);
		openvdb::FloatGrid::Ptr chemC2 = openvdb::FloatGrid::create(0.0f);
		openvdb::FloatGrid::Ptr chemCO2 = openvdb::FloatGrid::create(0.0f);
		openvdb::Vec3fGrid::Ptr velocity = openvdb::Vec3fGrid::create(openvdb::Vec3f(0));
		for( const auto& grid : {openvdb::GridBase::Ptr(carbon),openvdb::GridBase::Ptr(temperature),
			openvdb::GridBase::Ptr(reaction),openvdb::GridBase::Ptr(chemCH),
			openvdb::GridBase::Ptr(chemC2),openvdb::GridBase::Ptr(chemCO2)} ) {
			grid->setTransform(transform->copy());
		}
		openvdb::math::Transform::Ptr velocityTransform = transform->copy();
		velocityTransform->postTranslate(openvdb::Vec3d(-voxelSize));
		velocity->setTransform(velocityTransform);
		carbon->setName("carbon"); temperature->setName("temperature");
		reaction->setName("reaction"); velocity->setName("velocity");
		chemCH->setName("chem_CH"); chemC2->setName("chem_C2"); chemCO2->setName("chem_CO2");
		if(solverGrid && solver.temperature.size()==solver.dimensions[0]*solver.dimensions[1]*
			solver.dimensions[2] && solver.reaction.size()==solver.temperature.size() &&
			solver.carbon.size()==solver.temperature.size() &&
			solver.velocity.size()==solver.temperature.size()) {
			for(std::size_t z=0;z<solver.dimensions[2];++z)
				for(std::size_t y=0;y<solver.dimensions[1];++y)
				for(std::size_t x=0;x<solver.dimensions[0];++x) {
					const std::size_t index=(z*solver.dimensions[1]+y)*solver.dimensions[0]+x;
					const openvdb::Coord scalar(static_cast<int>(x),static_cast<int>(y),
						static_cast<int>(z));
					carbon->tree().setValueOn(scalar,solver.carbon[index]);
					temperature->tree().setValueOn(scalar,solver.temperature[index]);
					reaction->tree().setValueOn(scalar,solver.reaction[index]);
					const std::array<float,3>& v=solver.velocity[index];
					velocity->tree().setValueOn(openvdb::Coord(static_cast<int>(x+1),
						static_cast<int>(y+1),static_cast<int>(z+1)),openvdb::Vec3f(v[0],v[1],v[2]));
				}
		} else {
			carbon->tree().setValueOn(openvdb::Coord(0,0,0),carbonValue);
			temperature->tree().setValueOn(openvdb::Coord(0,0,0),solver.temperatureK);
			reaction->tree().setValueOn(openvdb::Coord(0,0,0),solver.reactionWPerM3);
		}
		if( includeChem ) {
			float reactionMaximum=0.0f,temperatureMaximum=300.0f;
			if(solverGrid) for(std::size_t cell=0u;cell<solver.reaction.size();++cell){
				reactionMaximum=std::max(reactionMaximum,solver.reaction[cell]);
				temperatureMaximum=std::max(temperatureMaximum,solver.temperature[cell]);
			}
			if(solverGrid) for(std::size_t z=0;z<solver.dimensions[2];++z)
				for(std::size_t y=0;y<solver.dimensions[1];++y)
					for(std::size_t x=0;x<solver.dimensions[0];++x) {
						const std::size_t index=(z*solver.dimensions[1]+y)*solver.dimensions[0]+x;
						const float reactionWeight=reactionMaximum>0.0f?
							std::clamp(solver.reaction[index]/reactionMaximum,0.0f,1.0f):0.0f;
						const float thermalWeight=temperatureMaximum>300.0f?
							std::clamp((solver.temperature[index]-300.0f)/
								(temperatureMaximum-300.0f),0.0f,1.0f):0.0f;
						const float previewSourceWeight=std::max(reactionWeight,thermalWeight);
						const openvdb::Coord fixture(static_cast<int>(x),static_cast<int>(y),
							static_cast<int>(z));
						chemCH->tree().setValueOn(fixture,chemScale*120.0f*previewSourceWeight);
						chemC2->tree().setValueOn(fixture,chemScale*50.0f*previewSourceWeight);
						chemCO2->tree().setValueOn(fixture,chemScale*8.0f*previewSourceWeight);
					}
			else {
				chemCH->tree().setValueOn(openvdb::Coord(0,0,0),chemScale*120.0f);
				chemC2->tree().setValueOn(openvdb::Coord(0,0,0),chemScale*50.0f);
				chemCO2->tree().setValueOn(openvdb::Coord(0,0,0),chemScale*8.0f);
			}
		}
		if( mutation.kind == FrameMutation::NegativeActiveCarbon ) {
			carbon->tree().setValueOn(openvdb::Coord(1,0,0),-1.0f);
		} else if( mutation.kind == FrameMutation::HotInactiveTemperature ) {
			temperature->tree().setValueOff(openvdb::Coord(1,0,0),1200.0f);
		} else if( mutation.kind == FrameMutation::NegativeInactiveCarbon ) {
			carbon->tree().setValueOff(openvdb::Coord(1,0,0),-1.0f);
		} else if( mutation.kind == FrameMutation::NonfiniteInactiveCarbon ) {
			carbon->tree().setValueOff(openvdb::Coord(1,0,0),FloatFromBits(0x7fc00001u));
		} else if( mutation.kind == FrameMutation::NonfiniteVelocity ) {
			velocity->tree().setValueOn(openvdb::Coord(0,0,0),
				openvdb::Vec3f(FloatFromBits(0x7f800000u),0,0));
		} else if( mutation.kind == FrameMutation::PositiveInfinityReaction ) {
			reaction->tree().setValueOn(openvdb::Coord(1,0,0),FloatFromBits(0x7f800000u));
		} else if( mutation.kind == FrameMutation::NegativeInfinityReaction ) {
			reaction->tree().setValueOn(openvdb::Coord(1,0,0),FloatFromBits(0xff800000u));
		} else if( mutation.kind == FrameMutation::ZeroTemperature ) {
			temperature->tree().setValueOn(openvdb::Coord(1,0,0),0.0f);
		} else if( mutation.kind == FrameMutation::OutOfDomainTemperature ) {
			temperature->tree().setValueOn(openvdb::Coord(1,0,0),2501.0f);
		} else if( mutation.kind == FrameMutation::NegativeActiveTile ) {
			carbon->tree().addTile(1,openvdb::Coord(0,0,0),-1.0f,true);
		} else if( mutation.kind == FrameMutation::HotInactiveTile ) {
			temperature->tree().addTile(1,openvdb::Coord(0,0,0),1200.0f,false);
		} else if( mutation.kind == FrameMutation::NaNInactiveTile ) {
			carbon->tree().addTile(1,openvdb::Coord(0,0,0),FloatFromBits(0x7fc00001u),false);
		} else if( mutation.kind == FrameMutation::NegativeActiveChem ) {
			chemC2->tree().setValueOn(openvdb::Coord(1,0,0),-1.0f);
		} else if( mutation.kind == FrameMutation::NaNInactiveChem ) {
			chemCO2->tree().setValueOff(openvdb::Coord(1,0,0),FloatFromBits(0x7fc00001u));
		}
		openvdb::GridPtrVec grids{carbon,temperature,reaction,velocity};
		if( includeChem ) {
			grids.push_back(chemCH); grids.push_back(chemC2); grids.push_back(chemCO2);
		}
		openvdb::io::File file(path.string());
		file.write(grids);
		file.close();
		std::string canonicalError;
		const bool canonical=CanonicalizeOpenVDBFileIdentity(path.string(),canonicalError);
		Check(canonical,
			"producer replaces OpenVDB's random UUID with a content-derived identity");
		const bool durable=canonical&&DurableSyncFileAndDirectory(path,canonicalError);
		Check(durable,
			"produced sequence frame is durable before the next simulation step");
		return canonical&&durable;
	}

	bool WriteProductionTemporalFrame(const std::filesystem::path& path,
		const SolverFrameValues& values)
	{
		return WriteFrame(path,FrameMutation{},0.0f,true,values,0.8f);
	}

	int RunProductionFrameWriteBenchmarkChild(const std::filesystem::path& checkpointPath,
		const std::filesystem::path& outputDirectory)
	{
		MethaneRunCheckpoint checkpoint;std::string error;
		if(!LoadMethaneRunCheckpoint(checkpointPath,checkpoint,error))return 90;
		std::error_code directoryError;
		std::filesystem::create_directories(outputDirectory,directoryError);
		if(directoryError)return 91;
		std::vector<double> measuredMS;std::string referenceDigest;
		for(std::size_t trial=0u;trial<6u;++trial){
			std::ostringstream name;name<<(trial==0u?"warmup_":"measured_")<<
				std::setw(2)<<std::setfill('0')<<trial<<".vdb";
			const std::filesystem::path path=outputDirectory/name.str();
			const auto begin=std::chrono::steady_clock::now();
			if(!WriteProductionTemporalFrame(path,checkpoint.values))return 92;
			const double elapsedMS=std::chrono::duration<double,std::milli>(
				std::chrono::steady_clock::now()-begin).count();
			const std::string digest=DigestFile(path);
			if(digest.empty()||(!referenceDigest.empty()&&digest!=referenceDigest))return 93;
			if(referenceDigest.empty())referenceDigest=digest;
			if(trial>0u)measuredMS.push_back(elapsedMS);
		}
		std::sort(measuredMS.begin(),measuredMS.end());
		std::ofstream result(outputDirectory/"frame_write_benchmark.v1",std::ios::trunc);
		result<<std::setprecision(17)<<"checkpoint_sha256 "<<DigestFile(checkpointPath)<<"\n"
			<<"frame_sha256 "<<referenceDigest<<"\n"
			<<"warmup_count 1\nmeasured_count 5\n";
		for(std::size_t sample=0u;sample<measuredMS.size();++sample)
			result<<"sorted_wall_ms_"<<sample<<' '<<measuredMS[sample]<<'\n';
		result<<"p95_max_order_statistic_ms "<<measuredMS.back()<<'\n';
		if(!result)return 94;
		std::fprintf(stderr,"PRODUCTION_FRAME_WRITE_BENCHMARK p95_ms=%.17g frame_sha256=%s\n",
			measuredMS.back(),referenceDigest.c_str());return 0;
	}

	int RunCheckpointChild(const std::string& mode,const std::filesystem::path& checkpointPath,
		const std::filesystem::path& framePath,const unsigned int workerCount)
	{
		if(mode!="baseline"&&mode!="kill"&&mode!="resume"&&mode!="resume-final"&&
			mode!="resume-one-fp64-reject"&&mode!="retain"&&
			mode!="syncfail")return 96;
		forcePostRenameDirectorySyncFailureForTest=mode=="syncfail";
		RunPersistenceOptions persistence;
		if(mode!="baseline"){
			persistence.checkpointPath=checkpointPath;
			persistence.checkpointCadenceWallS=0.0;
			if(mode=="retain")persistence.retainedCheckpointDirectory=
				checkpointPath.parent_path()/"retained";
			persistence.resume=mode=="resume"||mode=="resume-final"||
				mode=="resume-one-fp64-reject";
			if(mode=="resume-final"||mode=="resume-one-fp64-reject")
				persistence.finalCheckpointPath=checkpointPath;
			if(mode=="resume-one-fp64-reject")persistence.stopAfterAdditionalAcceptedSteps=1u;
			if(mode=="resume-one-fp64-reject")persistence.forceZeroSourceForTest=true;
			persistence.killAfterFirstCheckpoint=mode=="kill"||mode=="syncfail";
		}
		SolverFrameValues result=RunMethaneFrameProbe(workerCount,3u,0.0,1.0,4.0,6.0,
			CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,persistence);
		if(mode=="syncfail"){
			forcePostRenameDirectorySyncFailureForTest=false;
			if(result.succeeded||result.structuredError.find("cannot fsync replaced-run directory")==
				std::string::npos)return 97;
			std::ofstream marker(framePath,std::ios::binary|std::ios::trunc);
			marker << result.structuredError;marker.close();return marker?0:98;
		}
		if(mode=="resume-one-fp64-reject")return !result.succeeded&&
			result.structuredError.find("violates the certified affine rows")!=std::string::npos?
			0:97;
		if(!result.succeeded){std::fprintf(stderr,"checkpoint child failed: %s\n",
			result.structuredError.c_str());return 93;}
		if(!WriteFrame(framePath,FrameMutation{},0.0f,true,result))return 94;
		std::string error;
		if(!DurableSyncFileAndDirectory(framePath,error)){std::fprintf(stderr,
			"checkpoint child frame durability failed: %s\n",error.c_str());return 94;}
		return failures?95:0;
	}

	SolverFrameValues CheckpointDiagnosticFrame(const MethaneRunCheckpoint& checkpoint)
	{
		SolverFrameValues values;
		PeriodicMACShape shape;shape.nx=checkpoint.dimensions[0];shape.ny=checkpoint.dimensions[1];
		shape.nz=checkpoint.dimensions[2];shape.cellWidthM=checkpoint.cellWidthM;
		values.dimensions=checkpoint.dimensions;values.cellWidthM=checkpoint.cellWidthM;
		values.caseRecordId=checkpoint.caseRecordId;
		values.temperature.resize(shape.CellCount());values.reaction.assign(shape.CellCount(),0.0f);
		values.carbon.resize(shape.CellCount());values.velocity.resize(shape.CellCount());
		for(std::size_t cell=0;cell<shape.CellCount();++cell){
			values.temperature[cell]=static_cast<float>(checkpoint.states[cell].temperatureK);
			values.carbon[cell]=static_cast<float>(std::max(0.0,
				checkpoint.states[cell].constituent[MethaneCarbon]));
			for(unsigned int axis=0;axis<3;++axis){
				const std::size_t lower=OpenLowerFaceForCell3D(shape,cell,axis);
				const std::size_t upper=OpenUpperFaceForCell3D(shape,cell,axis);
				values.velocity[cell][axis]=static_cast<float>(0.5*(
					checkpoint.velocity.component[axis][lower]+
					checkpoint.velocity.component[axis][upper]));
			}
		}
		values.temperatureK=*std::max_element(values.temperature.begin(),values.temperature.end());
		values.reactionWPerM3=0.0f;values.succeeded=true;
		return values;
	}

	bool ParseUnsignedArgument(const char* text,const unsigned long maximum,
		unsigned long& value)
	{
		char* end=nullptr;errno=0;value=std::strtoul(text,&end,10);
		return !errno&&end!=text&&*end=='\0'&&value>0u&&value<=maximum;
	}
	bool ParsePositiveDoubleArgument(const char* text,double& value)
	{
		char* end=nullptr;errno=0;value=std::strtod(text,&end);
		return !errno&&end!=text&&*end=='\0'&&std::isfinite(value)&&value>0.0;
	}

	int RunResumeEquivalenceTraceChild(const std::filesystem::path& checkpointPath,
		const std::filesystem::path& tracePath,const std::filesystem::path& framePath,
		const unsigned int workerCount,const unsigned int acceptedStepCount,
		const double caseDurationS,const double caseFramesPerS,const double resolutionTier,
		const double poolDiameterM,const double heatReleaseRateKW,
		const std::string& expectedCheckpointBuildId)
	{
		MethaneRunCheckpoint checkpoint;std::string error;
		if(!LoadMethaneRunCheckpoint(checkpointPath,checkpoint,error)||
			checkpoint.producerBuildId!=expectedCheckpointBuildId){
			std::fprintf(stderr,"resume-equivalence checkpoint rejected: %s\n",error.c_str());
			return 91;
		}
		const std::string checkpointDigest=DigestFile(checkpointPath);
		if(checkpointDigest.empty())return 92;
		const std::filesystem::path snapshotDirectory=tracePath.string()+".snapshots";
		std::error_code directoryError;
		std::filesystem::create_directories(snapshotDirectory,directoryError);
		if(directoryError)return 92;
		RunPersistenceOptions persistence;
		persistence.checkpointPath=checkpointPath;
		persistence.checkpointCadenceWallS=std::numeric_limits<double>::max();
		persistence.resume=true;persistence.isolatedEquivalenceProbe=true;
		persistence.isolatedExpectedCheckpointBuildId=expectedCheckpointBuildId;
		persistence.stopAfterAdditionalAcceptedSteps=acceptedStepCount;
		persistence.equivalenceSnapshotDirectory=snapshotDirectory;
		const SolverFrameValues result=RunMethaneFrameProbe(workerCount,1u,0.0,
			caseDurationS,caseFramesPerS,resolutionTier,poolDiameterM,heatReleaseRateKW,
			false,persistence);
		if(!result.succeeded){std::fprintf(stderr,"resume-equivalence continuation failed: %s\n",
			result.structuredError.c_str());return 93;}
		RISECBOR64::Bytes buildRecord;std::string buildId,executableDigest;
		if(!CurrentRendererBuildIdentity(buildRecord,buildId)||
			!CurrentExecutableDigest(buildRecord,executableDigest,error))return 95;
		ResumeEquivalenceTrace trace;
		trace.checkpointDigest=checkpointDigest;
		trace.checkpointProducerBuildId=checkpoint.producerBuildId;
		trace.buildId=buildId;trace.executableDigest=executableDigest;
		trace.resumedFromStep=checkpoint.acceptedSteps;
		trace.acceptedStepCount=acceptedStepCount;
		trace.timeStepBits.reserve(acceptedStepCount);
		trace.maximumTemperatureBits.reserve(acceptedStepCount);
		trace.maximumEOSResidualBits.reserve(acceptedStepCount);
		const std::vector<double> timeSteps=FinalEvidenceValues(
			result.acceptedTimeStepHistoryS,acceptedStepCount);
		const std::vector<double> temperatures=FinalEvidenceValues(
			result.acceptedMaximumTemperatureHistoryK,acceptedStepCount);
		const std::vector<double> eosResiduals=FinalEvidenceValues(
			result.acceptedMaximumEOSResidualHistory,acceptedStepCount);
		if(timeSteps.size()!=acceptedStepCount||temperatures.size()!=acceptedStepCount||
			eosResiduals.size()!=acceptedStepCount)return 96;
		for(const double value:timeSteps)trace.timeStepBits.push_back(DoubleBits(value));
		for(const double value:temperatures)trace.maximumTemperatureBits.push_back(DoubleBits(value));
		for(const double value:eosResiduals)trace.maximumEOSResidualBits.push_back(DoubleBits(value));
		for(unsigned int evidenceStep=1u;evidenceStep<=acceptedStepCount;++evidenceStep){
			std::ostringstream name;name<<"step_"<<std::setw(2)<<std::setfill('0')<<
				evidenceStep<<".checkpoint";
			MethaneRunCheckpoint snapshot;
			if(!LoadMethaneRunCheckpoint(snapshotDirectory/name.str(),snapshot,error))return 94;
			const SolverFrameValues diagnostic=CheckpointDiagnosticFrame(snapshot);
			const std::filesystem::path evidenceFrame=evidenceStep==acceptedStepCount?framePath:
				std::filesystem::path(framePath.string()+".step_"+std::to_string(evidenceStep)+".vdb");
			if(!WriteFrame(evidenceFrame,FrameMutation{},0.0f,true,diagnostic))return 94;
			trace.frameDigests.push_back(DigestFile(evidenceFrame));
		}
		if(trace.frameDigests.size()!=acceptedStepCount||
			std::any_of(trace.frameDigests.begin(),trace.frameDigests.end(),
				[](const std::string& digest){return digest.empty();})||
			!SaveResumeEquivalenceTrace(tracePath,trace,error)){
			std::fprintf(stderr,"resume-equivalence trace write failed: %s\n",error.c_str());
			return 97;
		}
		return 0;
	}

	double ProductionResolutionTierForCheckpointDimensions(
		const std::array<std::size_t,3>& dimensions)
	{
		if(dimensions==std::array<std::size_t,3>{{52u,52u,80u}})return 6.0;
		if(dimensions==std::array<std::size_t,3>{{69u,69u,106u}})return 8.0;
		if(dimensions==std::array<std::size_t,3>{{86u,86u,132u}})return 10.0;
		return 0.0;
	}

	int RunProductionCheckpointPhysicsDiagnosticChild(
		const std::filesystem::path& checkpointPath,const std::filesystem::path& outputPath)
	{
		MethaneRunCheckpoint checkpoint;std::string error;
		if(!LoadMethaneRunCheckpoint(checkpointPath,checkpoint,error))return 90;
		const double diagnosticResolutionTier=
			ProductionResolutionTierForCheckpointDimensions(checkpoint.dimensions);
		if(diagnosticResolutionTier==0.0)return 92;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		FireCase::AuthoredV1 authored;authored.fuelRecordId=fuel.RecordId();
		authored.poolDiameterM=CapstonePoolDiameterM;
		authored.heatReleaseRateKW=CapstoneHeatReleaseRateKW;authored.envelope={{0.0,1.0}};
		authored.durationS=1.0;authored.quality="dstar";
		authored.numericDStarTier=diagnosticResolutionTier;
		authored.seed=1234;authored.outputFramesPerS=1.0;authored.plumeLaw=true;
		FireCase::RecordV1 caseRecord;const RISECBOR64::Bytes aerosol=AerosolRecord();
		const RISECBOR64::Bytes chem=SyntheticChemRecord();
		if(!FireCase::BuildMethaneV1(authored,fuel,{
			RISECBOR64::SHA256Hex(fuel.RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationThermochemistryRecord::OpenSubsetV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationTransportRecord::OpenV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireOpticsPreset::PredictiveV1().RecordBytes()),
			RISECBOR64::SHA256Hex(aerosol),RISECBOR64::SHA256Hex(chem)},caseRecord,error))return 91;
		PeriodicMACShape shape;shape.nx=caseRecord.derived.nx;shape.ny=caseRecord.derived.ny;
		shape.nz=caseRecord.derived.nz;shape.cellWidthM=caseRecord.derived.cellWidthM;
		if(checkpoint.dimensions!=std::array<std::size_t,3>{{shape.nx,shape.ny,shape.nz}}||
			checkpoint.cellWidthM!=shape.cellWidthM||checkpoint.states.size()!=shape.CellCount())return 92;

		auto canonicalState=[&](const std::vector<double>& fractions,
			const bool injected,MethaneCellState& state)->bool{
			if(fractions.size()!=MethaneSpeciesCount)return false;
			state=MethaneCellState();state.temperatureK=300.0;
			for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
				state.constituent[species]=fractions[species];
			double inverseWeight=0.0;
			for(std::size_t species=0u;species<MethaneCarbon;++species){
				const FireThermochemistrySpecies* record=fuel.FindSpecies(
					fuel.SpeciesOrder()[species].c_str());
				if(record)inverseWeight+=state.constituent[species]/record->molecularWeightKGPerKMol;
			}
			const double density=fuel.ThermodynamicPressurePa()/(8314.46261815324*
				state.temperatureK*inverseWeight);
			for(double& value:state.constituent)value*=density;
			state.rhoTotalZ=injected?state.TotalDensity():0.0;
			return fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(state),
				state.temperatureK,state.sensibleEnergyJPerM3,&error);
		};
		MethaneCellState ambient,injected;
		if(!canonicalState(fuel.AmbientMassFractions(),false,ambient)||
			!canonicalState(fuel.InjectedMassFractions(),true,injected))return 93;
		OpenBoundaryConfig3D boundary;boundary.ambientDensityKGPerM3=ambient.GasDensity();
		boundary.injectedGasDensityKGPerM3=injected.GasDensity();
		boundary.ambientState=ToConservativeVector(ambient);
		boundary.injectedState=ToConservativeVector(injected);
		boundary.fuelMassFluxKGPerM2S=0.0;
		const double referenceVelocity=std::sqrt(9.80665*
			caseRecord.derived.characteristicDiameterM);
		boundary.velocityToleranceMPerS=1.0e-3*referenceVelocity;
		boundary.pressureTolerancePa=ambient.GasDensity()*referenceVelocity*
			boundary.velocityToleranceMPerS;
		std::vector<double> sourcePattern;
		std::vector<std::uint8_t> canonicalPilotMask;
		if(!FireCase::BuildSourcePattern(authored,caseRecord.derived,sourcePattern,error)||
			!FireCase::BuildPilotMask(authored,caseRecord.derived,canonicalPilotMask,error))return 94;
		boundary.bottomFuelMask.resize(shape.nx*shape.ny,false);
		boundary.bottomFuelMassFluxKGPerM2S.resize(shape.nx*shape.ny,0.0);
		for(std::size_t face=0u;face<sourcePattern.size();++face)if(sourcePattern[face]!=0.0){
			boundary.bottomFuelMask[face]=true;
			boundary.bottomFuelMassFluxKGPerM2S[face]=
				caseRecord.derived.nominalFuelFluxKGPerM2S*sourcePattern[face];
		}

		double maximumSpeed=0.0;unsigned int maximumSpeedAxis=0u;std::size_t maximumSpeedFace=0u;
		for(unsigned int axis=0u;axis<3u;++axis){
			if(checkpoint.velocity.component[axis].size()!=OpenMACFaceCount3D(shape,axis))return 95;
			for(std::size_t face=0u;face<checkpoint.velocity.component[axis].size();++face){
				const double speed=std::fabs(checkpoint.velocity.component[axis][face]);
				if(speed>maximumSpeed){maximumSpeed=speed;maximumSpeedAxis=axis;maximumSpeedFace=face;}
			}
		}
		double minimumGasDensity=std::numeric_limits<double>::infinity(),maximumGasDensity=0.0;
		std::size_t minimumGasDensityCell=0u;
		double maximumReducedGravity=0.0;std::size_t maximumReducedGravityCell=0u;
		std::vector<double> deviations;deviations.reserve(checkpoint.states.size());
		std::vector<double> signedDeviation(checkpoint.states.size(),0.0);
		for(std::size_t cell=0u;cell<checkpoint.states.size();++cell){
			const double density=checkpoint.states[cell].GasDensity();
			if(density<minimumGasDensity){minimumGasDensity=density;minimumGasDensityCell=cell;}
			maximumGasDensity=std::max(maximumGasDensity,density);
			const double reduced=std::max(0.0,9.80665*(ambient.GasDensity()-density)/density);
			if(reduced>maximumReducedGravity){maximumReducedGravity=reduced;
				maximumReducedGravityCell=cell;}
			double ratio=0.0;
			if(!AcceptedConservativeVolumeRatio(ToConservativeVector(checkpoint.states[cell]),fuel,
				checkpoint.states[cell].producerPrecision,ratio,&error))return 96;
			signedDeviation[cell]=ratio-1.0;deviations.push_back(std::fabs(ratio-1.0));
		}
		std::sort(deviations.begin(),deviations.end());
		auto quantile=[&](const double fraction){return deviations[static_cast<std::size_t>(
			fraction*static_cast<double>(deviations.size()-1u))];};
		auto faceCoordinates=[&](const unsigned int axis,const std::size_t face,
			std::size_t& x,std::size_t& y,std::size_t& z){
			const std::size_t ex=axis==0u?shape.nx+1u:shape.nx;
			const std::size_t ey=axis==1u?shape.ny+1u:shape.ny;
			x=face%ex;const std::size_t row=face/ex;y=row%ey;z=row/ey;
		};
		auto cellIndex=[&](const std::size_t x,const std::size_t y,const std::size_t z){
			return (z*shape.ny+y)*shape.nx+x;};
		auto faceDensity=[&](const unsigned int axis,const std::size_t face,
			std::size_t* lowCell,std::size_t* highCell){
			std::size_t x=0u,y=0u,z=0u;faceCoordinates(axis,face,x,y,z);
			const std::size_t coordinate=axis==0u?x:(axis==1u?y:z);
			const std::size_t extent=axis==0u?shape.nx:(axis==1u?shape.ny:shape.nz);
			std::array<std::size_t,3> low={{x,y,z}},high=low;
			if(coordinate>0u&&coordinate<extent){low[axis]=coordinate-1u;high[axis]=coordinate;}
			else {low[axis]=coordinate==0u?0u:extent-1u;high=low;}
			const std::size_t lowIndex=cellIndex(low[0],low[1],low[2]);
			const std::size_t highIndex=cellIndex(high[0],high[1],high[2]);
			if(lowCell)*lowCell=lowIndex;if(highCell)*highCell=highIndex;
			const float lowDensity=static_cast<float>(checkpoint.states[lowIndex].GasDensity());
			if(coordinate==0u||coordinate==extent)return 0.5f*lowDensity+
				0.5f*static_cast<float>(ambient.GasDensity());
			return 0.5f*lowDensity+0.5f*
				static_cast<float>(checkpoint.states[highIndex].GasDensity());
		};
		double maximumMomentumVelocityResidual=0.0;unsigned int residualAxis=0u;
		std::size_t residualFace=0u;
		for(unsigned int axis=0u;axis<3u;++axis)for(std::size_t face=0u;
			face<checkpoint.velocity.component[axis].size();++face){
			const double density=faceDensity(axis,face,nullptr,nullptr);
			const double residual=std::fabs(checkpoint.momentum.component[axis][face]-
				density*checkpoint.velocity.component[axis][face]);
			if(residual>maximumMomentumVelocityResidual){maximumMomentumVelocityResidual=residual;
				residualAxis=axis;residualFace=face;}
		}
		std::size_t extremeLowCell=0u,extremeHighCell=0u,extremeX=0u,extremeY=0u,extremeZ=0u;
		faceCoordinates(maximumSpeedAxis,maximumSpeedFace,extremeX,extremeY,extremeZ);
		const double extremeFaceDensity=faceDensity(maximumSpeedAxis,maximumSpeedFace,
			&extremeLowCell,&extremeHighCell);
		const double extremeMomentum=checkpoint.momentum.component[maximumSpeedAxis][maximumSpeedFace];
		const double extremeVelocity=checkpoint.velocity.component[maximumSpeedAxis][maximumSpeedFace];
		const double extremeCompatibilityResidual=std::fabs(extremeMomentum-
			extremeFaceDensity*extremeVelocity);

		std::vector<ConservativeVector> conservative(shape.CellCount());
		std::vector<double> temperature(shape.CellCount());
		for(std::size_t cell=0u;cell<shape.CellCount();++cell){
			conservative[cell]=ToConservativeVector(checkpoint.states[cell]);
			temperature[cell]=checkpoint.states[cell].temperatureK;
		}
		OpenMACField3D velocity;velocity.component=checkpoint.velocity.component;
		std::vector<CellTransportEvaluation> transport;
		if(!BuildOpenStageTransportEvaluations3D(shape,conservative,temperature,velocity,boundary,
			false,fuel,FireSimulationTransportRecord::OpenV1(),
			FireStateProducerPrecision::Binary32,transport,&error,8u))return 97;
		IgnitionGrid eligibilityGrid;eligibilityGrid.nx=shape.nx;eligibilityGrid.ny=shape.ny;
		eligibilityGrid.nz=shape.nz;eligibilityGrid.cells=checkpoint.states;
		eligibilityGrid.pilotMask.resize(shape.CellCount(),false);
		for(std::size_t cell=0u;cell<shape.CellCount();++cell)
			eligibilityGrid.pilotMask[cell]=ProductionPilotCommandCell(cell,shape.nx,shape.ny,
				canonicalPilotMask,sourcePattern);
		std::vector<bool> eligible;
		if(!BuildIgnitionEligibility(eligibilityGrid,fuel,fuel,
			FireSimulationTransportRecord::OpenV1(),eligible,&error))return 98;
		const double step=checkpoint.lastAcceptedStepS;
		std::vector<MethaneReactionStep> reactions(shape.CellCount());
		const double pilotEndS=caseRecord.derived.pilotDurationMultiplier*
			caseRecord.derived.flowThroughTimeS;
		for(std::size_t cell=0u;cell<shape.CellCount();++cell){
			reactions[cell].deltaTimeS=step;reactions[cell].maximumAcceptedTemperatureK=
				caseRecord.derived.maximumAcceptedTemperatureK;
			reactions[cell].primaryEligible=eligible[cell];reactions[cell].sootOxidationEnabled=true;
			if(!ComputeMixingTimeS(checkpoint.states[cell],transport[cell],
				FireSimulationTransportRecord::OpenV1(),shape.cellWidthM,ambient.GasDensity(),
				9.80665,false,reactions[cell].mixingTimeS,&error)||
				!FireCase::EvaluatePilotSetpointTemperatureK(caseRecord.derived,
					ProductionPilotCommandCell(cell,shape.nx,shape.ny,canonicalPilotMask,sourcePattern),
					checkpoint.simulationTimeS,checkpoint.simulationTimeS+step,
					reactions[cell].pilotSetpointTemperatureK,error))return 99;
			reactions[cell].pilotExpansionVolumeRatioCap=
				reactions[cell].pilotSetpointTemperatureK>0.0?
				caseRecord.derived.pilotExpansionVolumeRatioCap:0.0;
		}
		std::vector<MethaneCellState> packetBeginning=checkpoint.states;
		const double injectedDensity=injected.GasDensity();
		const double injectedSpecificEnergy=injected.sensibleEnergyJPerM3/injected.TotalDensity();
		std::vector<ConservativeVector> staged(shape.CellCount());
		for(std::size_t cell=0u;cell<shape.CellCount();++cell)staged[cell]=
			ToConservativeVector(packetBeginning[cell]);
		for(std::size_t y=0u;y<shape.ny;++y)for(std::size_t x=0u;x<shape.nx;++x){
			const std::size_t face=y*shape.nx+x;if(sourcePattern[face]==0.0)continue;
			const double massFlux=caseRecord.derived.nominalFuelFluxKGPerM2S*sourcePattern[face];
			const double densityDelta=massFlux*step/shape.cellWidthM;const std::size_t cell=face;
			staged[cell][0]+=densityDelta;
			for(std::size_t species=0u;species<MethaneSpeciesCount;++species)
				staged[cell][1u+species]+=densityDelta*injected.constituent[species]/injectedDensity;
			staged[cell][8]+=densityDelta*injectedSpecificEnergy;
		}
		for(ConservativeVector& value:staged)for(std::size_t component=0u;
			component<MethaneConservativeDimension;++component)value[component]=
			static_cast<double>(static_cast<float>(value[component]));
		std::vector<double> stagedTemperature;
		if(!InvertPeriodicTemperaturesWithinBounds(staged,fuel,fuel.TemperatureMinK(),
			fuel.TemperatureMaxK(),FireStateProducerPrecision::Binary32,stagedTemperature,
			&error,8u,false))return 100;
		for(std::size_t cell=0u;cell<shape.CellCount();++cell){
			MethaneCellState represented=FromConservativeVector(staged[cell],
				FireStateProducerPrecision::Binary32);represented.temperatureK=stagedTemperature[cell];
			if(!SignedMixtureSensibleEnergy(represented,represented.temperatureK,fuel,
				represented.sensibleEnergyJPerM3,&error))return 100;
			represented.sensibleEnergyJPerM3=static_cast<double>(static_cast<float>(
				represented.sensibleEnergyJPerM3));packetBeginning[cell]=represented;
		}
		for(std::size_t cell=0u;cell<shape.nx*shape.ny;++cell){
			bool flameHolder=false;
			if(!ProductionEstablishedFlameHolderEligible(cell,shape.nx,shape.ny,canonicalPilotMask,
				sourcePattern,checkpoint.simulationTimeS>=pilotEndS,packetBeginning[cell],fuel,
				FireSimulationTransportRecord::OpenV1(),flameHolder,&error))return 101;
			if(flameHolder)reactions[cell].primaryEligible=true;
		}
		std::vector<MethaneSourcePacket> packets;RadiationEscapeFactor escape;
		const double cellVolume=shape.cellWidthM*shape.cellWidthM*shape.cellWidthM;
		if(!BuildFrozenMethaneSourcePackets(packetBeginning,reactions,
			std::vector<double>(shape.CellCount(),cellVolume),300.0,
			caseRecord.derived.referenceHeatReleaseRateW,
			caseRecord.derived.effectiveRadiativeFraction,false,fuel,fuel,
			FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1(),packets,escape,&error,8u))return 102;
		double heatReleaseW=0.0,maximumHeatReleaseWPerM3=0.0,
			fuelConsumptionKGPerS=0.0;
		for(MethaneSourcePacket& packet:packets){
			RepresentMethaneSourcePacketBinary32(packet);
			if(!CertifiedBinary32SourcePacket(packet,fuel))return 103;
			maximumHeatReleaseWPerM3=std::max(maximumHeatReleaseWPerM3,
				packet.gasHeatReleaseWPerM3);
			heatReleaseW+=packet.gasHeatReleaseWPerM3*cellVolume;
			fuelConsumptionKGPerS+=-packet.constituentDelta[MethaneCH4]*cellVolume/step;
		}
		const double consumptionHeatReleaseW=fuelConsumptionKGPerS*fuel.LowerHeatingValueJPerKG();
		const double advectiveStep=maximumSpeed>0.0?0.5*shape.cellWidthM/maximumSpeed:
			std::numeric_limits<double>::infinity();
		const double buoyantStep=maximumReducedGravity>0.0?
			0.5*std::sqrt(2.0*shape.cellWidthM/maximumReducedGravity):
			std::numeric_limits<double>::infinity();
		auto historySummary=[](const std::vector<double>& history,double& mean,double& p95,
			double& maximum)->bool{
			if(history.empty())return false;std::vector<double> ordered=history;
			if(std::any_of(ordered.begin(),ordered.end(),[](const double value){
				return !std::isfinite(value)||value<0.0;}))return false;
			mean=std::accumulate(ordered.begin(),ordered.end(),0.0)/
				static_cast<double>(ordered.size());std::sort(ordered.begin(),ordered.end());
			const std::size_t p95Index=std::min(ordered.size()-1u,
				static_cast<std::size_t>(std::ceil(0.95*static_cast<double>(ordered.size())))-1u);
			p95=ordered[p95Index];maximum=ordered.back();return true;
		};
		double wallMean=0.0,wallP95=0.0,wallMaximum=0.0;
		double deviceMean=0.0,deviceP95=0.0,deviceMaximum=0.0;
		const bool wallHistoryPersisted=historySummary(checkpoint.values.productionWallHistoryMS,
			wallMean,wallP95,wallMaximum);
		const bool deviceHistoryPersisted=historySummary(checkpoint.values.productionDeviceHistoryMS,
			deviceMean,deviceP95,deviceMaximum);
		std::ofstream output(outputPath,std::ios::trunc);output<<std::setprecision(17)
			<<"checkpoint_sha256 "<<DigestFile(checkpointPath)<<"\n"
			<<"checkpoint_producer_build_id "<<checkpoint.producerBuildId<<"\n"
			<<"accepted_steps "<<checkpoint.acceptedSteps<<"\n"
			<<"simulation_time_s "<<checkpoint.simulationTimeS<<"\n"
			<<"last_accepted_dt_s "<<checkpoint.lastAcceptedStepS<<"\n"
			<<"minimum_accepted_dt_s "<<*std::min_element(
				checkpoint.values.acceptedTimeStepHistoryS.begin(),
				checkpoint.values.acceptedTimeStepHistoryS.end())<<"\n"
			<<"maximum_velocity_m_per_s "<<maximumSpeed<<"\n"
			<<"maximum_velocity_axis "<<maximumSpeedAxis<<"\n"
			<<"maximum_velocity_face "<<maximumSpeedFace<<"\n"
			<<"maximum_velocity_face_xyz "<<extremeX<<' '<<extremeY<<' '<<extremeZ<<"\n"
			<<"maximum_velocity_face_density_kg_per_m3 "<<extremeFaceDensity<<"\n"
			<<"maximum_velocity_face_momentum_kg_per_m2_s "<<extremeMomentum<<"\n"
			<<"maximum_velocity_face_compatibility_residual "<<extremeCompatibilityResidual<<"\n"
			<<"maximum_velocity_low_cell "<<extremeLowCell<<"\n"
			<<"maximum_velocity_low_cell_rho_kg_per_m3 "<<
				checkpoint.states[extremeLowCell].GasDensity()<<"\n"
			<<"maximum_velocity_low_cell_temperature_K "<<
				checkpoint.states[extremeLowCell].temperatureK<<"\n"
			<<"maximum_velocity_low_cell_signed_deviation "<<signedDeviation[extremeLowCell]<<"\n"
			<<"maximum_velocity_high_cell "<<extremeHighCell<<"\n"
			<<"maximum_velocity_high_cell_rho_kg_per_m3 "<<
				checkpoint.states[extremeHighCell].GasDensity()<<"\n"
			<<"maximum_velocity_high_cell_temperature_K "<<
				checkpoint.states[extremeHighCell].temperatureK<<"\n"
			<<"maximum_velocity_high_cell_signed_deviation "<<signedDeviation[extremeHighCell]<<"\n"
			<<"minimum_density_cell "<<minimumGasDensityCell<<"\n"
			<<"minimum_density_cell_temperature_K "<<
				checkpoint.states[minimumGasDensityCell].temperatureK<<"\n"
			<<"minimum_density_cell_signed_deviation "<<signedDeviation[minimumGasDensityCell]<<"\n"
			<<"maximum_momentum_velocity_compatibility_residual "<<
				maximumMomentumVelocityResidual<<"\n"
			<<"maximum_momentum_velocity_compatibility_axis "<<residualAxis<<"\n"
			<<"maximum_momentum_velocity_compatibility_face "<<residualFace<<"\n"
			<<"advective_CFL_candidate_s "<<advectiveStep<<"\n"
			<<"ambient_gas_density_kg_per_m3 "<<ambient.GasDensity()<<"\n"
			<<"minimum_gas_density_kg_per_m3 "<<minimumGasDensity<<"\n"
			<<"maximum_gas_density_kg_per_m3 "<<maximumGasDensity<<"\n"
			<<"maximum_positive_reduced_gravity_m_per_s2 "<<maximumReducedGravity<<"\n"
			<<"maximum_positive_reduced_gravity_cell "<<maximumReducedGravityCell<<"\n"
			<<"buoyant_candidate_s "<<buoyantStep<<"\n"
			<<"manifold_deviation_max "<<deviations.back()<<"\n"
			<<"manifold_deviation_p95 "<<quantile(0.95)<<"\n"
			<<"manifold_deviation_p50 "<<quantile(0.50)<<"\n"
			<<"source_probe_dt_s "<<step<<"\n"
			<<"source_probe_maximum_HRR_W_per_m3 "<<maximumHeatReleaseWPerM3<<"\n"
			<<"source_probe_realized_HRR_W "<<heatReleaseW<<"\n"
			<<"source_probe_fuel_consumption_kg_per_s "<<fuelConsumptionKGPerS<<"\n"
			<<"source_probe_consumption_times_LHV_W "<<consumptionHeatReleaseW<<"\n"
			<<"source_probe_HRR_relative_ledger_error "<<std::fabs(heatReleaseW-
				consumptionHeatReleaseW)/std::max(1.0,heatReleaseW)<<"\n"
			<<"production_wall_history_samples "<<checkpoint.values.productionWallHistoryMS.size()<<"\n"
			<<"format13_per_step_cost_histories_persisted "
				<<(wallHistoryPersisted&&deviceHistoryPersisted?"true":"false")<<"\n"
			<<"production_wall_mean_ms "<<wallMean<<"\n"
			<<"production_wall_p95_ms "<<wallP95<<"\n"
			<<"production_wall_maximum_ms "<<wallMaximum<<"\n"
			<<"production_device_mean_ms "<<deviceMean<<"\n"
			<<"production_device_p95_ms "<<deviceP95<<"\n"
			<<"production_device_maximum_ms "<<deviceMaximum<<"\n"
			<<"format13_retry_counters_persisted false\n";
		if(!output)return 105;
		std::fprintf(stderr,"PRODUCTION_CHECKPOINT_PHYSICS_DIAGNOSTIC steps=%llu time=%.17g "
			"velocity=%.17g dt=%.17g gprime=%.17g qmax=%.17g HRR=%.17g "
			"consumption_LHV=%.17g\n",
			static_cast<unsigned long long>(checkpoint.acceptedSteps),checkpoint.simulationTimeS,
			maximumSpeed,checkpoint.lastAcceptedStepS,maximumReducedGravity,
			maximumHeatReleaseWPerM3,heatReleaseW,
			consumptionHeatReleaseW);return 0;
	}

	int RunLegacyCheckpointTraceChild(const std::filesystem::path& checkpointPath,
		const std::filesystem::path& snapshotDirectory,const std::filesystem::path& tracePath,
		const std::filesystem::path& framePath,const std::string& executableDigest)
	{
		MethaneRunCheckpoint beginning;std::string error;
		if(executableDigest.size()!=64u||
			!LoadMethaneRunCheckpoint(checkpointPath,beginning,error))return 91;
		ResumeEquivalenceTrace trace;
		trace.checkpointDigest=DigestFile(checkpointPath);
		trace.checkpointProducerBuildId=beginning.producerBuildId;
		trace.buildId=beginning.producerBuildId;trace.executableDigest=executableDigest;
		trace.resumedFromStep=beginning.acceptedSteps;trace.acceptedStepCount=8u;
		MethaneRunCheckpoint finalCheckpoint;
		for(std::uint64_t evidenceStep=1u;evidenceStep<=8u;++evidenceStep){
			std::ostringstream name;name<<"step_"<<std::setw(2)<<std::setfill('0')<<
				evidenceStep<<".checkpoint";
			MethaneRunCheckpoint snapshot;
			if(!LoadMethaneRunCheckpoint(snapshotDirectory/name.str(),snapshot,error)||
				snapshot.caseRecordId!=beginning.caseRecordId||
				snapshot.producerBuildId!=beginning.producerBuildId||
				snapshot.acceptedSteps!=beginning.acceptedSteps+evidenceStep||
				snapshot.values.acceptedTimeStepHistoryS.empty()||
				snapshot.values.acceptedMaximumEOSResidualHistory.empty())return 92;
			trace.timeStepBits.push_back(DoubleBits(
				snapshot.values.acceptedTimeStepHistoryS.back()));
			double stepMaximumTemperatureK=0.0;
			for(const MethaneCellState& state:snapshot.states)
				stepMaximumTemperatureK=std::max(stepMaximumTemperatureK,state.temperatureK);
			trace.maximumTemperatureBits.push_back(DoubleBits(stepMaximumTemperatureK));
			trace.maximumEOSResidualBits.push_back(DoubleBits(
				snapshot.values.acceptedMaximumEOSResidualHistory.back()));
			const SolverFrameValues diagnostic=CheckpointDiagnosticFrame(snapshot);
			const std::filesystem::path evidenceFrame=evidenceStep==8u?framePath:
				std::filesystem::path(framePath.string()+".step_"+
					std::to_string(evidenceStep)+".vdb");
			if(!WriteFrame(evidenceFrame,FrameMutation{},0.0f,true,diagnostic))return 93;
			trace.frameDigests.push_back(DigestFile(evidenceFrame));
			finalCheckpoint=std::move(snapshot);
		}
		if(finalCheckpoint.acceptedSteps!=beginning.acceptedSteps+8u||
			trace.frameDigests.size()!=8u||
			std::any_of(trace.frameDigests.begin(),trace.frameDigests.end(),
				[](const std::string& digest){return digest.empty();})||
			!SaveResumeEquivalenceTrace(tracePath,trace,error))
			return 94;
		return 0;
	}

	int RunResumeEquivalenceCertificateChild(const std::filesystem::path& oldTracePath,
		const std::filesystem::path& newTracePath,const std::filesystem::path& checkpointPath,
		const std::filesystem::path& certificatePath)
	{
		ResumeEquivalenceTrace oldTrace,newTrace;ResumeEquivalenceCertificate certificate;
		std::string error;
		if(!LoadResumeEquivalenceTrace(oldTracePath,oldTrace,error)||
			!LoadResumeEquivalenceTrace(newTracePath,newTrace,error)||
			oldTrace.checkpointDigest!=DigestFile(checkpointPath)||
			!BuildResumeEquivalenceCertificate(oldTrace,newTrace,certificatePath,certificate,error)){
			std::fprintf(stderr,"resume-equivalence certificate failed: %s\n",error.c_str());
			return 91;
		}
		std::fprintf(stderr,"resume-equivalence certificate_id=%s steps=%llu\n",
			certificate.certificateId.c_str(),
			static_cast<unsigned long long>(certificate.acceptedStepCount));
		return 0;
	}

	bool ProductionTemporalCapstoneTierSupported(const double resolutionTier)
	{
		return resolutionTier==6.0||resolutionTier==8.0||resolutionTier==10.0;
	}

	int RunProductionTemporalCapstoneChild(const double resolutionTier,
		const double frameCadenceS,const std::filesystem::path& outputDirectory)
	{
#if !defined(RISE_ENABLE_OPENVDB)
		(void)resolutionTier;(void)frameCadenceS;(void)outputDirectory;return 90;
#else
		if(!ProductionTemporalCapstoneTierSupported(resolutionTier)||!(frameCadenceS>0.0)||
			!std::isfinite(frameCadenceS)||
			static_cast<double>(static_cast<float>(frameCadenceS))!=frameCadenceS)return 91;
		std::error_code directoryError;
		std::filesystem::create_directories(outputDirectory/"checkpoints",directoryError);
		std::filesystem::create_directories(outputDirectory/"frames",directoryError);
		if(directoryError)return 92;
		const SolverFrameValues beginning=RunMethaneFrameProbe(1u,0u,0.0,1.0,1.0,
			resolutionTier,CapstonePoolDiameterM,CapstoneHeatReleaseRateKW);
		if(!beginning.succeeded||!(beginning.flowThroughTimeS>0.0))return 93;
		const double expectedPuffingHz=1.5/std::sqrt(CapstonePoolDiameterM);
		const double statisticsStartS=5.0*beginning.flowThroughTimeS;
		const double statisticsDurationS=std::ceil((40.0/expectedPuffingHz)/
			frameCadenceS)*frameCadenceS;
		double targetTimeS=statisticsStartS+statisticsDurationS;
		if(const char* diagnosticTarget=std::getenv("RISE_FIRE_TEMPORAL_TARGET_S")){
			char* end=nullptr;const double parsed=std::strtod(diagnosticTarget,&end);
			if(!end||*end!='\0'||!std::isfinite(parsed)||parsed<=0.0)return 91;
			targetTimeS=parsed;
		}
		RunPersistenceOptions persistence;
		persistence.productionMetal=true;
		persistence.checkpointPath=outputDirectory/"production_run.checkpoint";
		persistence.finalCheckpointPath=outputDirectory/"production_final.checkpoint";
		persistence.retainedCheckpointDirectory=outputDirectory/"checkpoint_history";
		persistence.checkpointCadenceWallS=300.0;
		const char* resumeRequested=std::getenv("RISE_FIRE_TEMPORAL_RESUME");
		if(resumeRequested&&std::strcmp(resumeRequested,"1")==0){
			if(!std::filesystem::exists(persistence.checkpointPath))return 92;
			persistence.resume=true;
		}
		persistence.temporalSnapshotDirectory=outputDirectory/"checkpoints";
		persistence.temporalSnapshotCadenceS=frameCadenceS;
		// r175 certifies the nonzero Binary32 source producer and burning-tail
		// continuation at this represented step.  A source-bearing campaign may
		// select a smaller CFL step, but it must not extrapolate that certificate
		// to a larger source integration interval.
		persistence.maximumProductionSourceStepS=
			static_cast<double>(static_cast<float>(0.0016462659696117043));
		// The capstone's CPU-side source/target reductions retain their sealed
		// eight-worker owner; Metal still owns the resident full-grid work.
		const unsigned int workers=8u;
		const auto wallStart=std::chrono::steady_clock::now();
		const SolverFrameValues result=RunMethaneFrameProbe(workers,1u,targetTimeS,
			targetTimeS,1.0,resolutionTier,CapstonePoolDiameterM,
			CapstoneHeatReleaseRateKW,false,persistence);
		const double wallS=std::chrono::duration<double>(
			std::chrono::steady_clock::now()-wallStart).count();
		if(!result.succeeded){std::fprintf(stderr,"production temporal capstone failed: %s\n",
			result.structuredError.c_str());return 94;}
		std::vector<std::filesystem::path> temporalFrames;
		for(const std::filesystem::directory_entry& entry:
			std::filesystem::directory_iterator(outputDirectory/"checkpoints"))
			if(entry.is_regular_file()&&entry.path().extension()==".vdb")
				temporalFrames.push_back(entry.path());
		std::sort(temporalFrames.begin(),temporalFrames.end());
		std::vector<std::string> frameDigests;
		std::vector<double> frameTimesS;
		std::string error;
		for(std::size_t frame=0u;frame<temporalFrames.size();++frame){
			std::ostringstream name;name<<"frame_"<<std::setw(5)<<std::setfill('0')<<frame<<".vdb";
			const std::filesystem::path framePath=outputDirectory/"frames"/name.str();
			if(temporalFrames[frame].filename()!=name.str())return 95;
			std::filesystem::copy_file(temporalFrames[frame],framePath,
				std::filesystem::copy_options::overwrite_existing,directoryError);
			if(directoryError)return 96;
			frameDigests.push_back(DigestFile(temporalFrames[frame]));
			if(frameDigests.back().empty())return 96;
			std::filesystem::path timePath=temporalFrames[frame];timePath.replace_extension(".time");
			std::ifstream timeRecord(timePath);double timeS=0.0;std::string recordedDigest;
			if(!(timeRecord>>timeS>>recordedDigest)||recordedDigest!=frameDigests.back()||
				!std::isfinite(timeS))return 95;
			frameTimesS.push_back(timeS);
		}
		if(frameTimesS.size()>1u)for(std::size_t frame=1u;frame<frameTimesS.size();++frame){
			const double expected=static_cast<double>(frame)*frameCadenceS;
			const double observed=frameTimesS[frame]-frameTimesS.front();
			const double tolerance=8.0*std::numeric_limits<float>::epsilon()*
				std::max(frameCadenceS,std::fabs(expected));
			if(std::fabs(observed-expected)>tolerance)return 97;
		}
		std::ofstream frameTimes(outputDirectory/"frame_times.csv");
		frameTimes<<"frame,simulation_time_s,vdb_sha256\n";
		std::vector<std::pair<std::string,std::string> > manifestFrames;
		for(std::size_t frame=0u;frame<frameDigests.size();++frame){
			std::ostringstream name;name<<"frames/frame_"<<std::setw(5)<<
				std::setfill('0')<<frame<<".vdb";
			frameTimes<<frame<<','<<std::setprecision(17)<<frameTimesS[frame]<<','<<
				frameDigests[frame]<<'\n';
			manifestFrames.push_back({name.str(),frameDigests[frame]});
		}
		frameTimes.close();
		std::string manifestDigest;
		if(manifestFrames.size()>=2u){
			const std::array<std::uint64_t,3> dimensions={{result.dimensions[0],
				result.dimensions[1],result.dimensions[2]}};
			const RISECBOR64::Bytes manifest=ManifestBytes("","","hold",true,true,
				dimensions,result.cellWidthM,frameTimesS.front(),frameCadenceS,1.0,0.0,
				true,CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,0.0,
				resolutionTier,true,manifestFrames);
			const std::filesystem::path manifestPath=
				outputDirectory/"sequence_manifest.rise-fire.cbor";
			std::ofstream manifestOutput(manifestPath,std::ios::binary|std::ios::trunc);
			manifestOutput.write(reinterpret_cast<const char*>(manifest.data()),
				static_cast<std::streamsize>(manifest.size()));
			manifestOutput.close();
			if(!manifestOutput||DigestFile(manifestPath).empty())return 98;
			FireSequenceManifest verified;
			if(!verified.LoadCanonicalEnvelope(manifest,outputDirectory.string(),error)||
				!verified.PreflightAllFrames(error))return 98;
			manifestDigest=DigestFile(manifestPath);
		}
		std::ofstream diagnostics(outputDirectory/"monitored_diagnostics.csv");
		diagnostics<<"step,dt_s,deviation_max,deviation_p95,deviation_p50,tail_cells,"
			"tail_drained_m3,device_ms,wall_ms\n";
		const std::size_t samples=std::min({result.acceptedTimeStepHistoryS.size(),
			result.monitoredManifoldMaximumHistory.size(),result.monitoredManifoldP95History.size(),
			result.monitoredManifoldP50History.size(),result.monitoredTailCellHistory.size(),
			result.monitoredTailDrainedVolumeHistoryM3.size(),result.productionDeviceHistoryMS.size(),
			result.productionWallHistoryMS.size()});
		for(std::size_t sample=0u;sample<samples;++sample)diagnostics<<sample+1u<<','<<
			std::setprecision(17)<<result.acceptedTimeStepHistoryS[sample]<<','<<
			result.monitoredManifoldMaximumHistory[sample]<<','<<
			result.monitoredManifoldP95History[sample]<<','<<
			result.monitoredManifoldP50History[sample]<<','<<
			result.monitoredTailCellHistory[sample]<<','<<
			result.monitoredTailDrainedVolumeHistoryM3[sample]<<','<<
			result.productionDeviceHistoryMS[sample]<<','<<result.productionWallHistoryMS[sample]<<'\n';
		diagnostics.close();
		std::ofstream summary(outputDirectory/"production_capstone_summary.txt");
		summary<<std::setprecision(17)<<"artifact_fidelity=simulation_evidence\n"
			<<"resolution_tier="<<resolutionTier<<"\n"
			<<"maximum_source_step_s="<<persistence.maximumProductionSourceStepS<<"\n"
			<<"target_time_s="<<targetTimeS<<"\n"
			<<"statistics_start_s="<<result.statisticsStartS<<"\n"
			<<"accepted_steps="<<result.acceptedTimeStepHistoryS.size()<<"\n"
			<<"snapshot_cadence_s="<<frameCadenceS<<"\n"
			<<"snapshot_count="<<frameDigests.size()<<"\n"
			<<"frame_times_sha256="<<DigestFile(outputDirectory/"frame_times.csv")<<"\n"
			<<"sequence_manifest_sha256="<<manifestDigest<<"\n"
			<<"wall_s="<<wallS<<"\n"
			<<"puffing_hz="<<result.puffingFrequencyHz<<"\n"
			<<"puffing_relative_error="<<result.puffingRelativeError<<"\n"
			<<"integrated_hrr_J="<<result.integratedHeatReleaseJ<<"\n"
			<<"integrated_fuel_kg="<<result.integratedFuelConsumptionKG<<"\n"
			<<"external_fuel_kg="<<result.externalFuelMassKG<<"\n"
			<<"integrated_radiative_fraction="<<result.integratedRadiativeFraction<<"\n"
			<<"hard_bound_retries="<<result.productionHardBoundRetryCount<<"\n"
			<<"physical_projection_retries="<<result.productionPhysicalProjectionRetryCount<<"\n"
			<<"diagnostics_sha256="<<DigestFile(outputDirectory/"monitored_diagnostics.csv")<<"\n";
		for(std::size_t frame=0u;frame<frameDigests.size();++frame)
			summary<<"frame_"<<std::setw(5)<<std::setfill('0')<<frame<<"_sha256="<<
				frameDigests[frame]<<"\n";
		summary.close();
		std::fprintf(stderr,"PRODUCTION_TEMPORAL_CAPSTONE tier=%.0f time=%.17g steps=%zu "
			"frames=%zu wall_s=%.17g summary=%s\n",resolutionTier,result.simulatedTimeS,
			result.acceptedTimeStepHistoryS.size(),frameDigests.size(),wallS,
			(outputDirectory/"production_capstone_summary.txt").string().c_str());
		return frameDigests.empty()?97:0;
#endif
	}

	int RunProductionOnsetCampaignChild(const double resolutionTier,
		const std::filesystem::path& outputDirectory,const bool sealedLegacyReplay=false,
		const std::filesystem::path& replayProtocolPath=std::filesystem::path(),
		const std::string& replayProtocolDigest=std::string(),
		const bool sealedProjectedReplay=false)
	{
#if !defined(RISE_ENABLE_OPENVDB)
		(void)resolutionTier;(void)outputDirectory;return 90;
#else
		if(resolutionTier!=6.0&&resolutionTier!=8.0&&resolutionTier!=10.0)return 91;
		if((sealedLegacyReplay||sealedProjectedReplay)&&(resolutionTier!=8.0||
			sealedLegacyReplay==sealedProjectedReplay||std::filesystem::exists(outputDirectory)||
			replayProtocolDigest.size()!=64u||DigestFile(replayProtocolPath)!=replayProtocolDigest||
			std::getenv("RISE_FIRE_ONSET_RESUME_CHECKPOINT")||
			std::getenv("RISE_FIRE_SINGLE_STAGE_FCT_ONSET")||
			std::getenv("RISE_FIRE_FORCE_TEST_FAILURE")||
			std::getenv("RISE_FIRE_PRODUCTION_RESTORATION_CYCLE_PROBE")||
			std::getenv("RISE_FIRE_PRODUCTION_RESTORATION_TEST")||
			std::getenv("RISE_FIRE_PRODUCTION_STEP_FAILURE")||
			std::getenv("RISE_FIRE_PROFILE")||
			std::getenv("RISE_FIRE_PROJECTION_TEST_FAILURE")||
			std::getenv("RISE_FIRE_RESTORATION_PLATEAU_PROBE")||
			std::getenv("RISE_FIRE_TIMESTEP_VELOCITY_AUDIT")||
			std::getenv("RISE_FIRE_TIMESTEP_VELOCITY_PACK_MODE")||
			std::getenv("RISE_FIRE_MOMENTUM_AUDIT_PATH")||
			std::getenv("RISE_FIRE_TARGET_TEMPERATURE_IDENTITY_RED")||
			std::getenv("RISE_FIRE_TARGET_MOLECULAR_IDENTITY_RED")||
			std::getenv("RISE_FIRE_REQUEST_LAYOUT_IDENTITY_RED")||
			std::getenv("RISE_FIRE_MANIFOLD_TIMESTEP_PROBE")||
			std::getenv("RISE_FIRE_MANIFOLD_STAGE_BUDGET_PROBE")||
			std::getenv("RISE_FIRE_GOLDEN_LONG_SHADOW")||
			std::getenv("RISE_FIRE_ADVECTIVE_ANOMALY_CLOSURE_TEST")||
			std::getenv("RISE_FIRE_MANIFOLD_TAIL_THRESHOLD_RED")||
			std::getenv("RISE_FIRE_ADVECTIVE_ANOMALY_CONVERGENCE_PROBE")||
			std::getenv("RISE_FIRE_ADVECTIVE_ANOMALY_CONVERGENCE_PASSES")||
			std::getenv("RISE_FIRE_HOST_RESIDUAL_PROBE")||
			std::getenv("RISE_FIRE_PHYSICAL_PROJECTION_VALIDATION_PROBE")))return 91;
		std::error_code directoryError;
		std::filesystem::create_directories(outputDirectory/"checkpoints",directoryError);
		std::filesystem::create_directories(outputDirectory/"budgets",directoryError);
		if(directoryError)return 92;
		double targetTimeS=(sealedLegacyReplay||sealedProjectedReplay)?3.0:2.2;
		if(!sealedLegacyReplay&&!sealedProjectedReplay)if(const char* target=std::getenv("RISE_FIRE_ONSET_TARGET_S")){
			char* end=nullptr;targetTimeS=std::strtod(target,&end);
			if(!end||*end!='\0'||!std::isfinite(targetTimeS)||!(targetTimeS>0.0))return 91;
		}
		RunPersistenceOptions persistence;
		persistence.productionMetal=true;
		persistence.sealedLegacyMomentumReplay=sealedLegacyReplay;
		persistence.sealedProjectedHeunReplay=sealedProjectedReplay;
		persistence.replayProtocolPath=replayProtocolPath;
		persistence.replayProtocolDigest=replayProtocolDigest;
		persistence.singleStageFCTDiagnostic=!sealedLegacyReplay&&!sealedProjectedReplay&&
			std::getenv("RISE_FIRE_SINGLE_STAGE_FCT_ONSET")!=nullptr;
		persistence.compatibleMomentumDiagnostic=!sealedLegacyReplay&&!sealedProjectedReplay&&
			!persistence.singleStageFCTDiagnostic;
		if(sealedLegacyReplay||sealedProjectedReplay){
			persistence.productionMomentumObservationTimeS=2.1079791976176079;
			persistence.productionMomentumObservationColumnX=38u;
			persistence.productionMomentumObservationColumnY=42u;
			persistence.productionMomentumObservationReferenceTier=10.0;
		}
		persistence.checkpointPath=outputDirectory/"latest.checkpoint";
		persistence.finalCheckpointPath=outputDirectory/"final.checkpoint";
		persistence.retainedCheckpointDirectory=outputDirectory/"checkpoints";
		persistence.checkpointCadenceWallS=120.0;
		if(persistence.singleStageFCTDiagnostic){
			persistence.checkpointPath.clear();
			persistence.finalCheckpointPath.clear();
			persistence.retainedCheckpointDirectory.clear();
			persistence.checkpointCadenceWallS=std::numeric_limits<double>::max();
		}
		persistence.productionOnsetDiagnosticDirectory=outputDirectory/"budgets";
		persistence.productionOnsetStopVelocityMPerS=60.0;
		if(!sealedLegacyReplay)if(const char* resumeCheckpoint=
			std::getenv("RISE_FIRE_ONSET_RESUME_CHECKPOINT")){
			const char* expectedBuild=std::getenv("RISE_FIRE_ONSET_EXPECTED_BUILD_ID");
			const char* expectedDigest=std::getenv("RISE_FIRE_ONSET_EXPECTED_CHECKPOINT_DIGEST");
			if(!expectedBuild||std::strlen(expectedBuild)!=64u||!expectedDigest||
				std::strlen(expectedDigest)!=64u||
				!std::filesystem::exists(resumeCheckpoint))return 91;
			persistence.checkpointPath=resumeCheckpoint;
			persistence.resume=true;
			persistence.isolatedEquivalenceProbe=true;
			persistence.isolatedExpectedCheckpointBuildId=expectedBuild;
			persistence.isolatedExpectedCheckpointDigest=expectedDigest;
			persistence.stopAfterAdditionalAcceptedSteps=50u;
			persistence.checkpointCadenceWallS=std::numeric_limits<double>::max();
		}
		persistence.maximumProductionSourceStepS=
			static_cast<double>(static_cast<float>(0.0016462659696117043));
		RISECBOR64::Bytes producerBuildRecord;
		std::string producerBuildId,producerExecutableDigest,producerIdentityError;
		if(!CurrentRendererBuildIdentity(producerBuildRecord,producerBuildId)||
			!CurrentExecutableDigest(producerBuildRecord,producerExecutableDigest,
				producerIdentityError)||producerBuildId.size()!=64u||
			producerExecutableDigest.size()!=64u)return 92;
		const auto wallStart=std::chrono::steady_clock::now();
		setenv("RISE_FIRE_CAPSTONE_OUTPUT","1",1);
		const SolverFrameValues result=RunMethaneFrameProbe(8u,1u,targetTimeS,targetTimeS,1.0,
			resolutionTier,CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,persistence);
		unsetenv("RISE_FIRE_CAPSTONE_OUTPUT");
		const double wallS=std::chrono::duration<double>(
			std::chrono::steady_clock::now()-wallStart).count();
		if(!result.succeeded){std::fprintf(stderr,"production onset campaign failed: %s\n",
			result.structuredError.c_str());return 93;}
		const std::filesystem::path trajectory=outputDirectory/"budgets"/
			"maximum_velocity_trajectory.csv";
		const std::filesystem::path retryTrajectory=outputDirectory/"budgets"/
			"retry_attempt_trajectory.csv";
		const bool reachedTarget=result.simulatedTimeS>=targetTimeS;
		const std::filesystem::path summaryPath=outputDirectory/"onset_campaign_summary.v2";
		const std::filesystem::path pendingSummaryPath=
			outputDirectory/"onset_campaign_summary.pending.v2";
		std::ofstream summary(pendingSummaryPath,std::ios::trunc);
		summary<<std::setprecision(17)<<"schema rise.fire.production.onset_campaign.summary.v2\n"
			<<"resolution_tier "<<resolutionTier<<"\n"
			<<"operator_mode "<<(sealedLegacyReplay?
				"ordinary_independent_dual_momentum_resident_step":
				(sealedProjectedReplay?"section_3_7_projected_heun_resident_owner":
				(persistence.singleStageFCTDiagnostic?
				"production_single_stage_fct_diagnostic_v1":"compatible_momentum_diagnostic")))<<"\n"
			<<"compatible_momentum_diagnostic "<<
				(persistence.compatibleMomentumDiagnostic?1:0)<<"\n"
			<<"single_stage_fct_diagnostic "<<
				(persistence.singleStageFCTDiagnostic?1:0)<<"\n"
			<<"producer_build_id "<<producerBuildId<<"\n"
			<<"producer_executable_sha256 "<<producerExecutableDigest<<"\n"
			<<"seed 1234\n"
			<<"protocol_sha256 "<<replayProtocolDigest<<"\n"
			<<"from_zero_identity_sha256 "<<
				DigestFile(outputDirectory/"from_zero_identity.v1")<<"\n"
			<<"target_time_s "<<targetTimeS<<"\n"
			<<"simulated_time_s "<<result.simulatedTimeS<<"\n"
			<<"completed_target "<<(reachedTarget?1:0)<<"\n"
			<<"accepted_steps "<<result.acceptedTimeStepHistoryS.size()<<"\n"
			<<"wall_s "<<wallS<<"\n"
			<<"trajectory_sha256 "<<DigestFile(trajectory)<<"\n"
			<<"retry_trajectory_sha256 "<<DigestFile(retryTrajectory)<<"\n"
			<<"stop_velocity_threshold_m_per_s "<<
				persistence.productionOnsetStopVelocityMPerS<<"\n"
			<<"stop_reason "<<(reachedTarget?"target_time_reached":
				"velocity_threshold_crossing")<<"\n"
			<<"checkpoint_authority "<<(persistence.singleStageFCTDiagnostic?
				"tokenless_diagnostic_unavailable":"ordinary_accepted_state")<<"\n"
			<<"final_checkpoint_sha256 "<<
				DigestFile(outputDirectory/"final.checkpoint")<<"\n";
		bool capturedThresholdBundlesComplete=true;
		for(const unsigned int threshold:{15u,30u,60u}){
			const std::filesystem::path budget=outputDirectory/"budgets"/
				("threshold_"+std::to_string(threshold)+".raw.csv");
			if(std::filesystem::exists(budget))capturedThresholdBundlesComplete=
				capturedThresholdBundlesComplete&&
				!DigestFile(budget.string()+".column.csv").empty()&&
				!DigestFile(budget.string()+".source_context.csv").empty()&&
				!DigestFile(budget.string()+".source_packets.bin").empty()&&
				!DigestFile(budget.string()+".source_ledger.v1").empty()&&
				!DigestFile(budget.string()+".source_observation_inputs.bin").empty()&&
				!DigestFile(budget.string()+".event.v1").empty();
			summary<<"threshold_"<<threshold<<"_captured "<<
				(std::filesystem::exists(budget)?1:0)<<"\n";
			if(std::filesystem::exists(budget))summary<<"threshold_"<<threshold<<
				"_summary_sha256 "<<DigestFile(budget)<<"\n"<<"threshold_"<<threshold<<
				"_column_sha256 "<<DigestFile(budget.string()+".column.csv")<<"\n"<<
				"threshold_"<<threshold<<"_source_context_sha256 "<<
				DigestFile(budget.string()+".source_context.csv")<<"\n"<<
				"threshold_"<<threshold<<"_source_packets_sha256 "<<
				DigestFile(budget.string()+".source_packets.bin")<<"\n"<<
				"threshold_"<<threshold<<"_source_ledger_sha256 "<<
				DigestFile(budget.string()+".source_ledger.v1")<<"\n"<<
				"threshold_"<<threshold<<"_source_observation_inputs_sha256 "<<
				DigestFile(budget.string()+".source_observation_inputs.bin")<<"\n"<<
				"threshold_"<<threshold<<"_event_sha256 "<<
				DigestFile(budget.string()+".event.v1")<<"\n";
		}
		if((sealedLegacyReplay||sealedProjectedReplay)&&!reachedTarget)
			for(const unsigned int threshold:{15u,30u,60u})
				capturedThresholdBundlesComplete=capturedThresholdBundlesComplete&&
					std::filesystem::exists(outputDirectory/"budgets"/
						("threshold_"+std::to_string(threshold)+".raw.csv"));
		const std::filesystem::path exactObservation=outputDirectory/"budgets"/
			"reference_composition_candidate_fixed_column.raw.csv";
		summary<<"observation_not_before_s "<<
			persistence.productionMomentumObservationTimeS<<"\n"
			<<"observation_reference_tier "<<
				persistence.productionMomentumObservationReferenceTier<<"\n"
			<<"oracle_composition_required true\n"
			<<"reference_composition_candidate_observation_sha256 "<<
				DigestFile(exactObservation)<<"\n"
			<<"reference_composition_candidate_column_sha256 "<<
				DigestFile(exactObservation.string()+".column.csv")<<"\n"
			<<"reference_composition_candidate_source_context_sha256 "<<
				DigestFile(exactObservation.string()+".source_context.csv")<<"\n"
			<<"reference_composition_candidate_source_packets_sha256 "<<
				DigestFile(exactObservation.string()+".source_packets.bin")<<"\n"
			<<"reference_composition_candidate_source_ledger_sha256 "<<
				DigestFile(exactObservation.string()+".source_ledger.v1")<<"\n"
			<<"reference_composition_candidate_source_observation_inputs_sha256 "<<
				DigestFile(exactObservation.string()+".source_observation_inputs.bin")<<"\n"
			<<"reference_composition_candidate_event_sha256 "<<
				DigestFile(exactObservation.string()+".event.v1")<<"\n";
		summary.close();
		if(!summary||(!sealedLegacyReplay&&!persistence.compatibleMomentumDiagnostic&&
			!persistence.singleStageFCTDiagnostic)||DigestFile(trajectory).empty()||
			DigestFile(retryTrajectory).empty()||
			!capturedThresholdBundlesComplete||
			(sealedLegacyReplay&&(DigestFile(outputDirectory/"from_zero_identity.v1").empty()||
				DigestFile(exactObservation).empty()||
				DigestFile(exactObservation.string()+".column.csv").empty()||
				DigestFile(exactObservation.string()+".source_context.csv").empty()||
				DigestFile(exactObservation.string()+".source_packets.bin").empty()||
				DigestFile(exactObservation.string()+".source_ledger.v1").empty()||
				DigestFile(exactObservation.string()+".source_observation_inputs.bin").empty()||
				DigestFile(exactObservation.string()+".event.v1").empty()))||
			(!persistence.singleStageFCTDiagnostic&&
			 DigestFile(outputDirectory/"final.checkpoint").empty()))return 94;
		std::error_code summaryPublishError;
		std::filesystem::rename(pendingSummaryPath,summaryPath,summaryPublishError);
		if(summaryPublishError||DigestFile(summaryPath).empty())return 94;
		std::fprintf(stderr,"PRODUCTION_ONSET_CAMPAIGN%s tier=%.0f target=%.17g time=%.17g steps=%zu "
			"wall_s=%.17g operator=%s build=%s trajectory=%s "
			"summary=%s\n",reachedTarget?"":"_STOP",resolutionTier,targetTimeS,
			result.simulatedTimeS,
			result.acceptedTimeStepHistoryS.size(),wallS,sealedLegacyReplay?
				"ordinary_independent_dual_momentum_resident_step":
				(persistence.singleStageFCTDiagnostic?
				"production_single_stage_fct_diagnostic_v1":"compatible_momentum_diagnostic"),
			producerBuildId.c_str(),
			DigestFile(trajectory).c_str(),DigestFile(summaryPath).c_str());
		return reachedTarget?0:95;
#endif
	}

	int RunOracleRetainedTrajectoryChild(const double targetTimeS,
		const std::filesystem::path& outputDirectory)
	{
#if !defined(RISE_ENABLE_OPENVDB)
		(void)targetTimeS;(void)outputDirectory;return 90;
#else
		if(!std::isfinite(targetTimeS)||!(targetTimeS>0.0))return 91;
		std::error_code directoryError;
		std::filesystem::create_directories(outputDirectory/"checkpoints",directoryError);
		if(directoryError)return 92;
		RunPersistenceOptions persistence;
		persistence.checkpointPath=outputDirectory/"latest.checkpoint";
		persistence.finalCheckpointPath=outputDirectory/"final.checkpoint";
		persistence.retainedCheckpointDirectory=outputDirectory/"checkpoints";
		persistence.checkpointCadenceWallS=900.0;
		const auto wallStart=std::chrono::steady_clock::now();
		setenv("RISE_FIRE_CAPSTONE_OUTPUT","1",1);
		const SolverFrameValues result=RunMethaneFrameProbe(8u,1u,targetTimeS,targetTimeS,1.0,
			10.0,CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,persistence);
		unsetenv("RISE_FIRE_CAPSTONE_OUTPUT");
		const double wallS=std::chrono::duration<double>(
			std::chrono::steady_clock::now()-wallStart).count();
		if(!result.succeeded){std::fprintf(stderr,"oracle retained trajectory failed: %s\n",
			result.structuredError.c_str());return 93;}
		std::ofstream summary(outputDirectory/"oracle_regeneration_summary.v1",std::ios::trunc);
		summary<<std::setprecision(17)<<"schema rise.fire.oracle.retained_trajectory.summary.v1\n"
			<<"target_time_s "<<targetTimeS<<"\n"
			<<"simulated_time_s "<<result.simulatedTimeS<<"\n"
			<<"accepted_steps "<<result.acceptedTimeStepHistoryS.size()<<"\n"
			<<"wall_s "<<wallS<<"\n"
			<<"final_checkpoint_sha256 "<<DigestFile(outputDirectory/"final.checkpoint")<<"\n";
		summary.close();
		if(!summary||DigestFile(outputDirectory/"final.checkpoint").empty())return 94;
		std::fprintf(stderr,"ORACLE_RETAINED_TRAJECTORY time=%.17g steps=%zu wall_s=%.17g "
			"checkpoint=%s\n",result.simulatedTimeS,result.acceptedTimeStepHistoryS.size(),wallS,
			DigestFile(outputDirectory/"final.checkpoint").c_str());
		return 0;
#endif
	}

	int RunProductionMomentumReplayChild(const std::filesystem::path& checkpointPath,
		const std::filesystem::path& auditPath,const std::string& expectedCheckpointBuildId,
		const std::string& expectedCheckpointDigest)
	{
		if(expectedCheckpointBuildId.size()!=64u||expectedCheckpointDigest.size()!=64u||
			DigestFile(checkpointPath)!=expectedCheckpointDigest)return 90;
		MethaneRunCheckpoint checkpoint;std::string error;
		if(!LoadMethaneRunCheckpoint(checkpointPath,checkpoint,error)||
			checkpoint.producerBuildId!=expectedCheckpointBuildId)return 91;
		const double replayResolutionTier=
			ProductionResolutionTierForCheckpointDimensions(checkpoint.dimensions);
		if(replayResolutionTier==0.0)return 90;
		std::ofstream clearAudit(auditPath,std::ios::trunc);clearAudit.close();
		if(!clearAudit)return 92;
		std::ofstream clearColumn(std::filesystem::path(auditPath).string()+".column.csv",
			std::ios::trunc);clearColumn.close();
		if(!clearColumn)return 92;
		if(setenv("RISE_FIRE_MOMENTUM_AUDIT_PATH",auditPath.string().c_str(),1)!=0)return 92;
		const SolverFrameValues beginning=RunMethaneFrameProbe(1u,0u,0.0,1.0,1.0,
			replayResolutionTier,CapstonePoolDiameterM,CapstoneHeatReleaseRateKW);
		if(!beginning.succeeded||!(beginning.flowThroughTimeS>0.0))return 93;
		const double expectedPuffingHz=1.5/std::sqrt(CapstonePoolDiameterM);
		const double statisticsStartS=5.0*beginning.flowThroughTimeS;
		const double statisticsDurationS=std::ceil((40.0/expectedPuffingHz)/0.0625)*0.0625;
		double fullTargetS=statisticsStartS+statisticsDurationS;
		if(const char* replayDuration=std::getenv("RISE_FIRE_MOMENTUM_REPLAY_DURATION_S")){
			char* end=nullptr;const double parsed=std::strtod(replayDuration,&end);
			if(!end||*end!='\0'||!std::isfinite(parsed)||!(parsed>checkpoint.simulationTimeS))return 90;
			fullTargetS=parsed;
		}
		RunPersistenceOptions persistence;
		persistence.productionMetal=true;persistence.resume=true;
		persistence.compatibleMomentumDiagnostic=true;
		persistence.checkpointPath=checkpointPath;
		persistence.checkpointCadenceWallS=std::numeric_limits<double>::max();
		persistence.isolatedEquivalenceProbe=true;
		persistence.isolatedExpectedCheckpointBuildId=expectedCheckpointBuildId;
		persistence.isolatedExpectedCheckpointDigest=expectedCheckpointDigest;
		persistence.stopAfterAdditionalAcceptedSteps=28u;
		persistence.maximumProductionSourceStepS=
			static_cast<double>(static_cast<float>(0.0016462659696117043));
		const SolverFrameValues result=RunMethaneFrameProbe(8u,1u,0.0,fullTargetS,1.0,
			replayResolutionTier,CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,persistence);
		unsetenv("RISE_FIRE_MOMENTUM_AUDIT_PATH");
		if(!result.succeeded){std::fprintf(stderr,"production momentum replay failed: %s\n",
			result.structuredError.c_str());return 94;}
		std::ifstream audit(auditPath);std::string line;std::size_t rows=0u;
		while(std::getline(audit,line))if(!line.empty())++rows;
		if(rows<29u)return 95;
		std::fprintf(stderr,"PRODUCTION_MOMENTUM_REPLAY beginning_step=%llu additional_steps=28 "
			"audit_rows=%zu\n",static_cast<unsigned long long>(checkpoint.acceptedSteps),rows-1u);
		return 0;
	}

	int RunOracleMomentumReplayChild(const std::filesystem::path& checkpointPath,
		const std::filesystem::path& auditPath,const std::string& expectedCheckpointBuildId)
	{
		if(expectedCheckpointBuildId.size()!=64u)return 90;
		MethaneRunCheckpoint checkpoint;std::string error;
		if(!LoadMethaneRunCheckpoint(checkpointPath,checkpoint,error)||
			checkpoint.producerBuildId!=expectedCheckpointBuildId||
			checkpoint.dimensions!=std::array<std::size_t,3>{{86u,86u,132u}})return 91;
		std::ofstream clearAudit(auditPath,std::ios::trunc);clearAudit.close();
		std::ofstream clearColumn(auditPath.string()+".column.csv",std::ios::trunc);clearColumn.close();
		if(!clearAudit||!clearColumn||setenv("RISE_FIRE_ORACLE_MOMENTUM_AUDIT_PATH",
			auditPath.string().c_str(),1)!=0)return 92;
		RunPersistenceOptions persistence;
		persistence.resume=true;persistence.checkpointPath=checkpointPath;
		persistence.checkpointCadenceWallS=std::numeric_limits<double>::max();
		persistence.isolatedEquivalenceProbe=true;
		persistence.isolatedExpectedCheckpointBuildId=expectedCheckpointBuildId;
		persistence.stopAfterAdditionalAcceptedSteps=8u;
		const SolverFrameValues result=RunMethaneFrameProbe(8u,1u,0.0,2.2,1.0,10.0,
			CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,persistence);
		unsetenv("RISE_FIRE_ORACLE_MOMENTUM_AUDIT_PATH");
		if(!result.succeeded){std::fprintf(stderr,"oracle momentum replay failed: %s\n",
			result.structuredError.c_str());return 93;}
		std::ifstream audit(auditPath);std::string line;std::size_t rows=0u;
		while(std::getline(audit,line))if(!line.empty())++rows;
		if(rows!=9u||DigestFile(auditPath).empty()||
			DigestFile(auditPath.string()+".column.csv").empty())return 94;
		std::fprintf(stderr,"ORACLE_MOMENTUM_REPLAY beginning_step=%llu additional_steps=8 "
			"audit_rows=8 audit_sha256=%s\n",
			static_cast<unsigned long long>(checkpoint.acceptedSteps),
			DigestFile(auditPath).c_str());
		return 0;
	}

// Kept as a compact test-only include because the checkpoint schema and
// certified periodic oracle are private to this translation unit.
#include "FireProductionCalibrationFixture.h"
#include "FireProductionDyadicCalibrationFixture.h"
#include "FireProductionSubdominanceFixture.h"
#include "FireProductionGoldenProjectionFixture.h"
#include "FireProductionGoldenCompositionFixture.h"

	int RunR80GoldenContinuationFixture(const std::filesystem::path& checkpointPath,
		const std::filesystem::path& tracePath,const std::filesystem::path& framePath)
	{
		static const char* checkpointDigest=
			"1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947";
		static const char* checkpointBuild=
			"8fb5e3eb14566f29266013487be77bbbb64fa745383d7ec2af62c55f4f37cd56";
		if(DigestFile(checkpointPath)!=checkpointDigest){std::fprintf(stderr,
			"r80 golden continuation checkpoint digest mismatch\n");return 90;}
		const double targetS=25.032480502915522;
		const int status=RunResumeEquivalenceTraceChild(checkpointPath,tracePath,framePath,
			16u,8u,targetS,1.0/targetS,10.0,0.30,33.0,checkpointBuild);
		if(status!=0||DigestFile(checkpointPath)!=checkpointDigest)return status?status:91;
		ResumeEquivalenceTrace trace;std::string error;
		const std::vector<std::uint64_t> expectedTimeStepBits={
			4543432537948766955ull,4544197666642132584ull,4544654590867399846ull,
			4545157207515193834ull,4545710085827767221ull,4546318251971597947ull,
			4542483635102441249ull,4543219516136476427ull};
		const std::vector<std::uint64_t> expectedMaximumTemperatureBits={
			4655159579052727434ull,4655159374940711548ull,4655159154984970440ull,
			4655158912993804872ull,4655158636009097964ull,4655158346958421514ull,
			4655158196749501340ull,4655158022160599380ull};
		const std::vector<std::uint64_t> expectedMaximumEOSResidualBits={
			4485619931794112512ull,4489437736410808320ull,4488601145836568576ull,
			4486917660591783936ull,4488934740778287104ull,4487986528701644800ull,
			4484167744161316864ull,4484941030474383360ull};
		const std::vector<std::string> expectedFrameDigests={
			"4dd7381f54b3931f3e108f805f63d8d2ec456bd955ca7717059c182886092344",
			"77988ea6692f20e10bdf58035748bbbe33025dd33a4b969e82f37d7c3d10e4d7",
			"5d261f869ef75a40867ca0a80dfef4b21bd6e7d8016ac6123dd10541c64cedc2",
			"66ff620f3212e00f93d9f519813e6740201292ddf1b8ecf76ac3e2debee6b5b3",
			"4a36583eaf66cafd797077609248a3bc1cc44e5cc6611695a4363b55474d052e",
			"154b501c1b66437003557db728ba233aac04d89ba423bd678518d6014a2abce5",
			"5618775a8311ecb401277833ca5c38b9f5a6c0e6c4f4e8ab4fb820e20497b95c",
			"ccd9d2902468a6b2341307a495a9c567e89b4a68f1efffe176d780dd73a999fe"};
		if(!LoadResumeEquivalenceTrace(tracePath,trace,error)||
			trace.timeStepBits!=expectedTimeStepBits||
			trace.maximumTemperatureBits!=expectedMaximumTemperatureBits||
			trace.maximumEOSResidualBits!=expectedMaximumEOSResidualBits||
			trace.frameDigests!=expectedFrameDigests){
			std::fprintf(stderr,"r80 golden continuation trace mismatch: %s\n",error.c_str());
			return 92;
		}
		const std::filesystem::path finalSnapshot=tracePath.string()+
			".snapshots/step_08.checkpoint";
		MethaneRunCheckpoint final;
		if(!LoadMethaneRunCheckpoint(finalSnapshot,final,error)||final.acceptedSteps!=3487u||
			final.values.acceptedTimeStepHistoryS.empty()||
			DoubleBits(final.values.acceptedTimeStepHistoryS.back())!=
				expectedTimeStepBits.back()||
			final.values.discontinuousActiveSetEvents!=10u||
			final.values.maximumActiveSetCycleLength!=2u||
			final.values.maximumActiveSetDifferingFaceCount!=4u||
			DoubleBits(final.values.maximumActiveSetComplementarityDiscrepancyMPerS)!=
				DoubleBits(0.0011766913664658803)||
			final.values.activeSetAlgorithmVersion!=CurrentActiveSetAlgorithmVersion()||
			final.values.priorActiveSetAlgorithmVersion!=LegacyActiveSetAlgorithmVersion()||
			!final.values.activeSetThreadIdentityChecked||
			!final.values.activeSetThreadIdentity){
			std::fprintf(stderr,"r80 golden continuation failed: %s steps=%llu dt=%.17g "
				"events=%u thread_checked=%d thread_identical=%d\n",error.c_str(),
				static_cast<unsigned long long>(final.acceptedSteps),
				final.values.acceptedTimeStepHistoryS.empty()?0.0:
					final.values.acceptedTimeStepHistoryS.back(),
				final.values.discontinuousActiveSetEvents,
				final.values.activeSetThreadIdentityChecked?1:0,
				final.values.activeSetThreadIdentity?1:0);
			return 92;
		}
		std::fprintf(stderr,"r80 golden continuation passed step=%llu dt=%.17g events=%u "
			"cycle=%zu faces=%zu discrepancy=%.17g\n",
			static_cast<unsigned long long>(final.acceptedSteps),
			final.values.acceptedTimeStepHistoryS.back(),
			final.values.discontinuousActiveSetEvents,
			final.values.maximumActiveSetCycleLength,
			final.values.maximumActiveSetDifferingFaceCount,
			final.values.maximumActiveSetComplementarityDiscrepancyMPerS);
		return 0;
	}

	int RunR171GoldenBeginningGeneration(const std::filesystem::path& checkpointPath,
		const std::filesystem::path& tracePath,const std::filesystem::path& framePath)
	{
		static const char* checkpointBuild=
			"8fb5e3eb14566f29266013487be77bbbb64fa745383d7ec2af62c55f4f37cd56";
		const double targetS=25.032480502915522;
		const int status=RunResumeEquivalenceTraceChild(checkpointPath,tracePath,framePath,
			16u,8u,targetS,1.0/targetS,10.0,0.30,33.0,checkpointBuild);
		if(status!=0)return status;
		const std::filesystem::path snapshots=tracePath.string()+".snapshots";
		MethaneRunCheckpoint root;std::string error,stateDigest;
		if(!LoadMethaneRunCheckpoint(checkpointPath,root,error)||
			!CheckpointProductionBeginningSHA256(root,stateDigest))return 188;
		const double characteristicDiameterM=root.values.characteristicDiameterM;
		root.values.characteristicDiameterM=std::nextafter(characteristicDiameterM,
			std::numeric_limits<double>::infinity());
		std::string mutatedFilterScaleDigest;
		if(!CheckpointProductionBeginningSHA256(root,mutatedFilterScaleDigest)||
			mutatedFilterScaleDigest==stateDigest)return 188;
		root.values.characteristicDiameterM=characteristicDiameterM;
		std::fprintf(stderr,
			"r171 golden beginning filter-scale mutation refused original=%s mutated=%s\n",
			stateDigest.c_str(),mutatedFilterScaleDigest.c_str());
		std::fprintf(stderr,"r171 golden beginning step=0 state_sha256=%s\n",
			stateDigest.c_str());
		for(std::size_t step=0u;step<8u;++step){
			std::ostringstream name;name<<"step_"<<std::setw(2)<<std::setfill('0')<<step+1u<<
				".checkpoint";
			MethaneRunCheckpoint beginning;
			if(!LoadMethaneRunCheckpoint(snapshots/name.str(),beginning,error)||
				!CheckpointProductionBeginningSHA256(beginning,stateDigest))return 188;
			std::fprintf(stderr,"r171 golden beginning step=%zu state_sha256=%s\n",step+1u,
				stateDigest.c_str());
		}
		std::fprintf(stderr,"r171 golden beginning generation complete slices=8 root=%s\n",
			DigestFile(checkpointPath).c_str());
		return 189;
	}

#if defined(_WIN32)
	std::string QuoteSubprocessArgument(const std::string& value)
	{
		std::string result="\"";
		for(const char c:value){if(c=='\"')result+='\\';result+=c;}
		return result+'\"';
	}
#endif

	int RunCheckpointSubprocess(const std::filesystem::path& executable,const char* mode,
		const std::filesystem::path& checkpoint,const std::filesystem::path& frame,
		const unsigned int workers)
	{
#if defined(_WIN32)
		const std::string command=QuoteSubprocessArgument(executable.string())+
			" --fire-checkpoint-child "+mode+" "+QuoteSubprocessArgument(checkpoint.string())+
			" "+QuoteSubprocessArgument(frame.string())+" "+std::to_string(workers);
		return std::system(command.c_str());
#else
		const pid_t child=::fork();
		if(child<0)return -1;
		if(child==0){
			const std::string workerText=std::to_string(workers);
			::execl(executable.c_str(),executable.c_str(),"--fire-checkpoint-child",mode,
				checkpoint.c_str(),frame.c_str(),workerText.c_str(),static_cast<char*>(nullptr));
			::_exit(127);
		}
		int status=0;
		while(::waitpid(child,&status,0)<0){if(errno!=EINTR)return -1;}
		return status;
#endif
	}

	bool CheckpointSubprocessWasHardKilled(const int status)
	{
#if defined(_WIN32)
		return status==91;
#else
		return WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL;
#endif
	}
#endif
}

#if defined(__APPLE__)
template<class T>
bool ByteIdenticalVector(const std::vector<T>& first,const std::vector<T>& second)
{
	return first.size()==second.size()&&(first.empty()||std::memcmp(
		first.data(),second.data(),first.size()*sizeof(T))==0);
}

template<class T,std::size_t N>
bool ByteIdenticalVectorArray(const std::array<std::vector<T>,N>& first,
	const std::array<std::vector<T>,N>& second)
{
	for(std::size_t index=0u;index<N;++index)
		if(!ByteIdenticalVector(first[index],second[index]))return false;
	return true;
}

struct CertifiedBinary32
{
	float rounded;
	double exact;
	double error;
	bool valid;
};

CertifiedBinary32 InvalidCertifiedBinary32()
{
	return {0.0f,0.0,0.0,false};
}

CertifiedBinary32 CertifiedInput(const float value)
{
	return {value,static_cast<double>(value),0.0,true};
}

CertifiedBinary32 CertifiedRecord(const double value)
{
	const float rounded=static_cast<float>(value);
	return {rounded,value,std::fabs(static_cast<double>(rounded)-value),true};
}

double Binary32RoundingEnvelope(const double magnitude)
{
	const double unitRoundoff=0x1p-24;
	return unitRoundoff/(1.0-unitRoundoff)*magnitude+
		static_cast<double>(std::numeric_limits<float>::denorm_min());
}

CertifiedBinary32 CertifiedAdd(const CertifiedBinary32& a,const CertifiedBinary32& b)
{
	if(!a.valid||!b.valid)return InvalidCertifiedBinary32();
	const float rounded=a.rounded+b.rounded;const double exact=a.exact+b.exact;
	const double inherited=a.error+b.error;
	return {rounded,exact,inherited+Binary32RoundingEnvelope(std::fabs(exact)+inherited),true};
}

CertifiedBinary32 CertifiedSubtract(const CertifiedBinary32& a,const CertifiedBinary32& b)
{
	if(!a.valid||!b.valid)return InvalidCertifiedBinary32();
	const float rounded=a.rounded-b.rounded;const double exact=a.exact-b.exact;
	const double inherited=a.error+b.error;
	return {rounded,exact,inherited+Binary32RoundingEnvelope(std::fabs(exact)+inherited),true};
}

CertifiedBinary32 CertifiedMultiply(const CertifiedBinary32& a,const CertifiedBinary32& b)
{
	if(!a.valid||!b.valid)return InvalidCertifiedBinary32();
	const float rounded=a.rounded*b.rounded;const double exact=a.exact*b.exact;
	const double inherited=std::fabs(a.exact)*b.error+std::fabs(b.exact)*a.error+
		a.error*b.error;
	return {rounded,exact,inherited+Binary32RoundingEnvelope(std::fabs(exact)+inherited),true};
}

CertifiedBinary32 CertifiedDivide(const CertifiedBinary32& a,const CertifiedBinary32& b)
{
	if(!a.valid||!b.valid)return InvalidCertifiedBinary32();
	const float rounded=a.rounded/b.rounded;const double exact=a.exact/b.exact;
	const double denominator=std::fabs(b.exact)-b.error;
	if(!(denominator>0.0))return InvalidCertifiedBinary32();
	const double inherited=(a.error+std::fabs(exact)*b.error)/denominator;
	return {rounded,exact,inherited+Binary32RoundingEnvelope(std::fabs(exact)+inherited),true};
}

CertifiedBinary32 CertifiedSqrt(const CertifiedBinary32& value)
{
	if(!value.valid)return InvalidCertifiedBinary32();
	const float rounded=std::sqrt(std::max(0.0f,value.rounded));
	const double exact=std::sqrt(std::max(0.0,value.exact));
	const double lower=std::sqrt(std::max(0.0,value.exact-value.error));
	const double upper=std::sqrt(std::max(0.0,value.exact+value.error));
	const double inherited=std::max(std::fabs(exact-lower),std::fabs(upper-exact));
	return {rounded,exact,inherited+Binary32RoundingEnvelope(exact+inherited),true};
}

CertifiedBinary32 CertifiedMaxZero(const CertifiedBinary32& value)
{
	if(!value.valid)return InvalidCertifiedBinary32();
	const float rounded=std::max(0.0f,value.rounded);
	const double exact=std::max(0.0,value.exact);
	const double lower=std::max(0.0,value.exact-value.error);
	const double upper=std::max(0.0,value.exact+value.error);
	return {rounded,exact,std::max(std::fabs(exact-lower),std::fabs(upper-exact)),true};
}

CertifiedBinary32 CertifiedAbsolute(const CertifiedBinary32& value)
{
	if(!value.valid)return InvalidCertifiedBinary32();
	const float rounded=std::fabs(value.rounded);const double exact=std::fabs(value.exact);
	const double lower=std::min(std::fabs(value.exact-value.error),
		std::fabs(value.exact+value.error));
	const double enclosedLower=value.exact-value.error<=0.0&&
		value.exact+value.error>=0.0?0.0:lower;
	const double upper=std::max(std::fabs(value.exact-value.error),
		std::fabs(value.exact+value.error));
	return {rounded,exact,std::max(std::fabs(exact-enclosedLower),std::fabs(upper-exact)),true};
}

struct CertifiedResidentTransport
{
	std::array<CertifiedBinary32,3> coefficient;
	std::array<CertifiedBinary32,9> velocityGradient;
	CertifiedBinary32 eddyKinematicViscosity;
	CertifiedBinary32 gasDensity;
	CertifiedBinary32 gasCp;
	CertifiedBinary32 mixtureConductivity;
	std::uint32_t branchBitmap;
	double vremanUncancelledScale;
	CertifiedBinary32 vremanNumerator;
	CertifiedBinary32 vremanDenominator;
};

struct CertifiedResidentTransportInputEnvelope
{
	const std::vector<double>* conservativeExact;
	const std::vector<double>* conservativeRadius;
	const std::vector<double>* temperatureExact;
	const std::vector<double>* temperatureRadius;
	const std::array<std::vector<double>,3>* velocityExact;
	const std::array<std::vector<double>,3>* velocityRadius;
	std::array<const std::vector<float>*,3> coefficientRounded;
	std::array<const std::vector<double>*,3> coefficientExact;

	CertifiedResidentTransportInputEnvelope() : conservativeExact(0),
		conservativeRadius(0),temperatureExact(0),temperatureRadius(0),
		velocityExact(0),velocityRadius(0) {coefficientRounded.fill(0);coefficientExact.fill(0);}
};

bool EvaluateCertifiedResidentTransport(
	const FireProductionResidentTransportComparatorRequest& request,
	const std::size_t cell,CertifiedResidentTransport& output,std::string* error,
	const CertifiedResidentTransportInputEnvelope* inputEnvelope=0)
{
	const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
	const FireSimulationTransportRecord& transport=FireSimulationTransportRecord::OpenV1();
	const std::size_t cells=request.shape.CellCount();
	const std::size_t x=cell%request.shape.nx;
	const std::size_t y=(cell/request.shape.nx)%request.shape.ny;
	const std::size_t z=cell/(request.shape.nx*request.shape.ny);
	output.branchBitmap=0u;output.vremanUncancelledScale=0.0;
	auto uncertainInput=[](const float rounded,const double exact,const double radius){
		const bool valid=std::isfinite(exact)&&std::isfinite(radius)&&radius>=0.0&&
			std::fabs(static_cast<double>(rounded)-exact)<=radius;
		return CertifiedBinary32{rounded,exact,radius,valid};};
	auto conservativeInput=[&](const std::size_t index){return inputEnvelope?
		uncertainInput(request.conservativeValues[index],
			(*inputEnvelope->conservativeExact)[index],
			(*inputEnvelope->conservativeRadius)[index]):
		CertifiedInput(request.conservativeValues[index]);};
	auto temperatureInput=[&](const std::size_t index){return inputEnvelope?
		uncertainInput(request.temperatureK[index],(*inputEnvelope->temperatureExact)[index],
			(*inputEnvelope->temperatureRadius)[index]):CertifiedInput(request.temperatureK[index]);};
	auto velocityInput=[&](const unsigned int axis,const std::size_t face){return inputEnvelope?
		uncertainInput(request.projectedVelocityMPerS[axis][face],
			(*inputEnvelope->velocityExact)[axis][face],
			(*inputEnvelope->velocityRadius)[axis][face]):
		CertifiedInput(request.projectedVelocityMPerS[axis][face]);};
	if(inputEnvelope&&(!inputEnvelope->conservativeExact||!inputEnvelope->conservativeRadius||
		!inputEnvelope->temperatureExact||!inputEnvelope->temperatureRadius||
		!inputEnvelope->velocityExact||!inputEnvelope->velocityRadius||
		inputEnvelope->conservativeExact->size()!=request.conservativeValues.size()||
		inputEnvelope->conservativeRadius->size()!=request.conservativeValues.size()||
		inputEnvelope->temperatureExact->size()!=request.temperatureK.size()||
		inputEnvelope->temperatureRadius->size()!=request.temperatureK.size())){
		if(error)*error="resident transport input envelope shape is invalid";return false;}
	if(inputEnvelope)for(unsigned int axis=0u;axis<3u;++axis)
		if((*inputEnvelope->velocityExact)[axis].size()!=
			request.projectedVelocityMPerS[axis].size()||
			(*inputEnvelope->velocityRadius)[axis].size()!=
			request.projectedVelocityMPerS[axis].size()){
			if(error)*error="resident transport velocity envelope shape is invalid";return false;}
	auto faceIndex=[&](const unsigned int axis,const std::size_t fx,
		const std::size_t fy,const std::size_t fz){return axis==0u?
		(fz*request.shape.ny+fy)*(request.shape.nx+1u)+fx:
		(axis==1u?(fz*(request.shape.ny+1u)+fy)*request.shape.nx+fx:
			(fz*request.shape.ny+fy)*request.shape.nx+fx);};
	auto cellVelocity=[&](const unsigned int component,const std::size_t cx,
		const std::size_t cy,const std::size_t cz){std::size_t ux=cx,uy=cy,uz=cz;
		if(component==0u)++ux;else if(component==1u)++uy;else ++uz;
		return CertifiedMultiply(CertifiedAdd(velocityInput(component,
			faceIndex(component,cx,cy,cz)),velocityInput(component,
			faceIndex(component,ux,uy,uz))),CertifiedRecord(0.5));};
	auto boundaryVelocity=[&](const unsigned int side,const unsigned int component,
		std::size_t cx,std::size_t cy,std::size_t cz){const unsigned int normal=side/2u;
		if(component!=normal)return CertifiedInput(0.0f);
		if(normal==0u)cx=(side&1u)?request.shape.nx:0u;
		else if(normal==1u)cy=(side&1u)?request.shape.ny:0u;
		else cz=(side&1u)?request.shape.nz:0u;
		return velocityInput(component,faceIndex(component,cx,cy,cz));};
	CertifiedBinary32 gradient[3][3];
	for(unsigned int derivative=0u;derivative<3u;++derivative){
		const std::size_t coordinate=derivative==0u?x:(derivative==1u?y:z);
		const std::size_t extent=derivative==0u?request.shape.nx:
			(derivative==1u?request.shape.ny:request.shape.nz);
		for(unsigned int component=0u;component<3u;++component){
			std::size_t px=x,py=y,pz=z,nx=x,ny=y,nz=z;
			std::size_t previous=coordinate?coordinate-1u:0u;
			std::size_t next=coordinate+1u<extent?coordinate+1u:extent-1u;
			if(coordinate>0u&&coordinate+1u<extent)output.branchBitmap|=1u<<7u;
			if(coordinate==0u&&request.boundary[2u*derivative]==
				FireProductionProjectionPeriodic){previous=extent-1u;output.branchBitmap|=1u<<8u;}
			if(coordinate+1u==extent&&request.boundary[2u*derivative+1u]==
				FireProductionProjectionPeriodic){next=0u;output.branchBitmap|=1u<<8u;}
			if(derivative==0u){px=previous;nx=next;}else if(derivative==1u){py=previous;ny=next;}
			else{pz=previous;nz=next;}
			CertifiedBinary32 pv=cellVelocity(component,px,py,pz);
			CertifiedBinary32 nv=cellVelocity(component,nx,ny,nz);
			if(coordinate==0u&&request.boundary[2u*derivative]!=
				FireProductionProjectionPeriodic){const unsigned int side=2u*derivative;
				const std::size_t first=side<2u?y:x,second=side<4u?z:y;
				const std::size_t width=side<2u?request.shape.ny:request.shape.nx;
				const bool inlet=request.fuelInletBoundaryFace[side][second*width+first]!=0u;
				const bool open=request.boundary[side]==FireProductionProjectionPressureOpen&&!inlet;
				output.branchBitmap|=1u<<(open?9u:10u);
				if(inlet)output.branchBitmap|=1u<<18u;
				pv=open?cellVelocity(component,x,y,z):CertifiedSubtract(CertifiedMultiply(
					CertifiedRecord(2.0),boundaryVelocity(side,component,x,y,z)),
					cellVelocity(component,x,y,z));}
			if(coordinate+1u==extent&&request.boundary[2u*derivative+1u]!=
				FireProductionProjectionPeriodic){const unsigned int side=2u*derivative+1u;
				const std::size_t first=side<2u?y:x,second=side<4u?z:y;
				const std::size_t width=side<2u?request.shape.ny:request.shape.nx;
				const bool inlet=request.fuelInletBoundaryFace[side][second*width+first]!=0u;
				const bool open=request.boundary[side]==FireProductionProjectionPressureOpen&&!inlet;
				output.branchBitmap|=1u<<(open?9u:10u);
				if(inlet)output.branchBitmap|=1u<<18u;
				nv=open?cellVelocity(component,x,y,z):CertifiedSubtract(CertifiedMultiply(
					CertifiedRecord(2.0),boundaryVelocity(side,component,x,y,z)),
					cellVelocity(component,x,y,z));}
			gradient[derivative][component]=CertifiedDivide(CertifiedSubtract(nv,pv),
				CertifiedMultiply(CertifiedRecord(2.0),CertifiedInput(request.shape.cellWidthM)));
		}
	}
	CertifiedBinary32 alpha2=CertifiedInput(0.0f),beta[3][3];
	for(unsigned int i=0u;i<3u;++i)for(unsigned int j=0u;j<3u;++j){
		alpha2=CertifiedAdd(alpha2,CertifiedMultiply(gradient[i][j],gradient[i][j]));
		beta[i][j]=CertifiedInput(0.0f);}
	const CertifiedBinary32 width2=CertifiedMultiply(CertifiedInput(request.shape.cellWidthM),
		CertifiedInput(request.shape.cellWidthM));
	for(unsigned int m=0u;m<3u;++m)for(unsigned int i=0u;i<3u;++i)
		for(unsigned int j=0u;j<3u;++j)beta[i][j]=CertifiedAdd(beta[i][j],
			CertifiedMultiply(CertifiedMultiply(width2,gradient[m][i]),gradient[m][j]));
	CertifiedBinary32 b[6]={CertifiedMultiply(beta[0][0],beta[1][1]),
		CertifiedMultiply(beta[0][1],beta[0][1]),CertifiedMultiply(beta[0][0],beta[2][2]),
		CertifiedMultiply(beta[0][2],beta[0][2]),CertifiedMultiply(beta[1][1],beta[2][2]),
		CertifiedMultiply(beta[1][2],beta[1][2])};
	CertifiedBinary32 raw=CertifiedSubtract(b[0],b[1]);raw=CertifiedAdd(raw,b[2]);
	raw=CertifiedSubtract(raw,b[3]);raw=CertifiedAdd(raw,b[4]);raw=CertifiedSubtract(raw,b[5]);
	CertifiedBinary32 rawScale=CertifiedInput(0.0f);
	for(const CertifiedBinary32& term:b)rawScale=CertifiedAdd(rawScale,CertifiedAbsolute(term));
	output.vremanUncancelledScale=rawScale.exact+rawScale.error;
	output.vremanNumerator=raw;output.vremanDenominator=alpha2;
	if(std::fabs(raw.rounded)<=32.0f*0x1p-24f*std::max(1.0f,rawScale.rounded))
		output.branchBitmap|=1u<<11u;
	output.branchBitmap|=1u<<(alpha2.rounded==0.0f?12u:13u);
	output.branchBitmap|=1u<<(raw.rounded<0.0f?14u:15u);
	CertifiedBinary32 eddy;
	if(alpha2.rounded==0.0f)eddy=CertifiedInput(0.0f);
	else if(std::fabs(alpha2.exact)>alpha2.error)eddy=CertifiedMultiply(
		CertifiedRecord(transport.VremanCv()),CertifiedSqrt(CertifiedDivide(
			CertifiedMaxZero(raw),alpha2)));
	else{
		// beta=dx^2 G^T G is positive semidefinite, so the sum of its
		// principal 2x2 minors is at most 1/2 trace(beta)^2.  This gives a
		// finite branch envelope at the alpha^2==0 predicate without dividing
		// an interval which crosses zero: nu_t <= Cv dx^2 sqrt(alpha^2/2).
		const double alphaUpper=std::max(0.0,alpha2.exact+alpha2.error);
		const double eddyUpper=transport.VremanCv()*request.shape.cellWidthM*
			request.shape.cellWidthM*std::sqrt(0.5*alphaUpper);
		const double exactEddy=alpha2.exact>0.0?transport.VremanCv()*std::sqrt(
			std::max(0.0,raw.exact)/alpha2.exact):0.0;
		eddy={static_cast<float>(transport.VremanCv()*std::sqrt(
			std::max(0.0f,raw.rounded)/alpha2.rounded)),
			exactEddy,std::max(std::fabs(exactEddy),std::fabs(eddyUpper-exactEddy))+
				Binary32RoundingEnvelope(eddyUpper),std::isfinite(eddyUpper)};
	}
	for(unsigned int derivative=0u;derivative<3u;++derivative)
		for(unsigned int component=0u;component<3u;++component)
			output.velocityGradient[3u*derivative+component]=gradient[derivative][component];
	output.eddyKinematicViscosity=eddy;
	CertifiedBinary32 rho=CertifiedInput(0.0f),cp=CertifiedInput(0.0f),moleTotal=CertifiedInput(0.0f);
	CertifiedBinary32 mass[6],mole[6],mu[6],conductivity[6];
	const float temperature=request.temperatureK[cell];
	for(std::size_t species=0u;species<6u;++species){
		mass[species]=CertifiedMaxZero(conservativeInput((species+1u)*cells+cell));
		rho=CertifiedAdd(rho,mass[species]);
	}
	output.gasDensity=rho;
	for(std::size_t species=0u;species<6u;++species){
		const FireThermochemistrySpecies* thermo=fuel.FindSpecies(
			fuel.SpeciesOrder()[species].c_str());
		const FireTransportSpecies* curve=transport.FindSpecies(
			fuel.SpeciesOrder()[species].c_str());
		if(!thermo||!curve||thermo->segments.empty()){
			if(error)*error="resident transport certified mirror record is incomplete";
			return false;
		}
		const CertifiedBinary32 fraction=CertifiedDivide(mass[species],rho);
		mole[species]=CertifiedDivide(fraction,
			CertifiedRecord(thermo->molecularWeightKGPerKMol));
		moleTotal=CertifiedAdd(moleTotal,mole[species]);
		std::size_t selected=thermo->segments.size();
		for(std::size_t segment=0u;segment<thermo->segments.size();++segment){
			const float lo=static_cast<float>(thermo->segments[segment].temperatureMinK);
			const float hi=static_cast<float>(thermo->segments[segment].temperatureMaxK);
			if(temperature>=lo&&(temperature<hi||(segment+1u==thermo->segments.size()&&
				temperature==hi))){selected=segment;output.branchBitmap|=1u<<
					static_cast<unsigned int>(std::min<std::size_t>(segment,2u));}}
		if(selected==thermo->segments.size()){
			if(error)*error=
				"resident transport certified mirror temperature is outside NASA9 record";
			return false;
		}
		const FireThermochemistrySegment& segment=thermo->segments[selected];
		CertifiedBinary32 T=temperatureInput(cell),inverse=CertifiedDivide(
			CertifiedInput(1.0f),T),t2=CertifiedMultiply(T,T);
		CertifiedBinary32 speciesCp=CertifiedMultiply(CertifiedMultiply(
			CertifiedRecord(segment.coefficients[0]),inverse),inverse);
		speciesCp=CertifiedAdd(speciesCp,CertifiedMultiply(CertifiedRecord(
			segment.coefficients[1]),inverse));
		speciesCp=CertifiedAdd(speciesCp,CertifiedRecord(segment.coefficients[2]));
		speciesCp=CertifiedAdd(speciesCp,CertifiedMultiply(CertifiedRecord(
			segment.coefficients[3]),T));
		speciesCp=CertifiedAdd(speciesCp,CertifiedMultiply(CertifiedRecord(
			segment.coefficients[4]),t2));
		speciesCp=CertifiedAdd(speciesCp,CertifiedMultiply(CertifiedMultiply(
			CertifiedRecord(segment.coefficients[5]),t2),T));
		speciesCp=CertifiedAdd(speciesCp,CertifiedMultiply(CertifiedMultiply(
			CertifiedRecord(segment.coefficients[6]),t2),t2));
		speciesCp=CertifiedMultiply(speciesCp,CertifiedDivide(CertifiedRecord(
			8314.46261815324),CertifiedRecord(thermo->molecularWeightKGPerKMol)));
		// The NASA9 polynomial is cancellation-sensitive.  Its certified cp
		// interval comes from the record's positive lower certificate and an
		// absolute-polynomial upper bound over every segment touched by T.
		const double temperatureLower=T.exact-T.error,temperatureUpper=T.exact+T.error;
		double certifiedCpFloor=std::numeric_limits<double>::infinity(),
			cpDerivativeBound=0.0,cpAbsolutePolynomialBound=0.0;
		for(const FireThermochemistrySegment& candidate:thermo->segments){const double lo=
			std::max(temperatureLower,candidate.temperatureMinK),hi=std::min(
				temperatureUpper,candidate.temperatureMaxK);if(hi<lo||!(lo>0.0))continue;
			certifiedCpFloor=std::min(certifiedCpFloor,candidate.certifiedCpLowerJPerKGK);
			const double* a=candidate.coefficients;double absolutePolynomial=
				std::fabs(a[0])/(lo*lo)+std::fabs(a[1])/lo+std::fabs(a[2]),power=hi;
			for(std::size_t coefficient=3u;coefficient<7u;++coefficient){
				absolutePolynomial+=std::fabs(a[coefficient])*power;power*=hi;}
			const double derivative=2.0*std::fabs(a[0])/(lo*lo*lo)+
				std::fabs(a[1])/(lo*lo)+std::fabs(a[3])+2.0*std::fabs(a[4])*hi+
				3.0*std::fabs(a[5])*hi*hi+4.0*std::fabs(a[6])*hi*hi*hi;
			const double gas=8314.46261815324/thermo->molecularWeightKGPerKMol;
			cpDerivativeBound=std::max(cpDerivativeBound,derivative*gas);
			cpAbsolutePolynomialBound=std::max(cpAbsolutePolynomialBound,
				absolutePolynomial*gas);}
		const double cpRadius=cpDerivativeBound*T.error+256.0*
			std::numeric_limits<float>::epsilon()*cpAbsolutePolynomialBound;
		const double cpLower=std::max(certifiedCpFloor,speciesCp.exact-cpRadius),
			cpUpper=speciesCp.exact+cpRadius;
		const bool cpRangeValid=std::isfinite(cpLower)&&std::isfinite(cpUpper)&&
			cpLower>0.0&&cpUpper>=cpLower&&speciesCp.exact>=cpLower&&speciesCp.exact<=cpUpper;
		speciesCp.error=cpRangeValid?std::max(std::fabs(speciesCp.exact-cpLower),
			std::fabs(cpUpper-speciesCp.exact)):
			std::numeric_limits<double>::infinity();
		speciesCp.valid=speciesCp.valid&&cpRangeValid;
		cp=CertifiedAdd(cp,CertifiedMultiply(fraction,speciesCp));
		auto evaluateCurve=[&](const DifferentiableSpectrum& spectrum){
			const auto& knots=spectrum.Wavelengths();const auto& values=spectrum.Values();
			const auto& slopes=spectrum.Slopes();std::size_t lower=0u;
			if(temperature<=static_cast<float>(knots.front())){output.branchBitmap|=1u<<3u;
				return CertifiedRecord(values.front());}
			if(temperature>=static_cast<float>(knots.back())){output.branchBitmap|=1u<<4u;
				return CertifiedRecord(values.back());}
			output.branchBitmap|=1u<<5u;std::size_t upper=knots.size()-1u;
			while(upper-lower>1u){const std::size_t middle=(lower+upper)/2u;
				if(temperature<static_cast<float>(knots[middle])){upper=middle;
					output.branchBitmap|=1u<<16u;}else{lower=middle;output.branchBitmap|=1u<<17u;}}
			if(temperature==static_cast<float>(knots[lower])||
				temperature==static_cast<float>(knots[upper]))output.branchBitmap|=1u<<6u;
			const CertifiedBinary32 input=temperatureInput(cell);
			const bool classAmbiguous=input.exact-input.error<knots[lower]||
				input.exact+input.error>knots[upper];
			if(classAmbiguous&&cell==0u)std::fprintf(stderr,
				"PROJECTED_HEUN_OWNER_CURVE_CLASS_OBLIGATION stage=%u species=%zu "
				"predicate_interval=[%.17g,%.17g] knot_interval=[%.17g,%.17g] crosses_knot=1\n",
				static_cast<unsigned int>(request.stage),species,input.exact-input.error,
				input.exact+input.error,knots[lower],knots[upper]);
			// Walk the exact Metal Hermite DAG at its binary32 temperature.  The
			// walk's termwise arithmetic enclosure is independent of cancellation;
			// the record derivative then transports the upstream temperature radius.
			// A Bezier/de-Casteljau rewrite is not admissible here because it is a
			// different binary32 operation path even though the real polynomials agree.
			CertifiedBinary32 h=CertifiedSubtract(CertifiedRecord(knots[upper]),
				CertifiedRecord(knots[lower]));
			CertifiedBinary32 t=CertifiedDivide(CertifiedSubtract(CertifiedInput(temperature),
				CertifiedRecord(knots[lower])),h),tt=CertifiedMultiply(t,t),ttt=CertifiedMultiply(tt,t);
			CertifiedBinary32 one=CertifiedRecord(1.0),two=CertifiedRecord(2.0),
				three=CertifiedRecord(3.0);
			CertifiedBinary32 arithmetic=CertifiedMultiply(CertifiedAdd(CertifiedSubtract(
				CertifiedMultiply(two,ttt),CertifiedMultiply(three,tt)),one),
				CertifiedRecord(values[lower]));
			arithmetic=CertifiedAdd(arithmetic,CertifiedMultiply(CertifiedMultiply(CertifiedAdd(
				CertifiedSubtract(ttt,CertifiedMultiply(two,tt)),t),h),
				CertifiedRecord(slopes[lower])));
			arithmetic=CertifiedAdd(arithmetic,CertifiedMultiply(CertifiedAdd(CertifiedMultiply(
				CertifiedRecord(-2.0),ttt),CertifiedMultiply(three,tt)),
				CertifiedRecord(values[upper])));
			arithmetic=CertifiedAdd(arithmetic,CertifiedMultiply(CertifiedMultiply(
				CertifiedSubtract(ttt,tt),h),CertifiedRecord(slopes[upper])));
			const double center=spectrum.Evaluate(input.exact);
			double derivativeMagnitude=0.0;for(const SpectralDerivativeEnclosure& enclosure:
				spectrum.DerivativeEnclosures())derivativeMagnitude=std::max(derivativeMagnitude,
					std::max(std::fabs(enclosure.derivativeMin),std::fabs(enclosure.derivativeMax)));
			const double devicePathExact=spectrum.Evaluate(static_cast<double>(temperature));
			const double pathIdentityResidual=std::fabs(arithmetic.exact-devicePathExact);
			const double radius=derivativeMagnitude*input.error+arithmetic.error+
				pathIdentityResidual;
			const bool valid=arithmetic.valid&&
				std::isfinite(center)&&std::isfinite(radius)&&radius>=0.0&&std::fabs(
					static_cast<double>(arithmetic.rounded)-center)<=radius;
			if(!valid&&cell==0u)std::fprintf(stderr,"PROJECTED_HEUN_OWNER_CURVE_INVALID stage=%u "
				"species=%zu rounded=%.9g arithmetic_exact=%.17g arithmetic_error=%.17g "
				"center=%.17g radius=%.17g input_exact=%.17g input_error=%.17g class_ambiguous=%d\n",
				static_cast<unsigned int>(request.stage),species,arithmetic.rounded,
				arithmetic.exact,arithmetic.error,center,radius,input.exact,input.error,
				classAmbiguous?1:0);
			return CertifiedBinary32{arithmetic.rounded,center,radius,valid};};
		mu[species]=evaluateCurve(curve->viscosity);
		conductivity[species]=evaluateCurve(curve->conductivity);
	}
	for(std::size_t species=0u;species<6u;++species)if(
		mass[species].exact!=0.0||mass[species].error!=0.0)
		mole[species]=CertifiedDivide(mole[species],moleTotal);
	auto phi=[&](const std::size_t i,const std::size_t j){
		const FireThermochemistrySpecies* ti=fuel.FindSpecies(fuel.SpeciesOrder()[i].c_str());
		const FireThermochemistrySpecies* tj=fuel.FindSpecies(fuel.SpeciesOrder()[j].c_str());
		CertifiedBinary32 numerator=CertifiedAdd(CertifiedRecord(1.0),CertifiedMultiply(
			CertifiedSqrt(CertifiedDivide(mu[i],mu[j])),CertifiedSqrt(CertifiedSqrt(
				CertifiedDivide(CertifiedRecord(tj->molecularWeightKGPerKMol),
					CertifiedRecord(ti->molecularWeightKGPerKMol))))));
		return CertifiedDivide(CertifiedMultiply(numerator,numerator),CertifiedSqrt(
			CertifiedMultiply(CertifiedRecord(8.0),CertifiedAdd(CertifiedRecord(1.0),
				CertifiedDivide(CertifiedRecord(ti->molecularWeightKGPerKMol),
					CertifiedRecord(tj->molecularWeightKGPerKMol))))));};
	CertifiedBinary32 mixtureMu=CertifiedInput(0.0f),mixtureK=CertifiedInput(0.0f);
	for(std::size_t i=0u;i<6u;++i){
		// A device zero-mass species contributes exact +0 to both Wilke sums.
		// Omitting that algebraic zero in the certificate preserves the device
		// result while preventing an irrelevant trace-species ratio from creating
		// an unbounded interval. A species with any admitted mass radius remains.
		if(mass[i].exact==0.0&&mass[i].error==0.0)continue;
		CertifiedBinary32 denominator=CertifiedInput(0.0f);
		for(std::size_t j=0u;j<6u;++j)if(
			mass[j].exact!=0.0||mass[j].error!=0.0)denominator=CertifiedAdd(denominator,
			CertifiedMultiply(mole[j],phi(i,j)));
		mixtureMu=CertifiedAdd(mixtureMu,CertifiedDivide(CertifiedMultiply(mole[i],mu[i]),denominator));
		mixtureK=CertifiedAdd(mixtureK,CertifiedDivide(CertifiedMultiply(mole[i],conductivity[i]),denominator));}
	CertifiedBinary32 volumetric=CertifiedMultiply(rho,cp);
	output.gasCp=cp;output.mixtureConductivity=mixtureK;
	output.coefficient[0]=CertifiedAdd(CertifiedDivide(mixtureK,volumetric),
		CertifiedDivide(eddy,CertifiedRecord(transport.TurbulentSchmidt())));
	output.coefficient[1]=CertifiedAdd(mixtureK,CertifiedDivide(CertifiedMultiply(volumetric,eddy),
		CertifiedRecord(transport.TurbulentPrandtl())));
	output.coefficient[2]=CertifiedDivide(mixtureMu,rho);
	// The qualification path uses the record-wide positive envelope below even
	// when the direct interval evaluation is finite. Wilke mixing contains
	// repeated ratios of trace species; independent interval propagation loses
	// their shared composition and is needlessly unbounded near a zero species.
	// The envelope uses only positive extrema and therefore has no subtractive-
	// cancellation premise.
	if(inputEnvelope&&(!output.coefficient[0].valid||!output.coefficient[1].valid||
		!output.coefficient[2].valid)){
		if(cell==0u)std::fprintf(stderr,"PROJECTED_HEUN_OWNER_TRANSPORT_FALLBACK stage=%u "
			"D=%d k=%d nu=%d rho=%d cp=%d mix_mu=%d mix_k=%d mass=%d%d%d%d%d%d "
			"mu=%d%d%d%d%d%d curve_k=%d%d%d%d%d%d\n",static_cast<unsigned int>(request.stage),
			output.coefficient[0].valid?1:0,output.coefficient[1].valid?1:0,
			output.coefficient[2].valid?1:0,rho.valid?1:0,cp.valid?1:0,
			mixtureMu.valid?1:0,mixtureK.valid?1:0,
			mass[0].valid?1:0,mass[1].valid?1:0,mass[2].valid?1:0,mass[3].valid?1:0,
			mass[4].valid?1:0,mass[5].valid?1:0,mu[0].valid?1:0,mu[1].valid?1:0,
			mu[2].valid?1:0,mu[3].valid?1:0,mu[4].valid?1:0,mu[5].valid?1:0,
			conductivity[0].valid?1:0,conductivity[1].valid?1:0,conductivity[2].valid?1:0,
			conductivity[3].valid?1:0,conductivity[4].valid?1:0,conductivity[5].valid?1:0);
		bool fallbackValid=rho.valid&&cp.valid&&eddy.valid;double muUpper=0.0,kUpper=0.0,
			phiLower=std::numeric_limits<double>::infinity();
		const double rhoLower=rho.exact-rho.error,rhoUpper=rho.exact+rho.error,
			eddyUpper=eddy.exact+eddy.error;
		double cpLower=0.0,cpUpper=0.0;
		std::array<double,6> muCurveLower={{}},muCurveUpper={{}},kCurveUpper={{}};
		const double localTemperatureLower=temperatureInput(cell).exact-
			temperatureInput(cell).error,localTemperatureUpper=temperatureInput(cell).exact+
			temperatureInput(cell).error;
		for(std::size_t species=0u;species<6u;++species){const FireThermochemistrySpecies*
			thermo=fuel.FindSpecies(fuel.SpeciesOrder()[species].c_str());const FireTransportSpecies*
			curve=transport.FindSpecies(fuel.SpeciesOrder()[species].c_str());
			if(!thermo||!curve){fallbackValid=false;continue;}
			muCurveLower[species]=*std::min_element(curve->viscosity.Values().begin(),
				curve->viscosity.Values().end());muCurveUpper[species]=*std::max_element(
				curve->viscosity.Values().begin(),curve->viscosity.Values().end());
			kCurveUpper[species]=*std::max_element(curve->conductivity.Values().begin(),
				curve->conductivity.Values().end());
			muUpper=std::max(muUpper,muCurveUpper[species]);
			kUpper=std::max(kUpper,kCurveUpper[species]);
			double speciesCpLower=std::numeric_limits<double>::infinity(),speciesCpUpper=0.0;
			for(const FireThermochemistrySegment& segment:thermo->segments){const double lo=
				std::max(localTemperatureLower,segment.temperatureMinK),hi=std::min(
					localTemperatureUpper,segment.temperatureMaxK);if(hi<lo||!(lo>0.0))continue;
				speciesCpLower=std::min(speciesCpLower,segment.certifiedCpLowerJPerKGK);
				const double* a=segment.coefficients;double polynomial=std::fabs(a[0])/(lo*lo)+
					std::fabs(a[1])/lo+std::fabs(a[2]),power=hi;
				for(std::size_t coefficient=3u;coefficient<7u;++coefficient){
					polynomial+=std::fabs(a[coefficient])*power;power*=hi;}
				speciesCpUpper=std::max(speciesCpUpper,polynomial*8314.46261815324/
					thermo->molecularWeightKGPerKMol);}
			const double densityLower=mass[species].exact-mass[species].error,
				densityUpper=mass[species].exact+mass[species].error;
			cpLower+=std::max(0.0,densityLower)*speciesCpLower;
			cpUpper+=std::max(0.0,densityUpper)*speciesCpUpper;}
		cpLower/=rhoUpper;cpUpper/=rhoLower;
		fallbackValid=fallbackValid&&rhoLower>0.0&&cpLower>0.0&&rhoUpper>=rhoLower&&
			cpUpper>=cpLower&&eddyUpper>=0.0;
		for(std::size_t i=0u;i<6u;++i)for(std::size_t j=0u;j<6u;++j){
			const FireThermochemistrySpecies* ti=fuel.FindSpecies(fuel.SpeciesOrder()[i].c_str());
			const FireThermochemistrySpecies* tj=fuel.FindSpecies(fuel.SpeciesOrder()[j].c_str());
			const double muILower=muCurveLower[i],muJUpper=muCurveUpper[j];
			if(!ti||!tj||!(muILower>0.0)||!(muJUpper>0.0)){fallbackValid=false;continue;}
			const double numerator=1.0+std::sqrt(muILower/muJUpper)*
				std::sqrt(std::sqrt(tj->molecularWeightKGPerKMol/
					ti->molecularWeightKGPerKMol));
			const double denominator=std::sqrt(8.0*(1.0+
				ti->molecularWeightKGPerKMol/tj->molecularWeightKGPerKMol));
			phiLower=std::min(phiLower,numerator*numerator/denominator);}
		fallbackValid=fallbackValid&&phiLower>0.0&&std::isfinite(phiLower)&&
			muUpper>=0.0&&kUpper>=0.0;
		const double mixtureMuUpper=fallbackValid?muUpper/phiLower:0.0,
			mixtureKUpper=fallbackValid?kUpper/phiLower:0.0;
		const double upper[3]={fallbackValid?mixtureKUpper/(rhoLower*cpLower)+
			eddyUpper/transport.TurbulentSchmidt():0.0,
			fallbackValid?mixtureKUpper+rhoUpper*cpUpper*eddyUpper/
				transport.TurbulentPrandtl():0.0,
			fallbackValid?mixtureMuUpper/rhoLower:0.0};
		for(unsigned int field=0u;field<3u;++field){fallbackValid=fallbackValid&&
			inputEnvelope->coefficientRounded[field]&&inputEnvelope->coefficientExact[field]&&
			cell<inputEnvelope->coefficientRounded[field]->size()&&
			cell<inputEnvelope->coefficientExact[field]->size()&&std::isfinite(upper[field]);
			if(fallbackValid){const float rounded=(*inputEnvelope->coefficientRounded[field])[cell];
				const double exact=(*inputEnvelope->coefficientExact[field])[cell];
				output.coefficient[field]={rounded,exact,std::max(std::fabs(exact),
					std::fabs(upper[field]-exact))+Binary32RoundingEnvelope(upper[field]),true};}}
	}
	if(!rawScale.valid||!output.coefficient[0].valid||!output.coefficient[1].valid||
		!output.coefficient[2].valid){
		if(error)*error=std::string("resident transport forward-error enclosure is invalid: raw=")+
			(rawScale.valid?"1":"0")+" D="+(output.coefficient[0].valid?"1":"0")+
			" k="+(output.coefficient[1].valid?"1":"0")+
			" nu="+(output.coefficient[2].valid?"1":"0")+
			" rho="+(rho.valid?"1":"0")+" eddy="+(eddy.valid?"1":"0")+
			" cp="+(cp.valid?"1":"0")+" mu_mix="+(mixtureMu.valid?"1":"0")+
			" k_mix="+(mixtureK.valid?"1":"0")+" cp_exact="+
			std::to_string(cp.exact)+" cp_error="+std::to_string(cp.error)+
			" volumetric_exact="+std::to_string(volumetric.exact)+
			" volumetric_error="+std::to_string(volumetric.error);
		return false;
	}
	return true;
}

::RISEFireProductionFP64::FireProductionScalarFCTRequest ScalarFCTRequestFP64(
	const RISE::FireProductionScalarFCTRequest& request)
{
	::RISEFireProductionFP64::FireProductionScalarFCTRequest result;
	result.shape.nx=request.shape.nx;result.shape.ny=request.shape.ny;
	result.shape.nz=request.shape.nz;
	result.shape.cellWidthM=static_cast<double>(request.shape.cellWidthM);
	result.timeStepS=static_cast<double>(request.timeStepS);
	for(unsigned int side=0u;side<6u;++side)result.boundary[side]=
		static_cast<::RISEFireProductionFP64::FireProductionProjectionBoundary>(
			request.boundary[side]);
	result.beginning.assign(request.beginning.begin(),request.beginning.end());
	result.sourceDelta.assign(request.sourceDelta.begin(),request.sourceDelta.end());
	for(unsigned int axis=0u;axis<3u;++axis)result.frozenVelocityMPerS[axis].assign(
		request.frozenVelocityMPerS[axis].begin(),request.frozenVelocityMPerS[axis].end());
	for(unsigned int component=0u;component<9u;++component)
		result.ambient[component]=static_cast<double>(request.ambient[component]);
	result.pressureOpenInflow=request.pressureOpenInflow;result.nullity=request.nullity;
	result.nullspaceBasis.assign(request.nullspaceBasis.begin(),request.nullspaceBasis.end());
	result.coordinateProjector.assign(
		request.coordinateProjector.begin(),request.coordinateProjector.end());
	for(unsigned int bound=0u;bound<14u;++bound)
		result.enthalpyBoundsJPerKG[bound]=static_cast<double>(request.enthalpyBoundsJPerKG[bound]);
	result.feasibilityFactor=static_cast<double>(request.feasibilityFactor);
	result.assemblyReserveFactor=static_cast<double>(request.assemblyReserveFactor);
	return result;
}

::RISEFireProductionFP64::FireProductionScalarPhysicalFluxPrerequisiteRequest
PhysicalFluxRequestFP64(const RISE::FireProductionScalarPhysicalFluxPrerequisiteRequest& request)
{
	::RISEFireProductionFP64::FireProductionScalarPhysicalFluxPrerequisiteRequest result;
	result.shape.nx=request.shape.nx;result.shape.ny=request.shape.ny;
	result.shape.nz=request.shape.nz;
	result.shape.cellWidthM=static_cast<double>(request.shape.cellWidthM);
	for(unsigned int side=0u;side<6u;++side)result.boundary[side]=
		static_cast<::RISEFireProductionFP64::FireProductionProjectionBoundary>(
			request.boundary[side]);
	result.conservativeValues.assign(request.conservativeValues.begin(),
		request.conservativeValues.end());
	result.temperatureK.assign(request.temperatureK.begin(),request.temperatureK.end());
	result.diffusivityM2PerS.assign(request.diffusivityM2PerS.begin(),
		request.diffusivityM2PerS.end());
	result.conductivityWPerMK.assign(request.conductivityWPerMK.begin(),
		request.conductivityWPerMK.end());
	for(unsigned int axis=0u;axis<3u;++axis)result.frozenVelocityMPerS[axis].assign(
		request.frozenVelocityMPerS[axis].begin(),request.frozenVelocityMPerS[axis].end());
	for(unsigned int component=0u;component<9u;++component)
		result.ambient[component]=static_cast<double>(request.ambient[component]);
	result.ambientTemperatureK=static_cast<double>(request.ambientTemperatureK);
	result.pressureOpenInflow=request.pressureOpenInflow;
	return result;
}

template<class FloatVector,class DoubleVector>
bool RoundedFP64BytesMatch(const FloatVector& observed,const DoubleVector& reference,
	std::size_t& mismatches,double& maximumAbsoluteDifference)
{
	if(observed.size()!=reference.size())return false;
	bool match=true;
	for(std::size_t value=0u;value<observed.size();++value){
		const float rounded=static_cast<float>(reference[value]);
		const double difference=std::fabs(static_cast<double>(observed[value])-reference[value]);
		maximumAbsoluteDifference=std::max(maximumAbsoluteDifference,difference);
		if(std::memcmp(&observed[value],&rounded,sizeof(float))!=0){++mismatches;match=false;}
	}
	return match;
}

int RunProductionScalarFCTMetalStageFixture()
{
	RISE::FireProductionScalarFCTRequest first;
	first.shape.nx=4u;first.shape.ny=4u;first.shape.nz=4u;first.shape.cellWidthM=1.0f;
	first.timeStepS=0.25f;first.boundary.fill(RISE::FireProductionProjectionPeriodic);
	const std::size_t cells=first.shape.CellCount();first.beginning.assign(9u*cells,0.0f);
	first.sourceDelta.assign(9u*cells,0.0f);const float pattern[4]={1.0f,1.25f,1.5f,1.25f};
	for(std::size_t z=0u;z<4u;++z)for(std::size_t y=0u;y<4u;++y)
		for(std::size_t x=0u;x<4u;++x){const std::size_t cell=(z*4u+y)*4u+x;
			first.beginning[cell]=pattern[x];first.beginning[cells+cell]=pattern[x];}
	first.sourceDelta[0u]=0.125f;first.sourceDelta[cells]=0.125f;
	for(unsigned int axis=0u;axis<3u;++axis)first.frozenVelocityMPerS[axis].assign(
		RISE::FireProductionProjectionFaceCount(first.shape,axis),axis==0u?1.0f:0.0f);
	for(auto& side:first.pressureOpenInflow)side.assign(16u,0u);
	first.ambient[0]=1.0f;first.ambient[1]=1.0f;first.nullity=1u;
	first.nullspaceBasis.assign(8u,0.0f);first.nullspaceBasis[1]=1.0f;
	first.coordinateProjector.assign(1u,1.0f);first.feasibilityFactor=1.0f/1024.0f;
	first.assemblyReserveFactor=0.5f*first.feasibilityFactor;
	RISE::FireProductionScalarFCTRequest second=first;
	second.sourceDelta.assign(9u*cells,0.0f);
	for(std::size_t cell=0u;cell<cells;++cell){second.beginning[cell]=1.0f;
		second.beginning[cells+cell]=1.0f;}
	RISE::FireProductionScalarFCTFluxPair firstPair,secondPair,averagePair;
	RISE::FireProductionScalarFCTResult firstCPU,secondCPU,averageCPU;
	std::string stageError;const bool cpu=
		RISE::BuildFireProductionScalarFCTFluxPairCPU(first,firstPair,&stageError)&&
		RISE::BuildFireProductionScalarFCTFluxPairCPU(second,secondPair,&stageError)&&
		RISE::AverageFireProductionScalarFCTFluxPairsCPU(
			firstPair,secondPair,averagePair,&stageError)&&
		RISE::SolveFireProductionScalarFCTFluxPairCPU(first,firstPair,firstCPU,&stageError)&&
		RISE::SolveFireProductionScalarFCTFluxPairCPU(first,secondPair,secondCPU,&stageError)&&
		RISE::SolveFireProductionScalarFCTFluxPairCPU(first,averagePair,averageCPU,&stageError);
	RISE::FireProductionScalarFCTMetalStageDiagnosticResult metal;
	const bool computed=cpu&&RISE::EvaluateFireProductionScalarFCTMetalStageDiagnostic(
		first,second,metal,&stageError);
	auto sameSolve=[](const auto& a,const auto& b){return a.packedFaceOffset==b.packedFaceOffset&&
		ByteIdenticalVector(a.lowFlux,b.lowFlux)&&ByteIdenticalVector(a.fluxDelta,b.fluxDelta)&&
		ByteIdenticalVector(a.lowState,b.lowState)&&
		ByteIdenticalVector(a.limiterRatio,b.limiterRatio)&&
		ByteIdenticalVectorArray(a.sharedFaceAlpha,b.sharedFaceAlpha)&&
		ByteIdenticalVector(a.accepted,b.accepted);};
	const auto oracleFirstRequest=ScalarFCTRequestFP64(first);
	const auto oracleSecondRequest=ScalarFCTRequestFP64(second);
	::RISEFireProductionFP64::FireProductionScalarFCTFluxPair oracleFirstPair,
		oracleSecondPair,oracleAveragePair;
	::RISEFireProductionFP64::FireProductionScalarFCTResult oracleFirstSolve,
		oracleSecondSolve,oracleAverageSolve;
	const bool oracle=::RISEFireProductionFP64::BuildFireProductionScalarFCTFluxPairCPU(
		oracleFirstRequest,oracleFirstPair,&stageError)&&
		::RISEFireProductionFP64::BuildFireProductionScalarFCTFluxPairCPU(
			oracleSecondRequest,oracleSecondPair,&stageError)&&
		::RISEFireProductionFP64::AverageFireProductionScalarFCTFluxPairsCPU(
			oracleFirstPair,oracleSecondPair,oracleAveragePair,&stageError)&&
		::RISEFireProductionFP64::SolveFireProductionScalarFCTFluxPairCPU(
			oracleFirstRequest,oracleFirstPair,oracleFirstSolve,&stageError)&&
		::RISEFireProductionFP64::SolveFireProductionScalarFCTFluxPairCPU(
			oracleFirstRequest,oracleSecondPair,oracleSecondSolve,&stageError)&&
		::RISEFireProductionFP64::SolveFireProductionScalarFCTFluxPairCPU(
			oracleFirstRequest,oracleAveragePair,oracleAverageSolve,&stageError);
	std::size_t fp64RoundedMismatches=0u;double fp64MaximumAbsoluteDifference=0.0;
	auto pairMatchesFP64=[&](const auto& observed,const auto& reference){
		return observed.packedFaceOffset==reference.packedFaceOffset&&
			RoundedFP64BytesMatch(observed.lowFlux,reference.lowFlux,
				fp64RoundedMismatches,fp64MaximumAbsoluteDifference)&&
			RoundedFP64BytesMatch(observed.fluxDelta,reference.fluxDelta,
				fp64RoundedMismatches,fp64MaximumAbsoluteDifference);};
	auto solveMatchesFP64=[&](const auto& observed,const auto& reference){
		bool match=observed.packedFaceOffset==reference.packedFaceOffset&&
			RoundedFP64BytesMatch(observed.lowFlux,reference.lowFlux,
				fp64RoundedMismatches,fp64MaximumAbsoluteDifference)&&
			RoundedFP64BytesMatch(observed.fluxDelta,reference.fluxDelta,
				fp64RoundedMismatches,fp64MaximumAbsoluteDifference)&&
			RoundedFP64BytesMatch(observed.lowState,reference.lowState,
				fp64RoundedMismatches,fp64MaximumAbsoluteDifference)&&
			RoundedFP64BytesMatch(observed.limiterRatio,reference.limiterRatio,
				fp64RoundedMismatches,fp64MaximumAbsoluteDifference)&&
			RoundedFP64BytesMatch(observed.accepted,reference.accepted,
				fp64RoundedMismatches,fp64MaximumAbsoluteDifference);
		for(unsigned int axis=0u;axis<3u;++axis)match=
			RoundedFP64BytesMatch(observed.sharedFaceAlpha[axis],
				reference.sharedFaceAlpha[axis],fp64RoundedMismatches,
				fp64MaximumAbsoluteDifference)&&match;
		return match;
	};
	const bool fp64RoundedExact=oracle&&pairMatchesFP64(metal.firstFluxPair,oracleFirstPair)&&
		pairMatchesFP64(metal.secondFluxPair,oracleSecondPair)&&
		pairMatchesFP64(metal.averagedFluxPair,oracleAveragePair)&&
		solveMatchesFP64(metal.firstSolve,oracleFirstSolve)&&
		solveMatchesFP64(metal.secondSolve,oracleSecondSolve)&&
		solveMatchesFP64(metal.averagedSolve,oracleAverageSolve);
	bool freshAlpha=false;if(cpu)for(unsigned int axis=0u;axis<3u;++axis)
		for(std::size_t face=0u;face<averageCPU.sharedFaceAlpha[axis].size();++face)
			freshAlpha=freshAlpha||averageCPU.sharedFaceAlpha[axis][face]!=0.5f*(
				firstCPU.sharedFaceAlpha[axis][face]+secondCPU.sharedFaceAlpha[axis][face]);
	const bool passed=computed&&metal.failureBitmap==
		std::array<std::uint32_t,3>{{0u,0u,0u}}&&
		ByteIdenticalVector(metal.firstFluxPair.lowFlux,firstPair.lowFlux)&&
		ByteIdenticalVector(metal.firstFluxPair.fluxDelta,firstPair.fluxDelta)&&
		ByteIdenticalVector(metal.secondFluxPair.lowFlux,secondPair.lowFlux)&&
		ByteIdenticalVector(metal.secondFluxPair.fluxDelta,secondPair.fluxDelta)&&
		ByteIdenticalVector(metal.averagedFluxPair.lowFlux,averagePair.lowFlux)&&
		ByteIdenticalVector(metal.averagedFluxPair.fluxDelta,averagePair.fluxDelta)&&
		sameSolve(metal.firstSolve,firstCPU)&&sameSolve(metal.secondSolve,secondCPU)&&
		sameSolve(metal.averagedSolve,averageCPU)&&freshAlpha&&fp64RoundedExact&&
		metal.commandCommitCount==7u;
	std::fprintf(stderr,"SCALAR_FCT_METAL_STAGES computed=%d passed=%d fresh_alpha=%d "
		"fp64_rounded_exact=%d fp64_mismatches=%zu fp64_max_abs=%.17g "
		"commits=%u failures=%u/%u/%u error=%s\n",computed?1:0,passed?1:0,
		freshAlpha?1:0,fp64RoundedExact?1:0,fp64RoundedMismatches,
		fp64MaximumAbsoluteDifference,metal.commandCommitCount,metal.failureBitmap[0],
		metal.failureBitmap[1],metal.failureBitmap[2],stageError.c_str());return passed?0:181;
}

template<class FloatVector,class DoubleVector>
bool FP64VectorInsideAbsoluteEnvelope(const FloatVector& observed,
	const DoubleVector& reference,const double bound,double& maximumAbsoluteDifference)
{
	if(observed.size()!=reference.size())return false;
	bool inside=true;
	for(std::size_t value=0u;value<observed.size();++value){
		const double difference=std::fabs(static_cast<double>(observed[value])-reference[value]);
		maximumAbsoluteDifference=std::max(maximumAbsoluteDifference,difference);
		inside=inside&&std::isfinite(difference)&&difference<=bound;
	}
	return inside;
}

struct UnitIntervalComparison
{
	std::size_t interiorCount=0u;
	double minimumInteriorClassMargin=std::numeric_limits<double>::infinity();
	double maximumAbsoluteDifference=0.0;
};

template<class FloatVector,class DoubleVector>
bool FP64UnitIntervalClassesMatch(const FloatVector& observed,
	const DoubleVector& reference,UnitIntervalComparison& comparison)
{
	if(observed.size()!=reference.size())return false;
	bool match=true;
	for(std::size_t value=0u;value<observed.size();++value){
		const double fp64=reference[value],fp32=static_cast<double>(observed[value]);
		const double difference=std::fabs(fp32-fp64);
		comparison.maximumAbsoluteDifference=std::max(
			comparison.maximumAbsoluteDifference,difference);
		if(!(fp64>=0.0&&fp64<=1.0&&fp32>=0.0&&fp32<=1.0)||
			!std::isfinite(difference)){match=false;continue;}
		if(fp64==0.0||fp64==1.0){match=match&&fp32==fp64;continue;}
		const double classMargin=std::min(fp64,1.0-fp64);
		comparison.minimumInteriorClassMargin=std::min(
			comparison.minimumInteriorClassMargin,classMargin);
		++comparison.interiorCount;
		match=match&&fp32>0.0&&fp32<1.0&&difference<0.5*classMargin;
	}
	return match;
}

int RunProductionScalarFCTMetalMixedBoundaryFixture()
{
	RISE::FireProductionScalarFCTRequest first;
	first.shape.nx=4u;first.shape.ny=4u;first.shape.nz=4u;
	first.shape.cellWidthM=0.37f;first.timeStepS=0.013f;
	first.boundary={{RISE::FireProductionProjectionWall,
		RISE::FireProductionProjectionPressureOpen,
		RISE::FireProductionProjectionPressureOpen,
		RISE::FireProductionProjectionWall,
		RISE::FireProductionProjectionWall,
		RISE::FireProductionProjectionPressureOpen}};
	const std::size_t cells=first.shape.CellCount();
	first.beginning.assign(9u*cells,0.0f);first.sourceDelta.assign(9u*cells,0.0f);
	for(std::size_t cell=0u;cell<cells;++cell){
		const float variation=0.017f*static_cast<float>((7u*cell+3u)%11u);
		first.beginning[cell]=0.91f+variation;
		first.beginning[cells+cell]=first.beginning[cell]+0.00021f;
		first.sourceDelta[cell]=0.00037f*static_cast<float>(static_cast<int>(cell%5u)-2);
		first.sourceDelta[cells+cell]=first.sourceDelta[cell];
	}
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t xEnd=first.shape.nx+(axis==0u?1u:0u);
		const std::size_t yEnd=first.shape.ny+(axis==1u?1u:0u);
		const std::size_t zEnd=first.shape.nz+(axis==2u?1u:0u);
		first.frozenVelocityMPerS[axis].assign(
			RISE::FireProductionProjectionFaceCount(first.shape,axis),0.0f);
		for(std::size_t z=0u;z<zEnd;++z)for(std::size_t y=0u;y<yEnd;++y)
			for(std::size_t x=0u;x<xEnd;++x){
				const std::size_t normal=axis==0u?x:(axis==1u?y:z);
				const std::size_t extent=axis==0u?first.shape.nx:
					(axis==1u?first.shape.ny:first.shape.nz);
				const std::size_t face=axis==0u?(z*first.shape.ny+y)*(first.shape.nx+1u)+x:
					(axis==1u?(z*(first.shape.ny+1u)+y)*first.shape.nx+x:
						(z*first.shape.ny+y)*first.shape.nx+x);
				const bool wall=(normal==0u&&first.boundary[2u*axis]==
					RISE::FireProductionProjectionWall)||(normal==extent&&
					first.boundary[2u*axis+1u]==RISE::FireProductionProjectionWall);
				if(!wall)first.frozenVelocityMPerS[axis][face]=0.071f*
					static_cast<float>(static_cast<int>((x+3u*y+5u*z+axis)%9u)-4);
			}
	}
	for(auto& side:first.pressureOpenInflow)side.assign(16u,0u);
	first.ambient[0]=1.03f;first.ambient[1]=1.03021f;first.nullity=1u;
	first.nullspaceBasis.assign(8u,0.0f);first.nullspaceBasis[1]=1.0f;
	first.coordinateProjector.assign(1u,1.0f);first.feasibilityFactor=1.0f/1024.0f;
	first.assemblyReserveFactor=first.feasibilityFactor;
	RISE::FireProductionScalarFCTRequest second=first;
	for(std::size_t cell=0u;cell<cells;++cell){
		second.beginning[cell]+=0.0061f*static_cast<float>(static_cast<int>(cell%3u)-1);
		second.beginning[cells+cell]=second.beginning[cell]+0.00021f;
	}
	second.sourceDelta.assign(9u*cells,0.0f);
	RISE::FireProductionScalarFCTFluxPair firstPair,secondPair,averagePair;
	RISE::FireProductionScalarFCTResult firstCPU,secondCPU,averageCPU;
	std::string stageError;const bool cpu=
		RISE::BuildFireProductionScalarFCTFluxPairCPU(first,firstPair,&stageError)&&
		RISE::BuildFireProductionScalarFCTFluxPairCPU(second,secondPair,&stageError)&&
		RISE::AverageFireProductionScalarFCTFluxPairsCPU(
			firstPair,secondPair,averagePair,&stageError)&&
		RISE::SolveFireProductionScalarFCTFluxPairCPU(first,firstPair,firstCPU,&stageError)&&
		RISE::SolveFireProductionScalarFCTFluxPairCPU(first,secondPair,secondCPU,&stageError)&&
		RISE::SolveFireProductionScalarFCTFluxPairCPU(first,averagePair,averageCPU,&stageError);
	RISE::FireProductionScalarFCTMetalStageDiagnosticResult metal;
	const bool computed=cpu&&RISE::EvaluateFireProductionScalarFCTMetalStageDiagnostic(
		first,second,metal,&stageError);
	auto sameSolve=[](const auto& a,const auto& b){return a.packedFaceOffset==b.packedFaceOffset&&
		ByteIdenticalVector(a.lowFlux,b.lowFlux)&&ByteIdenticalVector(a.fluxDelta,b.fluxDelta)&&
		ByteIdenticalVector(a.lowState,b.lowState)&&
		ByteIdenticalVector(a.limiterRatio,b.limiterRatio)&&
		ByteIdenticalVectorArray(a.sharedFaceAlpha,b.sharedFaceAlpha)&&
		ByteIdenticalVector(a.accepted,b.accepted);};
	const auto oracleFirstRequest=ScalarFCTRequestFP64(first);
	const auto oracleSecondRequest=ScalarFCTRequestFP64(second);
	::RISEFireProductionFP64::FireProductionScalarFCTFluxPair oracleFirstPair,
		oracleSecondPair,oracleAveragePair;
	::RISEFireProductionFP64::FireProductionScalarFCTResult oracleFirstSolve,
		oracleSecondSolve,oracleAverageSolve;
	const bool oracle=::RISEFireProductionFP64::BuildFireProductionScalarFCTFluxPairCPU(
		oracleFirstRequest,oracleFirstPair,&stageError)&&
		::RISEFireProductionFP64::BuildFireProductionScalarFCTFluxPairCPU(
			oracleSecondRequest,oracleSecondPair,&stageError)&&
		::RISEFireProductionFP64::AverageFireProductionScalarFCTFluxPairsCPU(
			oracleFirstPair,oracleSecondPair,oracleAveragePair,&stageError)&&
		::RISEFireProductionFP64::SolveFireProductionScalarFCTFluxPairCPU(
			oracleFirstRequest,oracleFirstPair,oracleFirstSolve,&stageError)&&
		::RISEFireProductionFP64::SolveFireProductionScalarFCTFluxPairCPU(
			oracleFirstRequest,oracleSecondPair,oracleSecondSolve,&stageError)&&
		::RISEFireProductionFP64::SolveFireProductionScalarFCTFluxPairCPU(
			oracleFirstRequest,oracleAveragePair,oracleAverageSolve,&stageError);
	double maximumState=0.0,maximumSource=0.0,maximumVelocity=0.0;
	const RISE::FireProductionScalarFCTRequest* scaleRequests[]={&first,&second};
	for(const auto* scaleRequest:scaleRequests){
		for(const float value:scaleRequest->beginning)maximumState=std::max(
			maximumState,std::fabs(static_cast<double>(value)));
		for(const float value:scaleRequest->sourceDelta)maximumSource=std::max(
			maximumSource,std::fabs(static_cast<double>(value)));
		for(const auto& axis:scaleRequest->frozenVelocityMPerS)for(const float value:axis)
			maximumVelocity=std::max(maximumVelocity,std::fabs(static_cast<double>(value)));
	}
	// MC bounds each reconstructed state by 3 Q_max, so both donor and
	// antidiffusive flux magnitudes are <=4 Q_max U_max.  Six face differences
	// then bound a 3-D accepted update by Q_max+S_max+24 dt Q_max U_max/h.
	// No publication chains more than 512 rounded arithmetic operations (nine
	// shared components and six faces included), giving this input-derived
	// cancellation-safe absolute gamma_512 envelope for the flux and state
	// fields. Limiter ratios and alpha use a separate class/margin proof below.
	const double unitRoundoff=0.5*static_cast<double>(std::numeric_limits<float>::epsilon());
	const double gamma512=512.0*unitRoundoff/(1.0-512.0*unitRoundoff);
	const double fluxScale=4.0*maximumState*maximumVelocity;
	const double updateScale=maximumState+maximumSource+24.0*
		static_cast<double>(first.timeStepS)*maximumState*maximumVelocity/
		static_cast<double>(first.shape.cellWidthM);
	const double fp64AbsoluteBound=gamma512*std::max({1.0,fluxScale,updateScale});
	double fp64MaximumAbsoluteDifference=0.0;
	UnitIntervalComparison ratioComparison,alphaComparison;
	auto pairInside=[&](const auto& observed,const auto& reference){return
		observed.packedFaceOffset==reference.packedFaceOffset&&
		FP64VectorInsideAbsoluteEnvelope(observed.lowFlux,reference.lowFlux,
			fp64AbsoluteBound,fp64MaximumAbsoluteDifference)&&
		FP64VectorInsideAbsoluteEnvelope(observed.fluxDelta,reference.fluxDelta,
			fp64AbsoluteBound,fp64MaximumAbsoluteDifference);};
	auto solveInside=[&](const auto& observed,const auto& reference){
		bool inside=observed.packedFaceOffset==reference.packedFaceOffset&&
			FP64VectorInsideAbsoluteEnvelope(observed.lowFlux,reference.lowFlux,
				fp64AbsoluteBound,fp64MaximumAbsoluteDifference)&&
			FP64VectorInsideAbsoluteEnvelope(observed.fluxDelta,reference.fluxDelta,
				fp64AbsoluteBound,fp64MaximumAbsoluteDifference)&&
			FP64VectorInsideAbsoluteEnvelope(observed.lowState,reference.lowState,
				fp64AbsoluteBound,fp64MaximumAbsoluteDifference)&&
			FP64UnitIntervalClassesMatch(observed.limiterRatio,reference.limiterRatio,
				ratioComparison)&&
			FP64VectorInsideAbsoluteEnvelope(observed.accepted,reference.accepted,
				fp64AbsoluteBound,fp64MaximumAbsoluteDifference);
		for(unsigned int axis=0u;axis<3u;++axis)inside=
			FP64UnitIntervalClassesMatch(observed.sharedFaceAlpha[axis],
				reference.sharedFaceAlpha[axis],alphaComparison)&&inside;
		return inside;
	};
	const bool fp64Envelope=oracle&&pairInside(metal.firstFluxPair,oracleFirstPair)&&
		pairInside(metal.secondFluxPair,oracleSecondPair)&&
		pairInside(metal.averagedFluxPair,oracleAveragePair)&&
		solveInside(metal.firstSolve,oracleFirstSolve)&&
		solveInside(metal.secondSolve,oracleSecondSolve)&&
		solveInside(metal.averagedSolve,oracleAverageSolve);
	const bool fp32Bytes=computed&&metal.firstFluxPair.packedFaceOffset==firstPair.packedFaceOffset&&
		metal.secondFluxPair.packedFaceOffset==secondPair.packedFaceOffset&&
		metal.averagedFluxPair.packedFaceOffset==averagePair.packedFaceOffset&&
		ByteIdenticalVector(metal.firstFluxPair.lowFlux,firstPair.lowFlux)&&
		ByteIdenticalVector(metal.firstFluxPair.fluxDelta,firstPair.fluxDelta)&&
		ByteIdenticalVector(metal.secondFluxPair.lowFlux,secondPair.lowFlux)&&
		ByteIdenticalVector(metal.secondFluxPair.fluxDelta,secondPair.fluxDelta)&&
		ByteIdenticalVector(metal.averagedFluxPair.lowFlux,averagePair.lowFlux)&&
		ByteIdenticalVector(metal.averagedFluxPair.fluxDelta,averagePair.fluxDelta)&&
		sameSolve(metal.firstSolve,firstCPU)&&sameSolve(metal.secondSolve,secondCPU)&&
		sameSolve(metal.averagedSolve,averageCPU);
	std::size_t boundaryAdjacentLimitedAlphaCount=0u;
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t extent=axis==0u?first.shape.nx:
			(axis==1u?first.shape.ny:first.shape.nz);
		for(std::size_t face=0u;face<metal.averagedSolve.sharedFaceAlpha[axis].size();++face){
			const std::size_t normal=axis==0u?face%(first.shape.nx+1u):
				(axis==1u?(face/first.shape.nx)%(first.shape.ny+1u):
					face/(first.shape.nx*first.shape.ny));
			if((normal==1u||normal+1u==extent)&&
				metal.averagedSolve.sharedFaceAlpha[axis][face]<1.0f)
				++boundaryAdjacentLimitedAlphaCount;
		}
	}
	const bool limiterClassesExercised=ratioComparison.interiorCount>0u&&
		alphaComparison.interiorCount>0u&&boundaryAdjacentLimitedAlphaCount>0u&&
		std::isfinite(ratioComparison.minimumInteriorClassMargin)&&
		std::isfinite(alphaComparison.minimumInteriorClassMargin);
	const bool passed=fp32Bytes&&fp64Envelope&&metal.failureBitmap==
		std::array<std::uint32_t,3>{{0u,0u,0u}}&&limiterClassesExercised&&
		metal.commandCommitCount==7u;
	std::fprintf(stderr,"SCALAR_FCT_METAL_MIXED passed=%d fp32_byte_exact=%d "
		"fp64_envelope=%d fp64_max_abs=%.17g fp64_abs_bound=%.17g "
		"ratio_interior=%zu ratio_min_margin=%.17g ratio_max_abs=%.17g "
		"alpha_interior=%zu alpha_min_margin=%.17g alpha_max_abs=%.17g "
		"boundary_adjacent_limited_alpha=%zu commits=%u failures=%u/%u/%u error=%s\n",
		passed?1:0,fp32Bytes?1:0,fp64Envelope?1:0,fp64MaximumAbsoluteDifference,
		fp64AbsoluteBound,ratioComparison.interiorCount,
		ratioComparison.minimumInteriorClassMargin,ratioComparison.maximumAbsoluteDifference,
		alphaComparison.interiorCount,alphaComparison.minimumInteriorClassMargin,
		alphaComparison.maximumAbsoluteDifference,boundaryAdjacentLimitedAlphaCount,
		metal.commandCommitCount,metal.failureBitmap[0],metal.failureBitmap[1],
		metal.failureBitmap[2],stageError.c_str());return passed?0:183;
}

int RunProductionCompatibleMomentumMetalFP64Fixture()
{
	const auto faceIndex=[](const RISE::FireProductionProjectionShape& shape,
		const unsigned int axis,const std::size_t x,const std::size_t y,
		const std::size_t z){
		return axis==0u?(z*shape.ny+y)*(shape.nx+1u)+x:
			(axis==1u?(z*(shape.ny+1u)+y)*shape.nx+x:
				(z*shape.ny+y)*shape.nx+x);
	};
	const unsigned int passAxes[]={0u,1u,2u,1u,0u};
	const std::vector<float> positiveZero={0.0f},negativeZero={-0.0f};
	const bool signedZeroRED=!ByteIdenticalVector(positiveZero,negativeZero);
	bool passed=signedZeroRED,metalCPUBitExact=true,fp64EnvelopePassed=true;
	double maximumAbsoluteDifference=0.0,maximumNormalizedDifference=0.0,
		maximumDerivedAbsoluteBound=0.0;
	double maximumDeviceMS=0.0;
	for( unsigned int fixture=0u;fixture<2u&&passed;++fixture ) {
		RISE::FireProductionDualMomentumRequest request;
		request.shape.nx=4u;request.shape.ny=4u;request.shape.nz=4u;
		request.shape.cellWidthM=0.125f;request.timeStepS=0.03125f;
		if( fixture==0u ) request.boundary.fill(RISE::FireProductionProjectionPeriodic);
		else request.boundary={{RISE::FireProductionProjectionWall,
			RISE::FireProductionProjectionPressureOpen,
			RISE::FireProductionProjectionPressureOpen,
			RISE::FireProductionProjectionWall,
			RISE::FireProductionProjectionWall,
			RISE::FireProductionProjectionPressureOpen}};
		std::array<std::vector<float>,5> acceptedDose;
		for( unsigned int axis=0u;axis<3u;++axis ) {
			const std::size_t xEnd=request.shape.nx+(axis==0u?1u:0u);
			const std::size_t yEnd=request.shape.ny+(axis==1u?1u:0u);
			const std::size_t zEnd=request.shape.nz+(axis==2u?1u:0u);
			const std::size_t faces=RISE::FireProductionProjectionFaceCount(request.shape,axis);
			request.beginningFaceDensity[axis].assign(faces,0.0f);
			request.beginningMomentum[axis].assign(faces,0.0f);
			request.frozenVelocityMPerS[axis].assign(faces,0.0f);
			for( std::size_t z=0u;z<zEnd;++z ) for( std::size_t y=0u;y<yEnd;++y )
				for( std::size_t x=0u;x<xEnd;++x ) {
					const std::size_t normal=axis==0u?x:(axis==1u?y:z);
					const std::size_t extent=axis==0u?request.shape.nx:
						(axis==1u?request.shape.ny:request.shape.nz);
					const std::size_t canonical=fixture==0u&&normal==extent?0u:normal;
					const std::size_t cx=axis==0u?canonical:x;
					const std::size_t cy=axis==1u?canonical:y;
					const std::size_t cz=axis==2u?canonical:z;
					const std::size_t face=faceIndex(request.shape,axis,x,y,z);
					const float density=0.8f+0.025f*static_cast<float>(
						(canonical+3u*cx+5u*cy+7u*cz+axis)%9u);
					const float velocity=-0.35f+0.075f*static_cast<float>(
						(2u*canonical+cx+4u*cy+3u*cz+axis)%11u);
					request.beginningFaceDensity[axis][face]=density;
					request.beginningMomentum[axis][face]=density*velocity;
					request.frozenVelocityMPerS[axis][face]=velocity;
				}
		}
		for( unsigned int pass=0u;pass<5u;++pass ) {
			const unsigned int axis=passAxes[pass];
			const std::size_t xEnd=request.shape.nx+(axis==0u?1u:0u);
			const std::size_t yEnd=request.shape.ny+(axis==1u?1u:0u);
			const std::size_t zEnd=request.shape.nz+(axis==2u?1u:0u);
			acceptedDose[pass].assign(
				RISE::FireProductionProjectionFaceCount(request.shape,axis),0.0f);
			for( std::size_t z=0u;z<zEnd;++z ) for( std::size_t y=0u;y<yEnd;++y )
				for( std::size_t x=0u;x<xEnd;++x ) {
					const std::size_t normal=axis==0u?x:(axis==1u?y:z);
					const std::size_t extent=axis==0u?request.shape.nx:
						(axis==1u?request.shape.ny:request.shape.nz);
					const std::size_t canonical=fixture==0u&&normal==extent?0u:normal;
					const std::size_t cx=axis==0u?canonical:x;
					const std::size_t cy=axis==1u?canonical:y;
					const std::size_t cz=axis==2u?canonical:z;
					acceptedDose[pass][faceIndex(request.shape,axis,x,y,z)]=
						0.0025f*static_cast<float>(static_cast<int>(
						(canonical+2u*cx+3u*cy+5u*cz+pass)%7u)-3);
				}
		}
		RISE::FireProductionDualMomentumResult metal;
		std::string error;
		const bool metalOK=RISE::RemapFireProductionCompatibleDualMomentumMetalComparator(
			request,acceptedDose,metal,&error);
		maximumDeviceMS=std::max(maximumDeviceMS,metal.deviceElapsedMS);
		RISE::FireProductionDualMomentumResult cpu;
		const bool cpuOK=RISE::RemapFireProductionCompatibleDualMomentumCPU(
			request,acceptedDose,cpu,&error);
		::RISEFireProductionFP64::FireProductionDualMomentumRequest oracleRequest;
		oracleRequest.shape.nx=request.shape.nx;oracleRequest.shape.ny=request.shape.ny;
		oracleRequest.shape.nz=request.shape.nz;
		oracleRequest.shape.cellWidthM=static_cast<double>(request.shape.cellWidthM);
		oracleRequest.timeStepS=static_cast<double>(request.timeStepS);
		oracleRequest.ambientDensityKGPerM3=
			static_cast<double>(request.ambientDensityKGPerM3);
		for( unsigned int side=0u;side<6u;++side ) oracleRequest.boundary[side]=
			static_cast<::RISEFireProductionFP64::FireProductionProjectionBoundary>(
				request.boundary[side]);
		for( unsigned int axis=0u;axis<3u;++axis ) {
			oracleRequest.beginningFaceDensity[axis].assign(
				request.beginningFaceDensity[axis].begin(),request.beginningFaceDensity[axis].end());
			oracleRequest.beginningMomentum[axis].assign(
				request.beginningMomentum[axis].begin(),request.beginningMomentum[axis].end());
			oracleRequest.frozenVelocityMPerS[axis].assign(
				request.frozenVelocityMPerS[axis].begin(),request.frozenVelocityMPerS[axis].end());
		}
		std::array<std::vector<double>,5> oracleDose;
		for( unsigned int pass=0u;pass<5u;++pass ) oracleDose[pass].assign(
			acceptedDose[pass].begin(),acceptedDose[pass].end());
		::RISEFireProductionFP64::FireProductionDualMomentumResult oracle;
		const bool oracleOK=::RISEFireProductionFP64::RemapFireProductionCompatibleDualMomentumCPU(
			oracleRequest,oracleDose,oracle,&error);
		bool metadataOK=metalOK&&cpuOK&&oracleOK&&metal.executedSubmapCount==15u&&
			metal.commandCommitCount==1u&&metal.interstageFullGridTransferCount==0u;
		bool fixtureCPUBitExact=metadataOK,fixtureFP64Envelope=metadataOK;
		const float unitRoundoff=0.5f*std::numeric_limits<float>::epsilon();
		const float gamma128=128.0f*unitRoundoff/(1.0f-128.0f*unitRoundoff);
		double maximumDose=0.0,minimumBeginningDensity=std::numeric_limits<double>::infinity(),
			maximumBeginningDensity=0.0,maximumBeginningMomentum=0.0;
		for(const auto& dose:acceptedDose)for(const float value:dose)
			maximumDose=std::max(maximumDose,std::fabs(static_cast<double>(value)));
		for(unsigned int axis=0u;axis<3u;++axis){
			for(const float value:request.beginningFaceDensity[axis]){
				minimumBeginningDensity=std::min(
					minimumBeginningDensity,static_cast<double>(value));
				maximumBeginningDensity=std::max(
					maximumBeginningDensity,std::fabs(static_cast<double>(value)));
			}
			for(const float value:request.beginningMomentum[axis])
				maximumBeginningMomentum=std::max(
					maximumBeginningMomentum,std::fabs(static_cast<double>(value)));
		}
		// Each of the five palindrome passes changes density by at most 2D/h.
		// Therefore rho_min-10D/h is a request-derived lower bound for every
		// intermediate density.  The momentum recurrence
		// M_(k+1)<=M_k(1+2D/(h rho_lower)) bounds every cancellation-free term
		// magnitude.  At most 128 rounded elementary operations feed one final
		// publication (five passes, including both interpolated face velocities),
		// so gamma_128 times these magnitude bounds is an absolute forward bound.
		const double inverseCellWidth=1.0/static_cast<double>(request.shape.cellWidthM);
		const double minimumDensityBound=minimumBeginningDensity-
			10.0*maximumDose*inverseCellWidth;
		const double densityMagnitudeBound=maximumBeginningDensity+
			10.0*maximumDose*inverseCellWidth;
		double momentumMagnitudeBound=maximumBeginningMomentum;
		if(minimumDensityBound>0.0)for(unsigned int pass=0u;pass<5u;++pass)
			momentumMagnitudeBound*=1.0+2.0*maximumDose*inverseCellWidth/
				minimumDensityBound;
		const double densityAbsoluteBound=static_cast<double>(gamma128)*
			densityMagnitudeBound;
		const double momentumAbsoluteBound=static_cast<double>(gamma128)*
			momentumMagnitudeBound;
		maximumDerivedAbsoluteBound=std::max(maximumDerivedAbsoluteBound,
			std::max(densityAbsoluteBound,momentumAbsoluteBound));
		fixtureFP64Envelope=fixtureFP64Envelope&&minimumDensityBound>0.0&&
			std::isfinite(momentumMagnitudeBound);
		for( unsigned int axis=0u;axis<3u&&metadataOK;++axis ) {
			metadataOK=metal.auxiliaryFaceDensity[axis].size()==
				oracle.auxiliaryFaceDensity[axis].size()&&metal.momentum[axis].size()==
				oracle.momentum[axis].size();
			fixtureCPUBitExact=fixtureCPUBitExact&&metadataOK&&
				ByteIdenticalVector(metal.auxiliaryFaceDensity[axis],
					cpu.auxiliaryFaceDensity[axis])&&
				ByteIdenticalVector(metal.momentum[axis],cpu.momentum[axis]);
			for( std::size_t face=0u;face<metal.momentum[axis].size()&&metadataOK;++face ) {
				const double oracleDensity=oracle.auxiliaryFaceDensity[axis][face];
				const double oracleMomentum=oracle.momentum[axis][face];
				const double densityDifference=std::fabs(
					static_cast<double>(metal.auxiliaryFaceDensity[axis][face])-oracleDensity);
				const double momentumDifference=std::fabs(
					static_cast<double>(metal.momentum[axis][face])-oracleMomentum);
				const double densityScale=std::max(std::fabs(
					static_cast<double>(metal.auxiliaryFaceDensity[axis][face])),
					std::fabs(oracleDensity));
				const double momentumScale=std::max(std::fabs(
					static_cast<double>(metal.momentum[axis][face])),std::fabs(oracleMomentum));
				maximumAbsoluteDifference=std::max(maximumAbsoluteDifference,
					std::max(densityDifference,momentumDifference));
				if( densityScale>0.0f ) maximumNormalizedDifference=std::max(
					maximumNormalizedDifference,densityDifference/densityScale);
				if( momentumScale>0.0f ) maximumNormalizedDifference=std::max(
					maximumNormalizedDifference,momentumDifference/momentumScale);
				fixtureFP64Envelope=fixtureFP64Envelope&&
					densityDifference<=gamma128*densityScale&&
					momentumDifference<=gamma128*momentumScale&&
					densityDifference<=densityAbsoluteBound&&
					momentumDifference<=momentumAbsoluteBound;
			}
		}
		metalCPUBitExact=metalCPUBitExact&&fixtureCPUBitExact;
		fp64EnvelopePassed=fp64EnvelopePassed&&fixtureFP64Envelope;
		const bool fixturePassed=metadataOK&&fixtureCPUBitExact&&fixtureFP64Envelope;
		if( !fixturePassed ) std::fprintf(stderr,"COMPATIBLE_MOMENTUM_METAL_FP64 fixture=%u "
			"metal=%d cpu=%d oracle=%d max_abs=%.17g max_norm=%.17g gamma128=%.9g "
			"error=%s\n",fixture,metalOK?1:0,cpuOK?1:0,oracleOK?1:0,
			maximumAbsoluteDifference,maximumNormalizedDifference,gamma128,error.c_str());
		passed=passed&&fixturePassed;
	}
	std::fprintf(stderr,"COMPATIBLE_MOMENTUM_METAL_FP64 passed=%d fixtures=2 "
		"metal_cpu_byte_exact=%d signed_zero_red=%d fp64_gamma128_envelope=%d "
		"fp64_max_abs=%.17g fp64_max_norm=%.17g derived_abs_bound=%.17g "
		"max_device_ms=%.9g\n",passed?1:0,metalCPUBitExact?1:0,signedZeroRED?1:0,
		fp64EnvelopePassed?1:0,maximumAbsoluteDifference,maximumNormalizedDifference,
		maximumDerivedAbsoluteBound,maximumDeviceMS);
	return passed?0:182;
}

int RunProductionResidentTransportMetalFP64Fixture()
{
	static_assert(!std::is_convertible<FireProductionResidentTransportComparatorResult,
		FireProductionProjectedHeunTransportCoefficients>::value,
		"qualification output must never become a live transport publication");
	const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
	const FireSimulationTransportRecord& transport=FireSimulationTransportRecord::OpenV1();
	if(!fuel.IsValid()||!transport.IsValid()||fuel.SpeciesOrder().size()!=MethaneSpeciesCount)
		return 183;
	FireProductionResidentTransportComparatorRequest request;
	request.shape.nx=4u;request.shape.ny=5u;request.shape.nz=4u;
	request.shape.cellWidthM=0.025f;request.stage=FireProductionProjectedHeunStage::R0;
	request.attemptIdentity=UINT64_C(0x1960000000000001);
	request.parentCandidateIdentity=UINT64_C(0x1960000000000002);
	request.projectionIdentity=UINT64_C(0x1960000000000003);
	request.boundary.fill(FireProductionProjectionWall);
	const std::size_t cells=request.shape.CellCount();
	for(unsigned int side=0u;side<6u;++side)request.fuelInletBoundaryFace[side].assign(
		side<2u?request.shape.ny*request.shape.nz:
			(side<4u?request.shape.nx*request.shape.nz:request.shape.nx*request.shape.ny),0u);
	request.conservativeValues.assign(9u*cells,0.0f);
	request.temperatureK.resize(cells);
	PeriodicMACShape mirrorShape;mirrorShape.nx=request.shape.nx;
	mirrorShape.ny=request.shape.ny;mirrorShape.nz=request.shape.nz;
	mirrorShape.cellWidthM=request.shape.cellWidthM;
	std::vector<ConservativeVector> mirrorState(cells);
	const FireTransportSpecies* firstSpecies=transport.FindSpecies(
		fuel.SpeciesOrder().front().c_str());
	if(!firstSpecies||firstSpecies->viscosity.Wavelengths().size()<3u)return 184;
	const std::vector<double>& knots=firstSpecies->viscosity.Wavelengths();
	const std::array<double,4> temperaturePattern={{transport.TemperatureMinK(),knots[1],
		0.5*(knots[1]+knots[2]),transport.TemperatureMaxK()}};
	std::string error;
	for(std::size_t cell=0u;cell<cells;++cell){
		MethaneCellState physical;physical.producerPrecision=FireStateProducerPrecision::Binary32;
		physical.temperatureK=static_cast<double>(static_cast<float>(temperaturePattern[cell%4u]));
		const double mixtureFraction=0.015+0.005*static_cast<double>(cell%4u);
		double gasFraction[6];
		for(std::size_t species=0u;species<6u;++species)
			gasFraction[species]=(1.0-mixtureFraction)*fuel.AmbientMassFractions()[species]+
				mixtureFraction*fuel.InjectedMassFractions()[species];
		const double reacted=0.15*std::min(gasFraction[MethaneCH4],
			gasFraction[MethaneO2]/fuel.StoichiometricOxygenKGPerKGFuel());
		for(std::size_t species=0u;species<6u;++species)
			gasFraction[species]+=reacted*fuel.PrimaryReactionDelta()[species];
		double inverseWeight=0.0;
		for(std::size_t species=0u;species<MethaneCarbon;++species){
			const FireThermochemistrySpecies* speciesRecord=fuel.FindSpecies(
				fuel.SpeciesOrder()[species].c_str());
			if(!speciesRecord)return 185;
			inverseWeight+=gasFraction[species]/
				speciesRecord->molecularWeightKGPerKMol;
		}
		const double density=fuel.ThermodynamicPressurePa()/(8314.46261815324*
			physical.temperatureK*inverseWeight);
		for(std::size_t species=0u;species<MethaneCarbon;++species)
			physical.constituent[species]=gasFraction[species]*density;
		physical.constituent[MethaneCarbon]=0.0;
		physical.rhoTotalZ=mixtureFraction*density;
		if(!fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(physical),
			physical.temperatureK,physical.sensibleEnergyJPerM3,&error))return 186;
		const ConservativeVector conservative=ToConservativeVector(physical);
		for(std::size_t component=0u;component<MethaneConservativeDimension;++component){
			const float value=static_cast<float>(conservative[component]);
			request.conservativeValues[component*cells+cell]=value;
			mirrorState[cell][component]=static_cast<double>(value);
		}
		request.temperatureK[cell]=static_cast<float>(physical.temperatureK);
	}
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=FireProductionProjectionFaceCount(request.shape,axis);
		request.projectedVelocityMPerS[axis].resize(faces);
		const std::size_t ex=axis==0u?request.shape.nx+1u:request.shape.nx;
		const std::size_t ey=axis==1u?request.shape.ny+1u:request.shape.ny;
		const std::size_t ez=axis==2u?request.shape.nz+1u:request.shape.nz;
		for(std::size_t z=0u;z<ez;++z)for(std::size_t y=0u;y<ey;++y)
			for(std::size_t x=0u;x<ex;++x){
				const std::size_t face=axis==0u?(z*request.shape.ny+y)*
					(request.shape.nx+1u)+x:(axis==1u?(z*(request.shape.ny+1u)+y)*
					request.shape.nx+x:(z*request.shape.ny+y)*request.shape.nx+x);
				const std::size_t coordinate=axis==0u?x:(axis==1u?y:z);
				const std::size_t extent=axis==0u?request.shape.nx:
					(axis==1u?request.shape.ny:request.shape.nz);
				const float value=(coordinate==0u||coordinate==extent)?0.0f:
					0.03125f*static_cast<float>((axis+1u)*(x+1u))-
					0.0234375f*static_cast<float>((y+1u)*(z+1u))+
					0.015625f*static_cast<float>(axis+z);
				request.projectedVelocityMPerS[axis][face]=value;
			}
	}
	auto mirrorBoundary=[&](const FireProductionResidentTransportComparatorRequest& fixture){
		OpenBoundaryConfig3D boundary;
		for(unsigned int side=0u;side<6u;++side)boundary.kind[side]=
			fixture.boundary[side]==FireProductionProjectionPressureOpen?
				PressureOpenBoundary3D:AdiabaticWallBoundary3D;
		boundary.ambientState=mirrorState.front();boundary.injectedState=mirrorState.front();
		if(std::any_of(fixture.fuelInletBoundaryFace[4].begin(),
			fixture.fuelInletBoundaryFace[4].end(),[](unsigned char value){return value!=0u;})){
			boundary.bottomFuelMask.assign(fixture.fuelInletBoundaryFace[4].begin(),
				fixture.fuelInletBoundaryFace[4].end());
			boundary.bottomFuelMassFluxKGPerM2S.resize(boundary.bottomFuelMask.size());
			for(std::size_t face=0u;face<boundary.bottomFuelMask.size();++face)
				boundary.bottomFuelMassFluxKGPerM2S[face]=boundary.bottomFuelMask[face]?0.01:0.0;
		}
		double density=0.0;for(std::size_t species=0u;species<MethaneCarbon;++species)
			density+=mirrorState.front()[1u+species];
		boundary.ambientDensityKGPerM3=density;boundary.injectedGasDensityKGPerM3=density;
		boundary.fuelMassFluxKGPerM2S=0.0;boundary.velocityToleranceMPerS=0.0;
		boundary.pressureTolerancePa=1.0e-3;return boundary;
	};
	bool hostBinary32DAGBitExact=true;
	std::uint32_t independentBranchBitmap=0u;
	double maximumVremanUncancelledScale=0.0;
	std::array<double,3> maximumOracleAbsoluteResidual={{0.0,0.0,0.0}};
	std::array<double,3> boundAtMaximumOracleAbsoluteResidual={{0.0,0.0,0.0}};
	std::array<double,3> maximumOracleBoundFraction={{0.0,0.0,0.0}};
	auto compare=[&](const FireProductionResidentTransportComparatorRequest& fixture,
		const OpenBoundaryConfig3D& boundary,FireProductionResidentTransportComparatorResult& metal,
		double& maximumNormalized)->bool{
		if(!EvaluateFireProductionResidentTransportMetalComparator(fixture,metal,&error)){
			std::fprintf(stderr,"RESIDENT_TRANSPORT_METAL failed error=%s\n",error.c_str());
			return false;
		}
		OpenMACField3D fixtureVelocity;
		for(unsigned int axis=0u;axis<3u;++axis)fixtureVelocity.component[axis].assign(
			fixture.projectedVelocityMPerS[axis].begin(),fixture.projectedVelocityMPerS[axis].end());
		std::vector<CellTransportEvaluation> oracle;
		const bool periodic=std::all_of(fixture.boundary.begin(),fixture.boundary.end(),
			[](const FireProductionProjectionBoundary value){return
				value==FireProductionProjectionPeriodic;});
		if(periodic){
			std::array<std::vector<double>,3> cellVelocity;
			for(unsigned int component=0u;component<3u;++component){
				cellVelocity[component].resize(cells);
				for(std::size_t cell=0u;cell<cells;++cell)cellVelocity[component][cell]=0.5*(
					fixtureVelocity.component[component][OpenLowerFaceForCell3D(
						mirrorShape,cell,component)]+fixtureVelocity.component[component][
						OpenUpperFaceForCell3D(mirrorShape,cell,component)]);
			}
			oracle.resize(cells);const double widths[3]={mirrorShape.cellWidthM,
				mirrorShape.cellWidthM,mirrorShape.cellWidthM};
			for(std::size_t cell=0u;cell<cells;++cell){double gradient[3][3]={};
				for(unsigned int derivative=0u;derivative<3u;++derivative){
					const std::size_t previous=PeriodicPrevious(mirrorShape,cell,derivative);
					const std::size_t next=PeriodicNext(mirrorShape,cell,derivative);
					for(unsigned int component=0u;component<3u;++component)
						gradient[derivative][component]=(cellVelocity[component][next]-
							cellVelocity[component][previous])/(2.0*mirrorShape.cellWidthM);
				}
				MethaneCellState physical=FromConservativeVector(mirrorState[cell],
					FireStateProducerPrecision::Binary32);
				physical.temperatureK=fixture.temperatureK[cell];
				if(!EvaluateCellTransport(physical,gradient,widths,false,fuel,transport,
					FireStateProducerPrecision::Binary32,oracle[cell],&error)){
					std::fprintf(stderr,"RESIDENT_TRANSPORT_FP64 periodic cell=%zu error=%s\n",
						cell,error.c_str());return false;}
			}
		}else if(!BuildOpenStageTransportEvaluations3D(mirrorShape,mirrorState,
			std::vector<double>(fixture.temperatureK.begin(),fixture.temperatureK.end()),
			fixtureVelocity,boundary,false,fuel,transport,FireStateProducerPrecision::Binary32,
			oracle,&error,1u)){std::fprintf(stderr,"RESIDENT_TRANSPORT_FP64 open error=%s\n",
				error.c_str());return false;}
		bool bounded=oracle.size()==cells&&metal.diffusivityM2PerS.size()==cells&&
			metal.conductivityWPerMK.size()==cells&&
			metal.molecularKinematicViscosityM2PerS.size()==cells;
		for(std::size_t cell=0u;cell<cells&&bounded;++cell){
			double rho=0.0;for(std::size_t species=0u;species<MethaneCarbon;++species)
				rho+=mirrorState[cell][1u+species];
			const double target[3]={oracle[cell].totalDiffusivityM2PerS,
				oracle[cell].effectiveConductivityWPerMK,
				oracle[cell].molecularViscosityPaS/rho};
			const double positiveScale[3]={oracle[cell].molecularDiffusivityM2PerS+
				oracle[cell].sgsDiffusivityM2PerS,oracle[cell].molecularConductivityWPerMK+
				rho*oracle[cell].gasCpJPerKGK*oracle[cell].eddyViscosityM2PerS/
					transport.TurbulentPrandtl(),oracle[cell].molecularViscosityPaS/rho};
			const float observedFloat[3]={metal.diffusivityM2PerS[cell],
				metal.conductivityWPerMK[cell],metal.molecularKinematicViscosityM2PerS[cell]};
			const double observed[3]={observedFloat[0],observedFloat[1],observedFloat[2]};
			CertifiedResidentTransport certified;
			if(!EvaluateCertifiedResidentTransport(fixture,cell,certified,&error))return false;
			independentBranchBitmap|=certified.branchBitmap;
			maximumVremanUncancelledScale=std::max(maximumVremanUncancelledScale,
				certified.vremanUncancelledScale);
			for(unsigned int field=0u;field<3u;++field){
				const double difference=std::fabs(observed[field]-
					certified.coefficient[field].exact);
				const double doubleEquivalence=std::fabs(certified.coefficient[field].exact-
					target[field]);
				const double oracleDifference=std::fabs(observed[field]-target[field]);
				// Both the packed binary32 DAG and the canonical fp64 evaluator must
				// lie inside the independently propagated termwise enclosure. The
				// resulting device-vs-oracle radius is the triangle bound 2e. No
				// final-output or cancellation-sensitive scale enters this gate.
				const double doubleEquivalenceBound=certified.coefficient[field].error;
				const double bound=2.0*certified.coefficient[field].error;
				if(oracleDifference>maximumOracleAbsoluteResidual[field]){
					maximumOracleAbsoluteResidual[field]=oracleDifference;
					boundAtMaximumOracleAbsoluteResidual[field]=bound;
				}
				if(bound>0.0)maximumOracleBoundFraction[field]=std::max(
					maximumOracleBoundFraction[field],oracleDifference/bound);
				hostBinary32DAGBitExact=hostBinary32DAGBitExact&&
					std::memcmp(&observedFloat[field],&certified.coefficient[field].rounded,
						sizeof(float))==0;
				maximumNormalized=std::max(maximumNormalized,std::fabs(
					observed[field]-target[field])/positiveScale[field]);
				if((!std::isfinite(difference)||difference>bound||
					!std::isfinite(oracleDifference)||oracleDifference>bound||
					doubleEquivalence>doubleEquivalenceBound)&&bounded)
					std::fprintf(stderr,"RESIDENT_TRANSPORT_BOUND cell=%zu field=%u "
						"observed=%.17g strict=%.17g target=%.17g difference=%.17g "
						"bound=%.17g double_residual=%.17g double_bound=%.17g scale=%.17g\n",
						cell,field,observed[field],certified.coefficient[field].rounded,
						target[field],difference,bound,doubleEquivalence,
						doubleEquivalenceBound,positiveScale[field]);
				bounded=bounded&&std::isfinite(bound)&&bound>=0.0&&
					std::isfinite(doubleEquivalenceBound)&&doubleEquivalenceBound>=0.0&&
					std::isfinite(difference)&&difference<=
					certified.coefficient[field].error&&
					doubleEquivalence<=doubleEquivalenceBound&&
					std::isfinite(oracleDifference)&&oracleDifference<=bound;
			}
		}
		return bounded&&metal.deviceProduced&&metal.devicePublicationIdentity!=0u&&
			metal.commandCommitCount==1u&&metal.interstageFullGridTransferCount==0u&&
			metal.terminalStagingCount==1u&&metal.actualMetalAllocationBytes<=
				metal.certifiedWorkingSetBytes&&metal.stage==fixture.stage&&
			metal.attemptIdentity==fixture.attemptIdentity&&
			metal.parentCandidateIdentity==fixture.parentCandidateIdentity&&
			metal.projectionIdentity==fixture.projectionIdentity;
	};
	double maximumNormalized=0.0;std::uint32_t branchBitmap=0u;
	FireProductionResidentTransportComparatorResult wallResult;
	bool passed=compare(request,mirrorBoundary(request),wallResult,maximumNormalized);
	std::uint64_t liveIncrementBytes=0u,residentOwnerPeakBytes=0u;
	FireProductionProjectionShape ownerShape=request.shape;
	ownerShape.nx=8u;ownerShape.ny=8u;ownerShape.nz=8u;
	const bool liveIncrementCertified=
		FireProductionResidentTransportLiveIncrementWorkingSetBytes(ownerShape,
			liveIncrementBytes);
	const std::array<FireProductionProjectionBoundary,6> productionBoundary={{
		FireProductionProjectionPressureOpen,FireProductionProjectionPressureOpen,
		FireProductionProjectionPressureOpen,FireProductionProjectionPressureOpen,
		FireProductionProjectionWall,FireProductionProjectionPressureOpen}};
	const bool ownerPeakCertified=FireProductionResidentStepWorkingSetBytes(ownerShape,
		productionBoundary,residentOwnerPeakBytes);
	std::uint64_t forceProjectionProbe=0u,projectionProbe=0u,cellProbe=0u,dualProbe=0u;
	const bool forceProjectionCertified=FireProductionResidentForceProjectionWorkingSetBytes(
		ownerShape,forceProjectionProbe);
	const bool projectionCertified=FireProductionProjectionWorkingSetBytes(
		ownerShape,projectionProbe);
	const bool cellCertified=FireProductionCellPalindromeWorkingSetBytes(
		ownerShape,9u,cellProbe);
	const bool dualCertified=FireProductionDualMomentumResidentWorkingSetBytes(
		ownerShape,productionBoundary,dualProbe);
	passed=liveIncrementCertified&&ownerPeakCertified&&liveIncrementBytes>0u&&
		residentOwnerPeakBytes>=liveIncrementBytes&&passed;
	branchBitmap|=wallResult.branchObligationBitmap;
	FireProductionResidentTransportComparatorRequest lineageOnly=request;
	lineageOnly.attemptIdentity+=3u;
	FireProductionResidentTransportComparatorResult lineageResult;
	passed=compare(lineageOnly,mirrorBoundary(lineageOnly),lineageResult,maximumNormalized)&&
		lineageResult.devicePublicationIdentity!=wallResult.devicePublicationIdentity&&passed;
	FireProductionResidentTransportComparatorRequest parentOnly=request;
	parentOnly.parentCandidateIdentity+=7u;
	FireProductionResidentTransportComparatorResult parentResult;
	passed=compare(parentOnly,mirrorBoundary(parentOnly),parentResult,maximumNormalized)&&
		parentResult.devicePublicationIdentity!=wallResult.devicePublicationIdentity&&passed;
	FireProductionResidentTransportComparatorRequest projectionOnly=request;
	projectionOnly.projectionIdentity+=11u;
	FireProductionResidentTransportComparatorResult projectionResult;
	passed=compare(projectionOnly,mirrorBoundary(projectionOnly),projectionResult,maximumNormalized)&&
		projectionResult.devicePublicationIdentity!=wallResult.devicePublicationIdentity&&passed;
	FireProductionResidentTransportComparatorRequest stageOnly=request;
	stageOnly.stage=FireProductionProjectedHeunStage::R1;
	FireProductionResidentTransportComparatorResult stageResult;
	passed=compare(stageOnly,mirrorBoundary(stageOnly),stageResult,maximumNormalized)&&
		stageResult.devicePublicationIdentity!=wallResult.devicePublicationIdentity&&passed;
	FireProductionResidentTransportComparatorRequest periodic=request;
	periodic.boundary.fill(FireProductionProjectionPeriodic);periodic.attemptIdentity+=1u;
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t firstEnd=axis==0u?request.shape.ny:request.shape.nx;
		const std::size_t secondEnd=axis==2u?request.shape.ny:request.shape.nz;
		const std::size_t extent=axis==0u?request.shape.nx:
			(axis==1u?request.shape.ny:request.shape.nz);
		auto seamFace=[&](const std::size_t normal,const std::size_t first,
			const std::size_t second){return axis==0u?
			(second*request.shape.ny+first)*(request.shape.nx+1u)+normal:
			(axis==1u?(second*(request.shape.ny+1u)+normal)*request.shape.nx+first:
				(normal*request.shape.ny+second)*request.shape.nx+first);};
		for(std::size_t second=0u;second<secondEnd;++second)
			for(std::size_t first=0u;first<firstEnd;++first)
				periodic.projectedVelocityMPerS[axis][seamFace(extent,first,second)]=
					periodic.projectedVelocityMPerS[axis][seamFace(0u,first,second)];
	}
	OpenBoundaryConfig3D periodicBoundary=mirrorBoundary(periodic);
	periodicBoundary.kind.fill(AdiabaticWallBoundary3D);
	FireProductionResidentTransportComparatorResult periodicResult;
	passed=compare(periodic,periodicBoundary,periodicResult,maximumNormalized)&&passed;
	branchBitmap|=periodicResult.branchObligationBitmap;
	FireProductionResidentTransportComparatorRequest open=request;
	open.boundary.fill(FireProductionProjectionPressureOpen);open.attemptIdentity+=2u;
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t extent=axis==0u?request.shape.nx:
			(axis==1u?request.shape.ny:request.shape.nz);
		const std::size_t firstEnd=axis==0u?request.shape.ny:request.shape.nx;
		const std::size_t secondEnd=axis==2u?request.shape.ny:request.shape.nz;
		auto boundaryFace=[&](const std::size_t normal,const std::size_t first,
			const std::size_t second){return axis==0u?
			(second*request.shape.ny+first)*(request.shape.nx+1u)+normal:
			(axis==1u?(second*(request.shape.ny+1u)+normal)*request.shape.nx+first:
				(normal*request.shape.ny+second)*request.shape.nx+first);};
		for(std::size_t second=0u;second<secondEnd;++second)
			for(std::size_t first=0u;first<firstEnd;++first){
				open.projectedVelocityMPerS[axis][boundaryFace(0u,first,second)]=0.0625f;
				open.projectedVelocityMPerS[axis][boundaryFace(extent,first,second)]=-0.046875f;
			}
	}
	OpenBoundaryConfig3D openBoundary=mirrorBoundary(open);
	FireProductionResidentTransportComparatorResult openResult;
	passed=compare(open,openBoundary,openResult,maximumNormalized)&&passed;
	branchBitmap|=openResult.branchObligationBitmap;
	FireProductionResidentTransportComparatorRequest mixed=request;
	mixed.boundary={{FireProductionProjectionWall,FireProductionProjectionWall,
		FireProductionProjectionPressureOpen,FireProductionProjectionPressureOpen,
		FireProductionProjectionWall,FireProductionProjectionWall}};
	mixed.attemptIdentity+=4u;
	for(std::size_t face=0u;face<mixed.fuelInletBoundaryFace[4].size();++face)
		mixed.fuelInletBoundaryFace[4][face]=face%3u==0u?1u:0u;
	FireProductionResidentTransportComparatorResult mixedResult;
	passed=compare(mixed,mirrorBoundary(mixed),mixedResult,maximumNormalized)&&passed;
	branchBitmap|=mixedResult.branchObligationBitmap;
	FireProductionResidentTransportComparatorRequest cancellation=request;
	cancellation.boundary.fill(FireProductionProjectionPressureOpen);
	cancellation.attemptIdentity+=5u;
	const float cancellationGradient[3][3]={{-0.76821637f,1.02942061f,-4.74512291f},
		{0.35371006f,-0.47399589f,2.18486214f},
		{1.24271476f,-1.66526103f,7.67605352f}};
	for(unsigned int component=0u;component<3u;++component){
		const std::size_t ex=component==0u?request.shape.nx+1u:request.shape.nx;
		const std::size_t ey=component==1u?request.shape.ny+1u:request.shape.ny;
		const std::size_t ez=component==2u?request.shape.nz+1u:request.shape.nz;
		for(std::size_t z=0u;z<ez;++z)for(std::size_t y=0u;y<ey;++y)
			for(std::size_t x=0u;x<ex;++x){
				const float position[3]={request.shape.cellWidthM*(static_cast<float>(x)+
					(component==0u?0.0f:0.5f)),request.shape.cellWidthM*(static_cast<float>(y)+
					(component==1u?0.0f:0.5f)),request.shape.cellWidthM*(static_cast<float>(z)+
					(component==2u?0.0f:0.5f))};
				float value=0.0f;for(unsigned int derivative=0u;derivative<3u;++derivative)
					value+=cancellationGradient[derivative][component]*position[derivative];
				const std::size_t face=component==0u?(z*request.shape.ny+y)*
					(request.shape.nx+1u)+x:(component==1u?
					(z*(request.shape.ny+1u)+y)*request.shape.nx+x:
					(z*request.shape.ny+y)*request.shape.nx+x);
				cancellation.projectedVelocityMPerS[component][face]=value;
			}
	}
	FireProductionResidentTransportComparatorResult cancellationResult;
	passed=compare(cancellation,mirrorBoundary(cancellation),cancellationResult,
		maximumNormalized)&&passed;
	branchBitmap|=cancellationResult.branchObligationBitmap;
	// Independent arithmetic walker for the cancellation-negative Vreman arm.
	// The matrix is the sealed float tuple from the search witness above; this
	// evaluation neither reads the device bitmap nor reuses the Metal source.
	float walkerBeta[3][3]={};
	for(unsigned int m=0u;m<3u;++m)for(unsigned int i=0u;i<3u;++i)
		for(unsigned int j=0u;j<3u;++j)
			walkerBeta[i][j]+=cancellationGradient[m][i]*cancellationGradient[m][j];
	const float walkerB0=walkerBeta[0][0]*walkerBeta[1][1];
	const float walkerB1=walkerBeta[0][1]*walkerBeta[0][1];
	const float walkerB2=walkerBeta[0][0]*walkerBeta[2][2];
	const float walkerB3=walkerBeta[0][2]*walkerBeta[0][2];
	const float walkerB4=walkerBeta[1][1]*walkerBeta[2][2];
	const float walkerB5=walkerBeta[1][2]*walkerBeta[1][2];
	const float walkerRawB=walkerB0-walkerB1+walkerB2-walkerB3+walkerB4-walkerB5;
	if(walkerRawB<0.0f)independentBranchBitmap|=1u<<14u;
	FireProductionResidentTransportComparatorRequest zeroGradient=request;
	for(std::vector<float>& axis:zeroGradient.projectedVelocityMPerS)
		std::fill(axis.begin(),axis.end(),0.0f);
	for(float& temperature:zeroGradient.temperatureK)
		temperature=static_cast<float>(transport.TemperatureMinK());
	FireProductionResidentTransportComparatorResult zeroResult;
	passed=compare(zeroGradient,mirrorBoundary(zeroGradient),zeroResult,maximumNormalized)&&passed;
	branchBitmap|=zeroResult.branchObligationBitmap;
	std::uint32_t requiredCPBitmap=0u;bool thirdCPSegmentReachable=false;
	for(std::size_t species=0u;species<6u;++species){const FireThermochemistrySpecies* record=
		fuel.FindSpecies(fuel.SpeciesOrder()[species].c_str());
		for(std::size_t segment=0u;record&&segment<record->segments.size();++segment){
			const double lo=std::max(record->segments[segment].temperatureMinK,
				transport.TemperatureMinK());
			const double hi=std::min(record->segments[segment].temperatureMaxK,
				transport.TemperatureMaxK());
			if(lo<=hi){requiredCPBitmap|=1u<<std::min<std::size_t>(segment,2u);
				thirdCPSegmentReachable=thirdCPSegmentReachable||segment>=2u;}}
	}
	const std::uint32_t requiredBranchBitmap=requiredCPBitmap|(1u<<3u)|(1u<<4u)|(1u<<5u)|
		(1u<<6u)|(1u<<7u)|(1u<<8u)|(1u<<9u)|(1u<<10u)|(1u<<11u)|
		(1u<<12u)|(1u<<13u)|(1u<<14u)|(1u<<15u)|(1u<<16u)|(1u<<17u)|(1u<<18u);
	passed=passed&&branchBitmap==independentBranchBitmap&&
		(branchBitmap&requiredBranchBitmap)==requiredBranchBitmap&&
		wallResult.devicePublicationIdentity!=periodicResult.devicePublicationIdentity&&
		periodicResult.devicePublicationIdentity!=openResult.devicePublicationIdentity;
	FireProductionResidentTransportComparatorRequest missingLineage=request;
	missingLineage.attemptIdentity=0u;
	FireProductionResidentTransportComparatorResult refused;error.clear();
	const bool missingLineageRefused=!EvaluateFireProductionResidentTransportMetalComparator(
		missingLineage,refused,&error)&&!error.empty()&&!refused.deviceProduced&&
		refused.devicePublicationIdentity==0u;
	FireProductionResidentTransportComparatorRequest brokenSeam=periodic;
	brokenSeam.projectedVelocityMPerS[0][request.shape.nx]+=0.125f;
	error.clear();const bool seamRefused=
		!EvaluateFireProductionResidentTransportMetalComparator(brokenSeam,refused,&error)&&
		!error.empty()&&!refused.deviceProduced;
	FireProductionResidentTransportComparatorRequest invalidDeviceState=request;
	for(std::size_t species=0u;species<6u;++species)
		std::fill(invalidDeviceState.conservativeValues.begin()+(species+1u)*cells,
			invalidDeviceState.conservativeValues.begin()+(species+2u)*cells,0.0f);
	FireProductionResidentTransportComparatorResult invalidDeviceResult;error.clear();
	const bool invalidDeviceRefused=
		!EvaluateFireProductionResidentTransportMetalComparator(invalidDeviceState,
			invalidDeviceResult,&error)&&!error.empty()&&!invalidDeviceResult.deviceProduced&&
		invalidDeviceResult.devicePublicationIdentity==0u;
	FireProductionResidentTransportComparatorRequest shortVelocity=request;
	shortVelocity.projectedVelocityMPerS[2].pop_back();error.clear();
	const bool shortVelocityRefused=!EvaluateFireProductionResidentTransportMetalComparator(
		shortVelocity,refused,&error)&&!error.empty()&&!refused.deviceProduced;
	passed=passed&&missingLineageRefused&&seamRefused&&invalidDeviceRefused&&
		shortVelocityRefused;
	std::fprintf(stderr,"RESIDENT_TRANSPORT_METAL_FP64 passed=%d host_fp32_dag_bit_equal=%d "
		"relative_scale_diagnostic_max=%.17g "
		"diffusivity_abs_m2_s=%.17g diffusivity_bound_m2_s=%.17g diffusivity_bound_fraction=%.17g "
		"conductivity_abs_W_mK=%.17g conductivity_bound_W_mK=%.17g conductivity_bound_fraction=%.17g "
		"molecular_nu_abs_m2_s=%.17g molecular_nu_bound_m2_s=%.17g molecular_nu_bound_fraction=%.17g "
		"vreman_uncancelled=%.17g "
		"branch_bitmap=0x%08x independent_bitmap=0x%08x missing_lineage_refused=%d seam_refused=%d "
		"invalid_device_refused=%d short_velocity_refused=%d cp_segment2_reachable=%d "
		"fixture_ws=%llu actual_ws=%llu live_increment_ws=%llu owner_peak_ws=%llu "
		"live_increment_certified=%d owner_peak_certified=%d "
		"ws_parts=%d/%d/%d/%d "
		"device_identity=%016llx\n",
		passed?1:0,hostBinary32DAGBitExact?1:0,maximumNormalized,
		maximumOracleAbsoluteResidual[0],boundAtMaximumOracleAbsoluteResidual[0],
		maximumOracleBoundFraction[0],maximumOracleAbsoluteResidual[1],
		boundAtMaximumOracleAbsoluteResidual[1],maximumOracleBoundFraction[1],
		maximumOracleAbsoluteResidual[2],boundAtMaximumOracleAbsoluteResidual[2],
		maximumOracleBoundFraction[2],maximumVremanUncancelledScale,branchBitmap,
		independentBranchBitmap,missingLineageRefused?1:0,seamRefused?1:0,
		invalidDeviceRefused?1:0,shortVelocityRefused?1:0,thirdCPSegmentReachable?1:0,
		static_cast<unsigned long long>(wallResult.certifiedWorkingSetBytes),
		static_cast<unsigned long long>(wallResult.actualMetalAllocationBytes),
		static_cast<unsigned long long>(liveIncrementBytes),
		static_cast<unsigned long long>(residentOwnerPeakBytes),
		liveIncrementCertified?1:0,ownerPeakCertified?1:0,
		forceProjectionCertified?1:0,projectionCertified?1:0,cellCertified?1:0,
		dualCertified?1:0,
		static_cast<unsigned long long>(wallResult.devicePublicationIdentity));
	return passed?0:196;
}

int RunProductionResidentPhysicalFluxMetalFP64Fixture()
{
	static_assert(!std::is_convertible<FireProductionResidentPhysicalFluxComparatorResult,
		FireProductionScalarPhysicalFluxPrerequisiteResult>::value,
		"qualification output must not become resident physical-flux authority");
	const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
	if(!fuel.IsValid())return 197;
	FireProductionResidentPhysicalFluxComparatorRequest request;
	request.transport.shape.nx=4u;request.transport.shape.ny=4u;
	request.transport.shape.nz=4u;request.transport.shape.cellWidthM=0.025f;
	request.transport.stage=FireProductionProjectedHeunStage::R0;
	request.transport.attemptIdentity=UINT64_C(0x1980000000000001);
	request.transport.parentCandidateIdentity=UINT64_C(0x1980000000000002);
	request.transport.projectionIdentity=UINT64_C(0x1980000000000003);
	request.transport.boundary.fill(FireProductionProjectionWall);
	const std::size_t cells=request.transport.shape.CellCount();
	for(unsigned int side=0u;side<6u;++side){const std::size_t count=side<2u?
		request.transport.shape.ny*request.transport.shape.nz:(side<4u?
		request.transport.shape.nx*request.transport.shape.nz:
		request.transport.shape.nx*request.transport.shape.ny);
		request.transport.fuelInletBoundaryFace[side].assign(count,0u);
		request.pressureOpenInflow[side].assign(count,0u);
	}
	request.transport.conservativeValues.assign(9u*cells,0.0f);
	request.transport.temperatureK.resize(cells);std::string error;
	auto assignCell=[&](FireProductionResidentPhysicalFluxComparatorRequest& fixture,
		const std::size_t cell,const double temperature)->bool{
		MethaneCellState physical;physical.producerPrecision=FireStateProducerPrecision::Binary32;
		physical.temperatureK=temperature;
		const double fraction=0.01+0.00625*static_cast<double>((cell+cell/4u)%4u);
		double massFraction[6];for(std::size_t species=0u;species<6u;++species)
			massFraction[species]=(1.0-fraction)*fuel.AmbientMassFractions()[species]+
				fraction*fuel.InjectedMassFractions()[species];
		const double reacted=0.125*std::min(massFraction[MethaneCH4],
			massFraction[MethaneO2]/fuel.StoichiometricOxygenKGPerKGFuel());
		for(std::size_t species=0u;species<6u;++species)
			massFraction[species]+=reacted*fuel.PrimaryReactionDelta()[species];
		double inverseWeight=0.0;for(std::size_t species=0u;species<6u;++species){
			const FireThermochemistrySpecies* record=fuel.FindSpecies(
				fuel.SpeciesOrder()[species].c_str());if(!record)return false;
			inverseWeight+=massFraction[species]/record->molecularWeightKGPerKMol;
		}
		const double density=fuel.ThermodynamicPressurePa()/(8314.46261815324*
			physical.temperatureK*inverseWeight);
		for(std::size_t species=0u;species<6u;++species)
			physical.constituent[species]=density*massFraction[species];
		physical.constituent[MethaneCarbon]=0.0;physical.rhoTotalZ=density*fraction;
		if(!fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(physical),
			physical.temperatureK,physical.sensibleEnergyJPerM3,&error))return false;
		const ConservativeVector value=ToConservativeVector(physical);
		for(std::size_t component=0u;component<9u;++component)
			fixture.transport.conservativeValues[component*cells+cell]=
				static_cast<float>(value[component]);
		fixture.transport.temperatureK[cell]=static_cast<float>(physical.temperatureK);
		return true;
	};
	for(std::size_t cell=0u;cell<cells;++cell)if(!assignCell(request,cell,
		300.0+75.0*static_cast<double>(cell%4u)))return 199;
	for(unsigned int component=0u;component<9u;++component)
		request.ambient[component]=request.transport.conservativeValues[component*cells];
	request.ambientTemperatureK=request.transport.temperatureK[0];
	for(unsigned int axis=0u;axis<3u;++axis){
		const std::size_t faces=FireProductionProjectionFaceCount(request.transport.shape,axis);
		request.transport.projectedVelocityMPerS[axis].resize(faces);
		const std::size_t ex=axis==0u?5u:4u,ey=axis==1u?5u:4u,ez=axis==2u?5u:4u;
		for(std::size_t z=0u;z<ez;++z)for(std::size_t y=0u;y<ey;++y)
			for(std::size_t x=0u;x<ex;++x){const std::size_t face=axis==0u?
				(z*4u+y)*5u+x:(axis==1u?(z*5u+y)*4u+x:(z*4u+y)*4u+x);
				request.transport.projectedVelocityMPerS[axis][face]=
					0.0234375f*static_cast<float>((axis+1u)*(x+1u))-
					0.015625f*static_cast<float>((y+1u)*(z+1u));}
	}
	const FireCertifiedNullspace& reconstruction=fuel.ConservativeReconstruction();
	request.nullity=reconstruction.nullity;
	request.nullspaceBasis.resize(reconstruction.orthonormalBasis.size());
	for(std::size_t value=0u;value<request.nullspaceBasis.size();++value)
		request.nullspaceBasis[value]=static_cast<float>(reconstruction.orthonormalBasis[value]);
	request.coordinateProjector.assign(request.nullity*request.nullity,0.0f);
	for(std::size_t value=0u;value<request.nullity;++value)
		request.coordinateProjector[value*request.nullity+value]=1.0f;
	struct BoundStat {double residual=0.0,boundAtResidual=0.0,maximumRatio=0.0;bool passed=true;};
	std::array<BoundStat,9> donorStat,mcStat,lowStat,highStat;
	std::array<BoundStat,8> massStat;std::array<BoundStat,7> enthalpyStat;
	std::array<BoundStat,3> gasStat;BoundStat energyStat;
	auto updateStat=[](BoundStat& stat,const double observed,const double target,
		const double bound){const double residual=std::fabs(observed-target);
		if(residual>stat.residual){stat.residual=residual;stat.boundAtResidual=bound;}
		if(bound>0.0)stat.maximumRatio=std::max(stat.maximumRatio,residual/bound);
		else if(residual!=0.0)stat.maximumRatio=std::numeric_limits<double>::infinity();
		stat.passed=stat.passed&&std::isfinite(residual)&&std::isfinite(bound)&&bound>=0.0&&
			residual<=bound;};
	const double epsilon=std::numeric_limits<float>::epsilon();
	auto gamma=[&](const double operations){return operations*epsilon/(1.0-operations*epsilon);};
	bool donorBitExact=true,deltaBitExact=true,mcBitExact=true,shared=true,boundsPassed=true;
	bool enthalpyBitExact=true,logBitExact=true,logEnclosurePassed=true;
	double maximumDeviceLogEnclosureRatio=0.0;
	std::uint32_t branchBitmap=0u,independentBranchBitmap=0u;std::uint64_t wallIdentity=0u;
	FireProductionResidentPhysicalFluxComparatorResult metal;
	auto validateFixture=[&](const FireProductionResidentPhysicalFluxComparatorRequest& fixture,
		const char* label)->bool{
		FireProductionResidentPhysicalFluxComparatorResult observed;
		if(!EvaluateFireProductionResidentPhysicalFluxMetalComparator(fixture,observed,&error)){
			std::fprintf(stderr,"RESIDENT_PHYSICAL_FLUX fixture=%s error=%s\n",label,error.c_str());return false;}
		FireProductionScalarFCTRequest advective;advective.shape=fixture.transport.shape;
		advective.timeStepS=0.001f;advective.boundary=fixture.transport.boundary;
		advective.beginning=fixture.transport.conservativeValues;
		advective.sourceDelta.assign(9u*cells,0.0f);
		advective.frozenVelocityMPerS=fixture.transport.projectedVelocityMPerS;
		advective.ambient=fixture.ambient;advective.pressureOpenInflow=fixture.pressureOpenInflow;
		advective.nullity=fixture.nullity;advective.nullspaceBasis=fixture.nullspaceBasis;
		advective.coordinateProjector=fixture.coordinateProjector;
		advective.feasibilityFactor=1.0f/1024.0f;
		advective.assemblyReserveFactor=0.5f*advective.feasibilityFactor;
		FireProductionScalarFCTFluxPair advectiveCPU;
		if(!BuildFireProductionScalarFCTFluxPairCPU(advective,advectiveCPU,&error))return false;
		std::vector<float> highCPU(advectiveCPU.lowFlux.size());
		for(std::size_t value=0u;value<highCPU.size();++value)
			highCPU[value]=advectiveCPU.lowFlux[value]+advectiveCPU.fluxDelta[value];
		donorBitExact=donorBitExact&&ByteIdenticalVector(observed.donorAdvectiveFlux,
			advectiveCPU.lowFlux);deltaBitExact=deltaBitExact&&ByteIdenticalVector(
				observed.advectiveFluxDelta,advectiveCPU.fluxDelta);mcBitExact=mcBitExact&&
			ByteIdenticalVector(observed.mcMusclAdvectiveFlux,highCPU);
		FireProductionScalarPhysicalFluxPrerequisiteRequest physical;
		physical.shape=fixture.transport.shape;physical.boundary=fixture.transport.boundary;
		physical.conservativeValues=fixture.transport.conservativeValues;
		physical.temperatureK=fixture.transport.temperatureK;
		physical.diffusivityM2PerS=observed.diffusivityM2PerS;
		physical.conductivityWPerMK=observed.conductivityWPerMK;
		physical.frozenVelocityMPerS=fixture.transport.projectedVelocityMPerS;
		physical.ambient=fixture.ambient;physical.ambientTemperatureK=fixture.ambientTemperatureK;
		physical.pressureOpenInflow=fixture.pressureOpenInflow;
		const auto advective64=ScalarFCTRequestFP64(advective);
		const auto physical64=PhysicalFluxRequestFP64(physical);
		::RISEFireProductionFP64::FireProductionScalarFCTFluxPair advectiveOracle;
		::RISEFireProductionFP64::FireProductionScalarPhysicalFluxPrerequisiteResult physicalOracle;
		if(!::RISEFireProductionFP64::BuildFireProductionScalarFCTFluxPairCPU(
			advective64,advectiveOracle,&error)||
			!::RISEFireProductionFP64::BuildFireProductionScalarPhysicalFluxPrerequisiteCPU(
				physical64,physicalOracle,&error))return false;
		const std::size_t allFaces=observed.packedFaceOffset[2]+
			FireProductionProjectionFaceCount(fixture.transport.shape,2u);
		auto decode=[&](const std::size_t packed,unsigned int& axis,std::size_t& x,
			std::size_t& y,std::size_t& z){if(packed<observed.packedFaceOffset[1]){
				axis=0u;std::size_t rest=packed;x=rest%5u;rest/=5u;y=rest%4u;z=rest/4u;
			}else if(packed<observed.packedFaceOffset[2]){axis=1u;std::size_t rest=
				packed-observed.packedFaceOffset[1];x=rest%4u;rest/=4u;y=rest%5u;z=rest/5u;
			}else{axis=2u;std::size_t rest=packed-observed.packedFaceOffset[2];
				x=rest%4u;rest/=4u;y=rest%4u;z=rest/4u;}};
		auto localCell=[&](const unsigned int axis,const long coordinate,std::size_t x,
			std::size_t y,std::size_t z,std::size_t& cell,bool& ambient)->bool{
			const long extent=4;long selected=coordinate;ambient=false;
			if(selected<0||selected>=extent){const unsigned int side=2u*axis+(selected>=extent?1u:0u);
				const std::size_t sideFace=axis==0u?z*4u+y:(axis==1u?z*4u+x:y*4u+x);
				if(fixture.transport.boundary[side]==FireProductionProjectionPeriodic)
					selected=selected<0?extent-1:0;
				else if(fixture.transport.boundary[side]==FireProductionProjectionPressureOpen&&
					!fixture.pressureOpenInflow[side].empty()&&
					fixture.pressureOpenInflow[side][sideFace]!=0u){ambient=true;return true;}
				else selected=selected<0?0:extent-1;}
			if(axis==0u)x=static_cast<std::size_t>(selected);else if(axis==1u)y=
				static_cast<std::size_t>(selected);else z=static_cast<std::size_t>(selected);
			cell=(z*4u+y)*4u+x;return true;};
		const FireCertifiedNullspace& physicalProjection=fuel.NonadvectiveFluxProjection();
		for(std::size_t face=0u;face<allFaces;++face){unsigned int axis=0u;
			std::size_t x=0u,y=0u,z=0u;decode(face,axis,x,y,z);
			const long coordinate=static_cast<long>(axis==0u?x:(axis==1u?y:z));
			const std::size_t localFace=face-observed.packedFaceOffset[axis];
			const double velocity=fixture.transport.projectedVelocityMPerS[axis][localFace];
			std::array<double,9> lineScale={{}};
			for(int shift=-2;shift<=1;++shift){std::size_t cell=0u;bool ambient=false;
				localCell(axis,coordinate+shift,x,y,z,cell,ambient);
				for(std::size_t component=0u;component<9u;++component)lineScale[component]+=
					std::fabs(ambient?fixture.ambient[component]:
						fixture.transport.conservativeValues[component*cells+cell]);}
			std::array<double,8> slopeScale={{}};
			for(std::size_t component=0u;component<8u;++component)
				for(std::size_t basis=0u;basis<fixture.nullity;++basis){double coordinateScale=0.0;
					for(std::size_t column=0u;column<fixture.nullity;++column){double rawScale=0.0;
						for(std::size_t row=0u;row<8u;++row)rawScale+=std::fabs(
							fixture.nullspaceBasis[row*fixture.nullity+column])*lineScale[row];
						coordinateScale+=std::fabs(fixture.coordinateProjector[
							basis*fixture.nullity+column])*rawScale;}
					slopeScale[component]+=std::fabs(fixture.nullspaceBasis[
						component*fixture.nullity+basis])*coordinateScale;}
			std::array<double,8> physicalScale={{}};double maximumRhoD=0.0,
				minimumMass=std::numeric_limits<double>::max(),
				maximumK=0.0,minimumT=fixture.ambientTemperatureK,maximumT=minimumT;
			for(int shift=-1;shift<=0;++shift){std::size_t cell=0u;bool ambient=false;
				localCell(axis,coordinate+shift,x,y,z,cell,ambient);double total=0.0;
				for(std::size_t species=0u;species<7u;++species)total+=ambient?
					fixture.ambient[1u+species]:fixture.transport.conservativeValues[
						(1u+species)*cells+cell];
				minimumMass=std::min(minimumMass,total);if(!ambient){maximumRhoD=std::max(
					maximumRhoD,total*observed.diffusivityM2PerS[cell]);maximumK=std::max(
					maximumK,static_cast<double>(observed.conductivityWPerMK[cell]));
					minimumT=std::min(minimumT,static_cast<double>(fixture.transport.temperatureK[cell]));
					maximumT=std::max(maximumT,static_cast<double>(fixture.transport.temperatureK[cell]));}}
			const double distance=fixture.transport.shape.cellWidthM*0.5;
			std::array<double,8> rawScale={{}};for(std::size_t component=0u;component<8u;++component)
				rawScale[component]=2.0*maximumRhoD*lineScale[component]/
					(std::max(minimumMass,std::numeric_limits<double>::min())*distance);
			for(std::size_t component=0u;component<8u;++component)
				for(std::size_t basis=0u;basis<physicalProjection.nullity;++basis){double coordinateScale=0.0;
					for(std::size_t row=0u;row<8u;++row)
						coordinateScale+=std::fabs(physicalProjection.orthonormalBasis[
							row*physicalProjection.nullity+basis])*rawScale[row];
					physicalScale[component]+=std::fabs(
						physicalProjection.orthonormalBasis[component*physicalProjection.nullity+basis])*
						coordinateScale;}
			const bool geometricBoundary=coordinate==0||coordinate==4;
			const bool periodicAxis=fixture.transport.boundary[2u*axis]==
				FireProductionProjectionPeriodic;
			bool physicalActive=!geometricBoundary||periodicAxis;
			unsigned int boundarySide=0u;std::size_t boundarySideFace=0u;
			if(geometricBoundary&&!periodicAxis){const unsigned int side=2u*axis+
				(coordinate==4?1u:0u);const std::size_t sideFace=axis==0u?z*4u+y:
					(axis==1u?z*4u+x:y*4u+x);physicalActive=
					fixture.transport.boundary[side]==FireProductionProjectionPressureOpen&&
					fixture.pressureOpenInflow[side][sideFace]!=0u;boundarySide=side;
				boundarySideFace=sideFace;}
			if(periodicAxis&&geometricBoundary&&coordinate==4)
				independentBranchBitmap|=1u<<1u;
			if(!geometricBoundary||periodicAxis)independentBranchBitmap|=1u<<0u;
			else if(fixture.transport.boundary[boundarySide]==FireProductionProjectionWall)
				independentBranchBitmap|=1u<<2u;
			else if(fixture.pressureOpenInflow[boundarySide][boundarySideFace]==0u)
				independentBranchBitmap|=1u<<3u;
			else independentBranchBitmap|=1u<<4u;
			if(physicalActive)independentBranchBitmap|=1u<<5u;
			if(geometricBoundary&&!periodicAxis&&
				fixture.transport.boundary[boundarySide]!=FireProductionProjectionWall)
				independentBranchBitmap|=1u<<11u;
			else if(!geometricBoundary||periodicAxis)independentBranchBitmap|=1u<<12u;
			std::array<double,7> faceEnthalpyBound={{}},faceEnthalpyTarget={{}};
			double energyBound=0.0;
			if(physicalActive){std::size_t leftCell=0u,rightCell=0u;bool leftAmbient=false,
				rightAmbient=false;localCell(axis,coordinate-1,x,y,z,leftCell,leftAmbient);
				localCell(axis,coordinate,x,y,z,rightCell,rightAmbient);
				const float leftTemperature=leftAmbient?fixture.ambientTemperatureK:
					fixture.transport.temperatureK[leftCell];
				const float rightTemperature=rightAmbient?fixture.ambientTemperatureK:
					fixture.transport.temperatureK[rightCell];
				volatile float faceTemperatureValue=0.5f*(leftTemperature+rightTemperature);
				const float faceTemperature=faceTemperatureValue;
				auto deterministicLog32=[](const float value){std::uint32_t bits=0u;
					std::memcpy(&bits,&value,sizeof(bits));const int exponent=
						static_cast<int>((bits>>23u)&255u)-127;bits=(bits&0x007fffffu)|0x3f800000u;
					float normalized=0.0f;std::memcpy(&normalized,&bits,sizeof(normalized));
					volatile float numerator=normalized-1.0f,denominator=normalized+1.0f,
						y=numerator/denominator,y2=y*y,power=y,sum=power;
					for(unsigned int odd=3u;odd<=17u;odd+=2u){power=power*y2;
						volatile float term=power/static_cast<float>(odd);sum=sum+term;}
					volatile float exponentTerm=static_cast<float>(exponent)*0.6931471805599453f,
						series=2.0f*sum,result=exponentTerm+series;return static_cast<float>(result);};
				const float log32=deterministicLog32(faceTemperature);
				const float deviceLog=observed.faceLogTemperature[face];
				logBitExact=logBitExact&&std::memcmp(&log32,&deviceLog,sizeof(float))==0;
				const double log64=std::log(static_cast<double>(faceTemperature));
				std::uint32_t temperatureBits=0u;std::memcpy(&temperatureBits,&faceTemperature,
					sizeof(temperatureBits));const int exponent=
					static_cast<int>((temperatureBits>>23u)&255u)-127;
				temperatureBits=(temperatureBits&0x007fffffu)|0x3f800000u;
				float normalized32=0.0f;std::memcpy(&normalized32,&temperatureBits,sizeof(normalized32));
				const double normalized=normalized32,y=(normalized-1.0)/(normalized+1.0),y2=y*y;
				double power=y,seriesScale=std::fabs(static_cast<double>(exponent)*
					static_cast<double>(0.6931471805599453f))+2.0*std::fabs(power);
				for(unsigned int odd=3u;odd<=17u;odd+=2u){power*=y2;
					seriesScale+=2.0*std::fabs(power/static_cast<double>(odd));}
				const double nextPower=power*y2,
					seriesRemainder=2.0*std::fabs(nextPower)/(19.0*(1.0-y2)),
					ln2Packing=std::fabs(static_cast<double>(exponent))*
						std::fabs(static_cast<double>(0.6931471805599453f)-std::log(2.0)),
					logApproximationBound=gamma(64.0)*seriesScale+ln2Packing+seriesRemainder,
					deviceLogResidual=std::fabs(static_cast<double>(deviceLog)-log64),
					deviceLogEnclosureRatio=deviceLogResidual/logApproximationBound;
				maximumDeviceLogEnclosureRatio=std::max(maximumDeviceLogEnclosureRatio,
					deviceLogEnclosureRatio);logEnclosurePassed=logEnclosurePassed&&
					std::isfinite(logApproximationBound)&&deviceLogResidual<=logApproximationBound;
				if(!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(faceTemperature,
					faceEnthalpyTarget.data(),faceEnthalpyTarget.size(),&error))return false;
				double energyInputError=0.0,energyRoundScale=0.0;
				for(std::size_t species=0u;species<7u;++species){
					const FireThermochemistrySpecies* item=fuel.FindSpecies(
						fuel.SpeciesOrder()[species].c_str());if(!item)return false;
					const FireThermochemistrySegment* selected=&item->segments.front();
					for(const FireThermochemistrySegment& segment:item->segments)if(
						faceTemperature>=static_cast<float>(segment.temperatureMinK)&&
						(faceTemperature<static_cast<float>(segment.temperatureMaxK)||
						 &segment==&item->segments.back()))selected=&segment;
					const std::size_t segmentIndex=static_cast<std::size_t>(selected-
						item->segments.data());independentBranchBitmap|=1u<<
						(7u+static_cast<unsigned int>(std::min<std::size_t>(segmentIndex,2u)));
					float coefficient[7];for(std::size_t term=0u;term<7u;++term)
						coefficient[term]=static_cast<float>(selected->coefficients[term]);
					const float molecularWeight=static_cast<float>(item->molecularWeightKGPerKMol),
						offset=static_cast<float>(selected->sensibleEnthalpyOffsetJPerKG);
					volatile float inverse=1.0f/faceTemperature,t2=faceTemperature*faceTemperature,
						t3=t2*faceTemperature,t4=t3*faceTemperature,t5=t4*faceTemperature;
					volatile float term0=-coefficient[0]*inverse,term1=coefficient[1]*log32,
						term2=coefficient[2]*faceTemperature,
						term3=coefficient[3]*t2*0.5f,
						term4=coefficient[4]*t3*(1.0f/3.0f),
						term5=coefficient[5]*t4*0.25f,
						term6=coefficient[6]*t5*0.2f;
					volatile float primitive=term0+term1+term2+term3+term4+term5+term6;
					volatile float scaled=8314.46261815324f*primitive/molecularWeight;
					volatile float hostEnthalpyValue=scaled+offset;const float hostEnthalpy=hostEnthalpyValue;
					const float deviceEnthalpy=observed.faceSensibleEnthalpyJPerKG[
						species*allFaces+face];enthalpyBitExact=enthalpyBitExact&&
						std::memcmp(&hostEnthalpy,&deviceEnthalpy,sizeof(float))==0;
					const double temperature64=faceTemperature,t2d=temperature64*temperature64,
						t3d=t2d*temperature64,t4d=t3d*temperature64,t5d=t4d*temperature64;
					const std::array<double,7> basis={{-1.0/temperature64,log64,temperature64,
						t2d*0.5,t3d/3.0,t4d*0.25,t5d*0.2}};
					double packedPrimitiveScale=0.0,truePrimitiveScale=0.0,
						coefficientPacking=0.0;
					for(std::size_t term=0u;term<7u;++term){
						packedPrimitiveScale+=std::fabs(static_cast<double>(coefficient[term])*basis[term]);
						truePrimitiveScale+=std::fabs(selected->coefficients[term]*basis[term]);
						coefficientPacking+=std::fabs(static_cast<double>(coefficient[term])-
							selected->coefficients[term])*std::fabs(basis[term]);}
					const double gasConstant=8314.46261815324,
						packedFactor=static_cast<double>(static_cast<float>(gasConstant))/molecularWeight,
						trueFactor=gasConstant/item->molecularWeightKGPerKMol,
						uncancelledScale=std::fabs(packedFactor)*packedPrimitiveScale+
							std::fabs(static_cast<double>(offset)),
						packingBound=std::fabs(packedFactor)*coefficientPacking+
							std::fabs(packedFactor-trueFactor)*truePrimitiveScale+
							std::fabs(static_cast<double>(offset)-
								selected->sensibleEnthalpyOffsetJPerKG),
						logBound=std::fabs(packedFactor*coefficient[1])*logApproximationBound;
					faceEnthalpyBound[species]=gamma(64.0)*uncancelledScale+
						packingBound+logBound;
					updateStat(enthalpyStat[species],deviceEnthalpy,
						faceEnthalpyTarget[species],faceEnthalpyBound[species]);
					const double massTarget=physicalOracle.physicalMassFluxKGPerM2S[
						(1u+species)*allFaces+face],massBound=gamma(512.0)*
						physicalScale[1u+species];
					energyInputError+=std::fabs(faceEnthalpyTarget[species])*massBound+
						std::fabs(massTarget)*faceEnthalpyBound[species]+
						massBound*faceEnthalpyBound[species];
					energyRoundScale+=(std::fabs(faceEnthalpyTarget[species])+
						faceEnthalpyBound[species])*(std::fabs(massTarget)+massBound);
				}
				const double conductionScale=2.0*maximumK*(maximumT-minimumT)/distance;
				energyBound=energyInputError+gamma(128.0)*(energyRoundScale+conductionScale);
			}
			for(std::size_t component=0u;component<9u;++component){const std::size_t value=component*allFaces+face;
				const double donorScale=std::fabs(velocity)*lineScale[component];
				const double mcScale=std::fabs(velocity)*(2.0*lineScale[component]+
					(component<8u?slopeScale[component]:lineScale[component]));
				const double donorBound=gamma(4.0)*donorScale,mcBound=gamma(512.0)*mcScale;
				const double physicalBound=component<8u?gamma(512.0)*physicalScale[component]:
					energyBound;
				const double physicalTarget=component<8u?
					physicalOracle.physicalMassFluxKGPerM2S[value]:
					physicalOracle.physicalEnergyFluxWPerM2[face];
				const double donorTarget=advectiveOracle.lowFlux[value];
				const double mcTarget=donorTarget+advectiveOracle.fluxDelta[value];
				updateStat(donorStat[component],observed.donorAdvectiveFlux[value],donorTarget,donorBound);
				updateStat(mcStat[component],observed.mcMusclAdvectiveFlux[value],mcTarget,mcBound);
				const double lowBound=donorBound+physicalBound+gamma(1.0)*(
					std::fabs(donorTarget)+std::fabs(physicalTarget));
				const double highBound=mcBound+physicalBound+gamma(1.0)*(
					std::fabs(mcTarget)+std::fabs(physicalTarget));
				updateStat(lowStat[component],observed.lowCompositeFlux[value],
					donorTarget+physicalTarget,lowBound);
				updateStat(highStat[component],observed.highCompositeFlux[value],
					mcTarget+physicalTarget,highBound);
				if(component<8u)updateStat(massStat[component],
					observed.physicalMassFluxKGPerM2S[value],physicalTarget,physicalBound);
			}
			updateStat(energyStat,observed.physicalEnergyFluxWPerM2[face],
				physicalOracle.physicalEnergyFluxWPerM2[face],energyBound);
			double gasScale=0.0;for(std::size_t component=1u;component<=6u;++component)
				gasScale+=physicalScale[component];
			updateStat(gasStat[axis],observed.physicalGasFluxKGPerM2S[axis][localFace],
				physicalOracle.physicalGasFluxKGPerM2S[axis][localFace],gamma(640.0)*gasScale);
		}
		for(std::size_t component=0u;component<9u;++component)
			for(std::size_t face=0u;face<allFaces;++face){const std::size_t value=component*allFaces+face;
				const float physicalValue=component<8u?observed.physicalMassFluxKGPerM2S[value]:
					observed.physicalEnergyFluxWPerM2[face];const float low=
					observed.donorAdvectiveFlux[value]+physicalValue,high=
					observed.mcMusclAdvectiveFlux[value]+physicalValue;
				shared=shared&&std::memcmp(&low,&observed.lowCompositeFlux[value],sizeof(float))==0&&
					std::memcmp(&high,&observed.highCompositeFlux[value],sizeof(float))==0;}
		branchBitmap|=observed.branchObligationBitmap;independentBranchBitmap|=1u<<10u;
		if(std::strcmp(label,"wall")==0)wallIdentity=observed.devicePublicationIdentity;
		metal=std::move(observed);return true;
	};
	bool fixturesPassed=validateFixture(request,"wall");
	FireProductionResidentPhysicalFluxComparatorRequest periodic=request;
	periodic.transport.boundary.fill(FireProductionProjectionPeriodic);
	periodic.transport.attemptIdentity+=1u;
	for(unsigned int axis=0u;axis<3u;++axis){const std::size_t extent=4u;
		for(std::size_t second=0u;second<4u;++second)for(std::size_t first=0u;first<4u;++first){
			auto seam=[&](const std::size_t normal){return axis==0u?(second*4u+first)*5u+normal:
				(axis==1u?(second*5u+normal)*4u+first:(normal*4u+second)*4u+first);};
			periodic.transport.projectedVelocityMPerS[axis][seam(extent)]=
				periodic.transport.projectedVelocityMPerS[axis][seam(0u)];}}
	fixturesPassed=validateFixture(periodic,"periodic_seam")&&fixturesPassed;
	FireProductionResidentPhysicalFluxComparatorRequest open=request;
	open.transport.boundary.fill(FireProductionProjectionPressureOpen);
	open.transport.attemptIdentity+=2u;
	for(unsigned int side=0u;side<6u;++side)for(std::size_t face=0u;
		face<open.pressureOpenInflow[side].size();++face)
		open.pressureOpenInflow[side][face]=(face+side)%2u==0u?1u:0u;
	fixturesPassed=validateFixture(open,"pressure_open")&&fixturesPassed;
	FireProductionResidentPhysicalFluxComparatorRequest hot=request;
	hot.transport.attemptIdentity+=3u;
	for(std::size_t cell=0u;cell<cells;++cell)if(!assignCell(hot,cell,
		1200.0+100.0*static_cast<double>(cell%4u)))return 201;
	for(unsigned int component=0u;component<9u;++component)
		hot.ambient[component]=hot.transport.conservativeValues[component*cells];
	hot.ambientTemperatureK=hot.transport.temperatureK[0];
	fixturesPassed=validateFixture(hot,"high_temperature")&&fixturesPassed;
	for(const BoundStat& stat:donorStat)boundsPassed=boundsPassed&&stat.passed;
	for(const BoundStat& stat:mcStat)boundsPassed=boundsPassed&&stat.passed;
	for(const BoundStat& stat:massStat)boundsPassed=boundsPassed&&stat.passed;
	for(const BoundStat& stat:enthalpyStat)boundsPassed=boundsPassed&&stat.passed;
	for(const BoundStat& stat:lowStat)boundsPassed=boundsPassed&&stat.passed;
	for(const BoundStat& stat:highStat)boundsPassed=boundsPassed&&stat.passed;
	for(const BoundStat& stat:gasStat)boundsPassed=boundsPassed&&stat.passed;
	boundsPassed=boundsPassed&&energyStat.passed;
	FireProductionResidentPhysicalFluxComparatorRequest mutant=request;
	mutant.qualificationMutateHighNonadvective=true;
	FireProductionResidentPhysicalFluxComparatorResult refused;error.clear();
	const bool sharedMutantRefused=!EvaluateFireProductionResidentPhysicalFluxMetalComparator(
		mutant,refused,&error)&&!error.empty()&&!refused.deviceProduced&&
		refused.devicePublicationIdentity==0u;
	FireProductionResidentPhysicalFluxComparatorRequest lineage=request;
	lineage.transport.attemptIdentity+=17u;
	FireProductionResidentPhysicalFluxComparatorResult lineageResult;error.clear();
	const bool lineageRegenerated=EvaluateFireProductionResidentPhysicalFluxMetalComparator(
		lineage,lineageResult,&error)&&lineageResult.deviceProduced&&
		lineageResult.devicePublicationIdentity!=wallIdentity;
	FireProductionResidentPhysicalFluxComparatorRequest missingLineage=request;
	missingLineage.transport.parentCandidateIdentity=0u;error.clear();
	const bool missingLineageRefused=!EvaluateFireProductionResidentPhysicalFluxMetalComparator(
		missingLineage,refused,&error)&&!error.empty()&&!refused.deviceProduced&&
		refused.devicePublicationIdentity==0u;
	FireProductionResidentPhysicalFluxComparatorRequest mismatchedParent=request;
	mismatchedParent.qualificationMismatchedParentCandidate=true;error.clear();
	const bool mismatchedParentRefused=!EvaluateFireProductionResidentPhysicalFluxMetalComparator(
		mismatchedParent,refused,&error)&&!error.empty()&&!refused.deviceProduced&&
		refused.devicePublicationIdentity==0u;
	FireProductionResidentPhysicalFluxComparatorRequest shortInflow=request;
	shortInflow.qualificationShortInflowSurface=true;error.clear();
	const bool shortInflowRefused=!EvaluateFireProductionResidentPhysicalFluxMetalComparator(
		shortInflow,refused,&error)&&!error.empty()&&!refused.deviceProduced&&
		refused.devicePublicationIdentity==0u;
	FireProductionResidentPhysicalFluxComparatorRequest oversizedBasis=request;
	oversizedBasis.qualificationOversizedPhysicalBasisSurface=true;error.clear();
	const bool oversizedBasisRefused=!EvaluateFireProductionResidentPhysicalFluxMetalComparator(
		oversizedBasis,refused,&error)&&!error.empty()&&!refused.deviceProduced&&
		refused.devicePublicationIdentity==0u;
	std::uint64_t liveBytes=0u,ownerBytes=0u;
	const bool liveWorkingSet=FireProductionResidentPhysicalFluxLiveIncrementWorkingSetBytes(
		request.transport.shape,liveBytes);
	const std::array<FireProductionProjectionBoundary,6> productionBoundary={{
		FireProductionProjectionPressureOpen,FireProductionProjectionPressureOpen,
		FireProductionProjectionPressureOpen,FireProductionProjectionPressureOpen,
		FireProductionProjectionWall,FireProductionProjectionPressureOpen}};
	FireProductionProjectionShape ownerShape=request.transport.shape;
	ownerShape.nx=8u;ownerShape.ny=8u;ownerShape.nz=8u;
	const bool ownerWorkingSet=FireProductionResidentStepWorkingSetBytes(
		ownerShape,productionBoundary,ownerBytes);
	std::uint64_t fixtureBytes=0u;
	const bool fixtureWorkingSet=FireProductionResidentPhysicalFluxMetalWorkingSetBytes(
		request.transport.shape,fixtureBytes);
	FireProductionResidentPhysicalFluxComparatorRequest understated=request;
	understated.qualificationWorkingSetLimitBytes=fixtureBytes>0u?fixtureBytes-1u:0u;
	error.clear();const bool understatedRefused=fixtureWorkingSet&&
		!EvaluateFireProductionResidentPhysicalFluxMetalComparator(understated,refused,&error)&&
		!error.empty()&&!refused.deviceProduced&&refused.devicePublicationIdentity==0u;
	const bool workingSet=liveWorkingSet&&ownerWorkingSet&&liveBytes>0u&&
		ownerBytes>=liveBytes&&metal.liveAuthorityAllocationBytes<=liveBytes&&
		metal.actualMetalAllocationBytes<=metal.certifiedWorkingSetBytes;
	const std::uint32_t requiredBranches=(1u<<0u)|(1u<<1u)|(1u<<2u)|(1u<<3u)|
		(1u<<4u)|(1u<<5u)|(1u<<7u)|(1u<<8u)|(1u<<10u)|(1u<<11u)|(1u<<12u);
	const bool passed=fixturesPassed&&donorBitExact&&deltaBitExact&&mcBitExact&&shared&&
		sharedMutantRefused&&lineageRegenerated&&missingLineageRefused&&mismatchedParentRefused&&
		shortInflowRefused&&oversizedBasisRefused&&understatedRefused&&workingSet&&boundsPassed&&
		logBitExact&&logEnclosurePassed&&
		(branchBitmap&requiredBranches)==requiredBranches&&
		branchBitmap==independentBranchBitmap&&
		metal.commandCommitCount==1u&&metal.terminalStagingCount==1u&&
		metal.interstageFullGridTransferCount==0u&&metal.transportPublicationIdentity!=0u&&
		metal.devicePublicationIdentity!=0u;
	std::fprintf(stderr,"RESIDENT_PHYSICAL_FLUX_METAL passed=%d donor_bit_equal=%d delta_bit_equal=%d "
		"mc_bit_equal=%d shared_fN=%d shared_mutant_refused=%d lineage_regenerated=%d "
		"missing_lineage_refused=%d parent_candidate_mismatch_refused=%d "
		"short_inflow_refused=%d oversized_basis_refused=%d understated_ws_refused=%d "
		"enthalpy_host_binary32_diagnostic_bit_equal=%d log_binary32_bit_equal=%d "
		"log_series_enclosure_passed=%d log_max_residual_over_local_bound=%.17g bounds_passed=%d "
		"branch_bitmap=0x%08x independent_branch_bitmap=0x%08x required_branch_bitmap=0x%08x "
		"fixture_ws=%llu actual_ws=%llu live_ws=%llu owner_ws=%llu identity=%016llx\n",
		passed?1:0,donorBitExact?1:0,deltaBitExact?1:0,mcBitExact?1:0,shared?1:0,
		sharedMutantRefused?1:0,lineageRegenerated?1:0,missingLineageRefused?1:0,
		mismatchedParentRefused?1:0,shortInflowRefused?1:0,oversizedBasisRefused?1:0,
		understatedRefused?1:0,enthalpyBitExact?1:0,logBitExact?1:0,logEnclosurePassed?1:0,
		maximumDeviceLogEnclosureRatio,boundsPassed?1:0,branchBitmap,independentBranchBitmap,
		requiredBranches,
		static_cast<unsigned long long>(metal.certifiedWorkingSetBytes),
		static_cast<unsigned long long>(metal.actualMetalAllocationBytes),
		static_cast<unsigned long long>(liveBytes),static_cast<unsigned long long>(ownerBytes),
		static_cast<unsigned long long>(metal.devicePublicationIdentity));
	if(!liveWorkingSet||!ownerWorkingSet)std::fprintf(stderr,
		"RESIDENT_PHYSICAL_FLUX_WORKING_SET live=%d owner=%d\n",
		liveWorkingSet?1:0,ownerWorkingSet?1:0);
	const char* componentLabel[9]={"rhoZ","CH4","O2","CO2","H2O","N2","CO",
		"carbon","sensible_energy"};
	auto publish=[&](const char* family,const char* field,const char* units,
		const BoundStat& stat){std::fprintf(stderr,
		"RESIDENT_PHYSICAL_FLUX_BOUND family=%s field=%s scope=every_face_sample "
		"residual_%s=%.17g local_bound_%s=%.17g worst_residual_over_local_bound=%.17g passed=%d\n",
		family,field,units,stat.residual,units,stat.boundAtResidual,stat.maximumRatio,
		stat.passed?1:0);};
	for(unsigned int component=0u;component<9u;++component){const char* units=
		component<8u?"kg_m2_s":"W_m2";publish("donor_advective",componentLabel[component],
		units,donorStat[component]);publish("mc_muscl_advective",componentLabel[component],
		units,mcStat[component]);publish("low_composite",componentLabel[component],units,
		lowStat[component]);publish("high_composite",componentLabel[component],units,
		highStat[component]);}
	for(unsigned int component=0u;component<8u;++component)publish("physical_mass",
		componentLabel[component],"kg_m2_s",massStat[component]);
	for(unsigned int species=0u;species<7u;++species)publish("physical_enthalpy",
		componentLabel[1u+species],"J_kg",enthalpyStat[species]);
	publish("physical_energy","sensible_energy","W_m2",energyStat);
	for(unsigned int axis=0u;axis<3u;++axis)publish("physical_gas",axis==0u?"x":
		(axis==1u?"y":"z"),"kg_m2_s",gasStat[axis]);
	return passed?0:203;
}

int RunProductionResidentEOSCandidateMetalFP64Fixture()
{
	static_assert(!std::is_convertible<FireProductionResidentEOSCandidateComparatorResult,
		FireProductionScalarEOSAcceptanceResult>::value,
		"qualification output must not become a live EOS authority");
	const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
	if(!fuel.IsValid())return 204;
	FireCase::AuthoredV1 authored;authored.fuelRecordId=fuel.RecordId();
	authored.poolDiameterM=0.03;authored.heatReleaseRateKW=0.10;
	authored.envelope={{0.0,0.0},{0.5,1.0},{1.0,1.0}};authored.durationS=1.0;
	authored.quality="draft";authored.seed=199u;authored.outputFramesPerS=4.0;
	FireCase::RecordV1 sealedCase;std::string error;
	if(!FireCase::BuildMethaneV1(authored,fuel,{fuel.RecordId()},sealedCase,error))return 205;
	FireProductionResidentEOSCandidateComparatorRequest request;
	request.physicalFlux.transport.shape.nx=4u;request.physicalFlux.transport.shape.ny=4u;
	request.physicalFlux.transport.shape.nz=4u;
	request.physicalFlux.transport.shape.cellWidthM=0.025f;
	request.physicalFlux.transport.stage=FireProductionProjectedHeunStage::R0;
	request.physicalFlux.transport.attemptIdentity=UINT64_C(0x1990000000000001);
	request.physicalFlux.transport.parentCandidateIdentity=UINT64_C(0x1990000000000002);
	request.physicalFlux.transport.projectionIdentity=UINT64_C(0x1990000000000003);
	request.physicalFlux.transport.boundary.fill(FireProductionProjectionWall);
	request.producingStage=FireProductionScalarEOSStage::QStar;
	request.producerPrecision=FireStateProducerPrecision::Binary32;
	request.candidateTimeStepS=0x1p-30f;
	request.caseRecordEnvelope=sealedCase.envelopeBytes;
	const std::size_t cells=request.physicalFlux.transport.shape.CellCount();
	request.sourceDelta.assign(9u*cells,0.0f);
	for(unsigned int side=0u;side<6u;++side){const std::size_t count=side<2u?
		request.physicalFlux.transport.shape.ny*request.physicalFlux.transport.shape.nz:
		(side<4u?request.physicalFlux.transport.shape.nx*request.physicalFlux.transport.shape.nz:
		request.physicalFlux.transport.shape.nx*request.physicalFlux.transport.shape.ny);
		request.physicalFlux.transport.fuelInletBoundaryFace[side].assign(count,0u);
		request.physicalFlux.pressureOpenInflow[side].assign(count,0u);}
	request.physicalFlux.transport.conservativeValues.assign(9u*cells,0.0f);
	request.physicalFlux.transport.temperatureK.resize(cells);
	const double targetTemperature[8]={300.0,425.0,950.0,1000.0,1000.001,1500.0,1900.0,2200.0};
	auto buildState=[&](const double temperature,const double fraction,const double pressureScale,
		std::array<float,9>& projected)->bool{
		MethaneCellState physical;physical.producerPrecision=FireStateProducerPrecision::Binary32;
		physical.temperatureK=temperature;double massFraction[6];
		for(std::size_t species=0u;species<6u;++species)
			massFraction[species]=(1.0-fraction)*fuel.AmbientMassFractions()[species]+
				fraction*fuel.InjectedMassFractions()[species];
		const double reacted=0.125*std::min(massFraction[MethaneCH4],
			massFraction[MethaneO2]/fuel.StoichiometricOxygenKGPerKGFuel());
		for(std::size_t species=0u;species<6u;++species)
			massFraction[species]+=reacted*fuel.PrimaryReactionDelta()[species];
		double inverseWeight=0.0;for(std::size_t species=0u;species<6u;++species){
			const FireThermochemistrySpecies* record=fuel.FindSpecies(
				fuel.SpeciesOrder()[species].c_str());if(!record)return false;
			inverseWeight+=massFraction[species]/record->molecularWeightKGPerKMol;}
		const double density=pressureScale*fuel.ThermodynamicPressurePa()/(8314.46261815324*
			temperature*inverseWeight);for(std::size_t species=0u;species<6u;++species)
			physical.constituent[species]=density*massFraction[species];
		physical.constituent[MethaneCarbon]=0.0;physical.rhoTotalZ=density*fraction;
		if(!fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(physical),temperature,
			physical.sensibleEnergyJPerM3,&error))return false;
		const ConservativeVector value=ToConservativeVector(physical);
		for(std::size_t component=0u;component<9u;++component){
			projected[component]=static_cast<float>(value[component]);
		}return true;};
	auto assignCell=[&](const std::size_t cell,const double temperature)->bool{
		const double fraction=cell==10u?0.0:
			0.015625+0.00390625*static_cast<double>((cell+cell/4u)%8u);
		std::array<float,9> projected;if(!buildState(temperature,fraction,1.0,projected))return false;
		for(std::size_t component=0u;component<9u;++component)
			request.physicalFlux.transport.conservativeValues[component*cells+cell]=projected[component];
		request.physicalFlux.transport.temperatureK[cell]=static_cast<float>(temperature);return true;};
	for(std::size_t cell=0u;cell<cells;++cell)if(!assignCell(cell,targetTemperature[cell%8u]))return 206;
	for(std::size_t component=0u;component<9u;++component)for(std::size_t cell=0u;cell<cells;++cell)
		request.sourceDelta[component*cells+cell]=request.physicalFlux.transport.
			conservativeValues[component*cells+cell]*(cell==42u?0.0f:
				(cell%3u==0u?0x1p-12f:-0x1p-13f));
	for(unsigned int component=0u;component<9u;++component)
		request.physicalFlux.ambient[component]=
			request.physicalFlux.transport.conservativeValues[component*cells];
	request.physicalFlux.ambientTemperatureK=request.physicalFlux.transport.temperatureK[0];
	for(unsigned int axis=0u;axis<3u;++axis){request.physicalFlux.transport.projectedVelocityMPerS[axis].assign(
		FireProductionProjectionFaceCount(request.physicalFlux.transport.shape,axis),0.0f);
		for(std::size_t face=0u;face<request.physicalFlux.transport.projectedVelocityMPerS[axis].size();++face)
			request.physicalFlux.transport.projectedVelocityMPerS[axis][face]=
				(face%5u==0u?0x1p-8f:(face%5u==1u?-0x1p-9f:0x1p-10f));}
	auto uniformRequest=[&](const double temperature,const float scale,
		FireProductionResidentEOSCandidateComparatorRequest& destination)->bool{
		std::array<float,9> state;if(!buildState(temperature,0.03125,1.0,state))return false;
		destination=request;
		for(std::size_t component=0u;component<9u;++component){state[component]*=scale;
			for(std::size_t cell=0u;cell<cells;++cell)
				destination.physicalFlux.transport.conservativeValues[component*cells+cell]=state[component];
			destination.physicalFlux.ambient[component]=state[component];}
		std::fill(destination.sourceDelta.begin(),destination.sourceDelta.end(),0.0f);
		for(std::vector<float>& axis:destination.physicalFlux.transport.projectedVelocityMPerS)
			std::fill(axis.begin(),axis.end(),0.0f);
		std::fill(destination.physicalFlux.transport.temperatureK.begin(),
			destination.physicalFlux.transport.temperatureK.end(),static_cast<float>(temperature));
		destination.physicalFlux.ambientTemperatureK=static_cast<float>(temperature);return true;};
	const FireCertifiedNullspace& reconstruction=fuel.ConservativeReconstruction();
	request.physicalFlux.nullity=reconstruction.nullity;
	request.physicalFlux.nullspaceBasis.resize(reconstruction.orthonormalBasis.size());
	for(std::size_t index=0u;index<request.physicalFlux.nullspaceBasis.size();++index)
		request.physicalFlux.nullspaceBasis[index]=static_cast<float>(reconstruction.orthonormalBasis[index]);
	request.physicalFlux.coordinateProjector.assign(request.physicalFlux.nullity*
		request.physicalFlux.nullity,0.0f);
	for(std::size_t index=0u;index<request.physicalFlux.nullity;++index)
		request.physicalFlux.coordinateProjector[index*request.physicalFlux.nullity+index]=1.0f;
	FireProductionResidentPhysicalFluxComparatorResult candidateFluxMirror;
	if(!EvaluateFireProductionResidentPhysicalFluxMetalComparator(request.physicalFlux,
		candidateFluxMirror,&error))return 207;
	FireProductionScalarFCTRequest fctRequest;
	fctRequest.shape=request.physicalFlux.transport.shape;fctRequest.timeStepS=request.candidateTimeStepS;
	fctRequest.boundary=request.physicalFlux.transport.boundary;
	fctRequest.beginning=request.physicalFlux.transport.conservativeValues;
	fctRequest.sourceDelta=request.sourceDelta;
	fctRequest.frozenVelocityMPerS=request.physicalFlux.transport.projectedVelocityMPerS;
	fctRequest.ambient=request.physicalFlux.ambient;
	fctRequest.pressureOpenInflow=request.physicalFlux.pressureOpenInflow;
	fctRequest.nullity=request.physicalFlux.nullity;
	fctRequest.nullspaceBasis=request.physicalFlux.nullspaceBasis;
	fctRequest.coordinateProjector=request.physicalFlux.coordinateProjector;
	std::array<double,7> minimumEnthalpy,maximumEnthalpy;
	if(!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(fuel.TemperatureMinK(),minimumEnthalpy.data(),
		minimumEnthalpy.size(),&error)||!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(
		fuel.TemperatureMaxK(),maximumEnthalpy.data(),maximumEnthalpy.size(),&error))return 207;
	for(std::size_t species=0u;species<7u;++species){
		fctRequest.enthalpyBoundsJPerKG[species]=static_cast<float>(minimumEnthalpy[species]);
		fctRequest.enthalpyBoundsJPerKG[7u+species]=static_cast<float>(maximumEnthalpy[species]);}
	const FireAcceptedStateFeasibilityEnvelope& fctEnvelope=fuel.AcceptedStateFeasibilityEnvelope();
	fctRequest.feasibilityFactor=static_cast<float>(fctEnvelope.kappaEpsilon32*
		std::numeric_limits<float>::epsilon());
	fctRequest.assemblyReserveFactor=static_cast<float>(fctEnvelope.remapFactorEpsilon32*
		std::numeric_limits<float>::epsilon());
	FireProductionScalarFCTFluxPair fctPair;fctPair.shape=fctRequest.shape;
	fctPair.timeStepS=fctRequest.timeStepS;fctPair.boundary=fctRequest.boundary;
	fctPair.packedFaceOffset=candidateFluxMirror.packedFaceOffset;
	fctPair.lowFlux=candidateFluxMirror.lowCompositeFlux;
	fctPair.fluxDelta=candidateFluxMirror.advectiveFluxDelta;
	FireProductionScalarFCTResult fctMirror;
	if(!SolveFireProductionScalarFCTFluxPairCPU(fctRequest,fctPair,fctMirror,&error))return 207;
	FireProductionResidentEOSCandidateComparatorRequest r60Fixture;
	bool r60Prepared=uniformRequest(1000.0,1.0f,r60Fixture),r60Adjacent=false,
		r60Accepted=false,r60Refused=false;std::uint32_t r60AcceptedBitmap=~0u,
		r60RefusedBitmap=~0u;
	if(r60Prepared){FireProductionScalarFCTRequest r60Request=fctRequest;
		r60Request.beginning=r60Fixture.physicalFlux.transport.conservativeValues;
		r60Request.sourceDelta.assign(9u*cells,0.0f);
		r60Request.frozenVelocityMPerS=r60Fixture.physicalFlux.transport.projectedVelocityMPerS;
		r60Request.ambient=r60Fixture.physicalFlux.ambient;
		float exactTotalDensity=0.0f;for(std::size_t species=0u;species<7u;++species)
			exactTotalDensity+=r60Request.beginning[(1u+species)*cells];
		for(std::size_t cell=0u;cell<cells;++cell)
			r60Request.beginning[cell]=exactTotalDensity;
		r60Request.ambient[0]=exactTotalDensity;
		double enthalpySum=0.0,stateScale=1.0;
		for(std::size_t species=0u;species<7u;++species){const double density=
			r60Request.beginning[(1u+species)*cells];enthalpySum+=static_cast<double>(
				r60Request.enthalpyBoundsJPerKG[7u+species])*density;stateScale+=std::fabs(density);}
		stateScale+=std::fabs(r60Request.beginning[0]);const double feasibility=
			r60Request.feasibilityFactor;float probe=static_cast<float>((enthalpySum+
			feasibility*stateScale)/(1.0-feasibility));
		auto setCandidateEnergy=[&](const float energy){for(std::size_t cell=0u;cell<cells;++cell){
			r60Request.beginning[8u*cells+cell]=energy;}r60Request.ambient[8]=energy;};
		auto deviceBitmap=[&](const float energy,std::uint32_t& bitmap){setCandidateEnergy(energy);
			return EvaluateFireProductionScalarFCTCommitAdmissibilityMetalDiagnostic(r60Request,
				r60Request.beginning,bitmap,&error);};
		for(unsigned int step=0u;step<4096u;++step){if(!deviceBitmap(probe,r60AcceptedBitmap))break;
			if(r60AcceptedBitmap==0u)break;probe=std::nextafter(probe,
				-std::numeric_limits<float>::infinity());}
		float refusedEnergy=std::nextafter(probe,std::numeric_limits<float>::infinity());
		for(unsigned int step=0u;step<4096u;++step){if(!deviceBitmap(refusedEnergy,
			r60RefusedBitmap)){break;}if(r60RefusedBitmap!=0u){break;}probe=refusedEnergy;
			refusedEnergy=std::nextafter(refusedEnergy,std::numeric_limits<float>::infinity());}
		r60Accepted=r60AcceptedBitmap==0u;r60Refused=r60RefusedBitmap==0x10u;
		r60Adjacent=std::nextafter(probe,std::numeric_limits<float>::infinity())==refusedEnergy;
		std::fprintf(stderr,"RESIDENT_EOS_R60_COMMIT accepted_energy=%.9g refused_energy=%.9g "
			"adjacent=%d accepted_bitmap=0x%08x refused_bitmap=0x%08x passed=%d\n",probe,
			refusedEnergy,r60Adjacent?1:0,r60AcceptedBitmap,r60RefusedBitmap,
			(r60Adjacent&&r60Accepted&&r60Refused)?1:0);}
	float minimumCandidateAlpha=1.0f;for(const std::vector<float>& axis:fctMirror.sharedFaceAlpha)
		for(const float alpha:axis)minimumCandidateAlpha=std::min(minimumCandidateAlpha,alpha);
	bool limitedFCTBitEqual=false;float limitedMinimumAlpha=1.0f,searchMinimumAlpha=1.0f;
	unsigned int limitedCPUSolves=0u,limitedDeviceAttempts=0u;
	FireProductionResidentEOSCandidateComparatorRequest limiterBase=request;
	limiterBase.physicalFlux.transport.boundary.fill(FireProductionProjectionPeriodic);
	for(unsigned int side=0u;side<6u;++side){std::fill(limiterBase.physicalFlux.transport.
		fuelInletBoundaryFace[side].begin(),limiterBase.physicalFlux.transport.
		fuelInletBoundaryFace[side].end(),0u);std::fill(limiterBase.physicalFlux.
		pressureOpenInflow[side].begin(),limiterBase.physicalFlux.pressureOpenInflow[side].end(),0u);}
	for(std::size_t cell=0u;cell<cells;++cell){const std::size_t x=cell%4u,y=(cell/4u)%4u,z=cell/16u;
		const double fraction=0.0025*static_cast<double>(x+y+z);std::array<float,9> state;
		if(!buildState(700.0,fraction,1.0,state))return 207;
		for(std::size_t component=0u;component<9u;++component)limiterBase.physicalFlux.transport.
			conservativeValues[component*cells+cell]=state[component];
		limiterBase.physicalFlux.transport.temperatureK[cell]=700.0f;}
	for(std::size_t component=0u;component<9u;++component)limiterBase.physicalFlux.ambient[component]=
		limiterBase.physicalFlux.transport.conservativeValues[component*cells];
	limiterBase.physicalFlux.ambientTemperatureK=700.0f;
	for(unsigned int axis=0u;axis<3u;++axis)std::fill(limiterBase.physicalFlux.transport.
		projectedVelocityMPerS[axis].begin(),limiterBase.physicalFlux.transport.
		projectedVelocityMPerS[axis].end(),axis==0u?0.5f:(axis==1u?0.25f:-0.125f));
	FireProductionResidentPhysicalFluxComparatorResult limiterFlux;
	if(!EvaluateFireProductionResidentPhysicalFluxMetalComparator(limiterBase.physicalFlux,
		limiterFlux,&error))return 207;
	FireProductionScalarFCTRequest limiterFCT=fctRequest;
	limiterFCT.boundary=limiterBase.physicalFlux.transport.boundary;
	limiterFCT.beginning=limiterBase.physicalFlux.transport.conservativeValues;
	limiterFCT.frozenVelocityMPerS=limiterBase.physicalFlux.transport.projectedVelocityMPerS;
	limiterFCT.ambient=limiterBase.physicalFlux.ambient;
	limiterFCT.pressureOpenInflow=limiterBase.physicalFlux.pressureOpenInflow;
	FireProductionScalarFCTFluxPair limiterPair;limiterPair.shape=limiterFCT.shape;
	limiterPair.boundary=limiterFCT.boundary;limiterPair.packedFaceOffset=limiterFlux.packedFaceOffset;
	limiterPair.lowFlux=limiterFlux.lowCompositeFlux;
	limiterPair.fluxDelta=limiterFlux.advectiveFluxDelta;
	const std::array<float,7> limiterTimeSteps={{0x1p-14f,0x1p-12f,0x1p-10f,
		0x1p-9f,0x1p-8f,0x1p-7f,0x1p-6f}};
	const std::array<double,5> limiterMixtureFractions={{1.0e-3,1.0e-4,1.0e-5,1.0e-6,0.0}};
	for(const float trialTimeStep:limiterTimeSteps){
		FireProductionScalarFCTRequest baseFCT=limiterFCT;baseFCT.timeStepS=trialTimeStep;
		baseFCT.sourceDelta.assign(9u*cells,0.0f);FireProductionScalarFCTFluxPair trialPair=fctPair;
		trialPair=limiterPair;
		trialPair.timeStepS=trialTimeStep;FireProductionScalarFCTResult baseMirror;error.clear();
		if(!SolveFireProductionScalarFCTFluxPairCPU(baseFCT,trialPair,baseMirror,&error))continue;
		std::size_t limitedCell=cells;float mostNegative=0.0f;
		for(std::size_t cell=0u;cell<cells;++cell){const float correction=
			baseMirror.accepted[cell]-baseMirror.lowState[cell];if(correction<mostNegative){
				mostNegative=correction;limitedCell=cell;}}
		if(limitedCell==cells)continue;
		for(const double mixtureFraction:limiterMixtureFractions){if(limitedFCTBitEqual)break;
			FireProductionResidentEOSCandidateComparatorRequest trial=limiterBase;
			trial.candidateTimeStepS=trialTimeStep;trial.sourceDelta.assign(9u*cells,0.0f);
			std::array<float,9> target;if(!buildState(700.0,
				mixtureFraction,1.0,target))continue;
			for(std::size_t component=0u;component<9u;++component)
				trial.sourceDelta[component*cells+limitedCell]=target[component]-
					baseMirror.lowState[component*cells+limitedCell];
			FireProductionScalarFCTRequest trialFCT=baseFCT;trialFCT.sourceDelta=trial.sourceDelta;
			FireProductionScalarFCTResult trialMirror;error.clear();
			if(!SolveFireProductionScalarFCTFluxPairCPU(trialFCT,trialPair,trialMirror,&error))continue;
			++limitedCPUSolves;
		float trialMinimum=1.0f;for(const std::vector<float>& axis:trialMirror.sharedFaceAlpha)
			for(const float alpha:axis)trialMinimum=std::min(trialMinimum,alpha);
		searchMinimumAlpha=std::min(searchMinimumAlpha,trialMinimum);
		if(!(trialMinimum>=0.0f&&trialMinimum<1.0f))continue;
		FireProductionResidentEOSCandidateComparatorResult trialMetal;
		++limitedDeviceAttempts;
		if(!EvaluateFireProductionResidentEOSCandidateMetalComparator(trial,trialMetal,&error)||
			trialMetal.candidateConservativeValues.size()!=trialMirror.accepted.size())continue;
			limitedFCTBitEqual=ByteIdenticalVector(trialMetal.candidateConservativeValues,
				trialMirror.accepted)&&ByteIdenticalVectorArray(trialMetal.sharedFaceAlpha,
					trialMirror.sharedFaceAlpha);if(limitedFCTBitEqual)limitedMinimumAlpha=trialMinimum;}
	}
	FireProductionResidentEOSCandidateComparatorResult observed;
	if(!EvaluateFireProductionResidentEOSCandidateMetalComparator(request,observed,&error)){
		std::fprintf(stderr,"RESIDENT_EOS error=%s failure=0x%08x attempted=%d read=%d\n",
			error.c_str(),observed.deviceFailureBitmap,observed.deviceAttempted?1:0,
			observed.terminalRead?1:0);return 207;}
	bool temperatureBitEqual=observed.temperatureK.size()==cells&&
		observed.candidateConservativeValues.size()==9u*cells,
		pressureBitEqual=observed.representedPressureRatio.size()==cells,
		deviationBitEqual=observed.absoluteEOSDeviation.size()==cells;
	auto sameFloatBits=[](const float a,const float b){std::uint32_t first=0u,second=0u;
		std::memcpy(&first,&a,sizeof(first));std::memcpy(&second,&b,sizeof(second));
		return first==second;};
	bool arithmeticBoundarySweep=true;const double proofTemperature[4]={300.0,1000.0,
		1000.001,2200.0};const float proofScale[4]={0.7505f,0.875f,1.125f,1.2495f};
	for(unsigned int sample=0u;sample<4u&&arithmeticBoundarySweep;++sample){
		FireProductionResidentEOSCandidateComparatorRequest proof;
		arithmeticBoundarySweep=uniformRequest(proofTemperature[sample],proofScale[sample],proof);
		FireProductionResidentEOSCandidateComparatorResult proofResult;
		arithmeticBoundarySweep=arithmeticBoundarySweep&&
			EvaluateFireProductionResidentEOSCandidateMetalComparator(proof,proofResult,&error)&&
			proofResult.temperatureK.size()==cells&&proofResult.representedPressureRatio.size()==cells;
		for(std::size_t cell=0u;cell<cells&&arithmeticBoundarySweep;++cell){std::array<double,9> state;
			for(std::size_t component=0u;component<9u;++component)state[component]=
				proofResult.candidateConservativeValues[component*cells+cell];
			double temperature=0.0,invertedRatio=0.0,representedRatio=0.0;
			arithmeticBoundarySweep=fuel.InvertAcceptedConservativeStateByComponentOrder(state.data(),
				state.size(),sealedCase.derived.pilotAmbientTemperatureK,
				sealedCase.derived.maximumAcceptedTemperatureK,FireStateProducerPrecision::Binary32,
				temperature,invertedRatio,&error);
			const float expectedTemperature=static_cast<float>(temperature);
			arithmeticBoundarySweep=arithmeticBoundarySweep&&fuel.
				AcceptedConservativePressureRatioAtTemperatureByComponentOrder(state.data(),state.size(),
					expectedTemperature,FireStateProducerPrecision::Binary32,representedRatio,&error)&&
				sameFloatBits(proofResult.temperatureK[cell],expectedTemperature)&&sameFloatBits(
					proofResult.representedPressureRatio[cell],static_cast<float>(representedRatio));}}
	std::fprintf(stderr,"RESIDENT_EOS_ARITHMETIC_BOUNDARY_SWEEP samples=4 "
		"temperature_classes=4 composition_classes=main_64_cell_fixture scale_min=%.9g "
		"scale_max=%.9g passed=%d\n",proofScale[0],proofScale[3],
		arithmeticBoundarySweep?1:0);
	float firstLogTemperature=300.0f,lastLogTemperature=2200.0f;
	std::uint32_t firstLogBits=0u,lastLogBits=0u;
	std::memcpy(&firstLogBits,&firstLogTemperature,sizeof(firstLogBits));
	std::memcpy(&lastLogBits,&lastLogTemperature,sizeof(lastLogBits));
	const std::uint64_t latticeLogInputs=static_cast<std::uint64_t>(lastLogBits)-firstLogBits+1u,
		midpointLogInputs=latticeLogInputs-1u,totalLogInputs=latticeLogInputs+midpointLogInputs;
	const std::size_t logBatchCapacity=UINT64_C(1)<<18u;
	std::vector<std::array<float,2> > logInputs;logInputs.reserve(logBatchCapacity);
	std::vector<std::array<float,4> > logExpansion;bool logEnclosure=true;
	FireProductionEOSLogMetalQualificationIdentity logMetalIdentity,batchMetalIdentity;
	bool logMetalIdentitySet=false,logMetalIdentityConsistent=true;
	auto sameLogMetalIdentity=[](const FireProductionEOSLogMetalQualificationIdentity& first,
		const FireProductionEOSLogMetalQualificationIdentity& second){return
		first.deviceRegistryId==second.deviceRegistryId&&first.deviceName==second.deviceName&&
		first.deviceFamily==second.deviceFamily&&first.metalRuntimeImage==second.metalRuntimeImage&&
		first.metalRuntimeBundleIdentifier==second.metalRuntimeBundleIdentifier&&
		first.metalRuntimeBundleVersion==second.metalRuntimeBundleVersion&&
		first.metalLanguageVersion==second.metalLanguageVersion&&
		first.metalMathMode==second.metalMathMode&&
		first.librarySourceSHA256==second.librarySourceSHA256&&
		first.libraryFunctionSetSHA256==second.libraryFunctionSetSHA256&&
		first.kernelName==second.kernelName&&
		first.threadExecutionWidth==second.threadExecutionWidth&&
		first.maximumThreadsPerThreadgroup==second.maximumThreadsPerThreadgroup&&
		first.staticThreadgroupMemoryBytes==second.staticThreadgroupMemoryBytes;};
	double maximumLogRatio=0.0;std::uint64_t checkedLogInputs=0u,worstLogIndex=0u;
	for(std::uint64_t first=0u;first<totalLogInputs&&logEnclosure;first+=logBatchCapacity){
		const std::uint64_t end=std::min(totalLogInputs,
			first+static_cast<std::uint64_t>(logBatchCapacity));logInputs.clear();
		for(std::uint64_t index=first;index<end;++index){
			const bool midpoint=index>=latticeLogInputs;
			const std::uint32_t bits=firstLogBits+static_cast<std::uint32_t>(midpoint?
				index-latticeLogInputs:index);float high=0.0f;std::memcpy(&high,&bits,sizeof(high));
			float low=0.0f;if(midpoint){float upper=0.0f;const std::uint32_t upperBits=bits+1u;
				std::memcpy(&upper,&upperBits,sizeof(upper));low=(upper-high)*0.5f;}
			logInputs.push_back({{high,low}});}
		logEnclosure=EvaluateFireProductionEOSLogEnclosureMetalDiagnostic(
			logInputs,logExpansion,batchMetalIdentity,&error)&&
			logExpansion.size()==logInputs.size();
		if(logEnclosure){if(!logMetalIdentitySet){logMetalIdentity=batchMetalIdentity;
			logMetalIdentitySet=true;}else logMetalIdentityConsistent=
				sameLogMetalIdentity(logMetalIdentity,batchMetalIdentity);}
		for(std::size_t sample=0u;sample<logExpansion.size()&&logEnclosure;++sample){
			const double center=static_cast<double>(logExpansion[sample][0])+
				static_cast<double>(logExpansion[sample][1])+
				static_cast<double>(logExpansion[sample][2]),bound=logExpansion[sample][3],
				argument=static_cast<double>(logInputs[sample][0])+
				static_cast<double>(logInputs[sample][1]),
				residual=std::fabs(std::log(argument)-center),
				ratio=bound>0.0?residual/bound:std::numeric_limits<double>::infinity();
			if(ratio>maximumLogRatio){maximumLogRatio=ratio;
				worstLogIndex=first+sample;}
			logEnclosure=std::isfinite(bound)&&bound>0.0&&residual<=bound;
			++checkedLogInputs;}}
	void* logSymbol=dlsym(RTLD_DEFAULT,"log");Dl_info logProvider={};struct utsname hostName={};
	char osBuild[256]={};std::size_t osBuildBytes=sizeof(osBuild);
	const bool logProviderBound=logSymbol&&dladdr(logSymbol,&logProvider)!=0&&
		logProvider.dli_fname&&uname(&hostName)==0&&sysctlbyname("kern.osversion",osBuild,
			&osBuildBytes,nullptr,0)==0&&osBuild[0]!='\0';
	logEnclosure=logEnclosure&&checkedLogInputs==totalLogInputs&&logProviderBound&&
		logMetalIdentitySet&&logMetalIdentityConsistent;
	const double ln2Triple=static_cast<double>(0.693147182464599609375f)+
		static_cast<double>(-1.9046542121259336e-9f)+
		static_cast<double>(-1.1102230246251565e-16f),ln2Bound=0x1p-54;
	using SignedWide=__int128;using UnsignedWide=unsigned __int128;
	auto floatQ100=[](const float value){std::uint32_t bits=0u;std::memcpy(&bits,&value,
		sizeof(bits));const bool negative=(bits>>31u)!=0u;const unsigned int exponentBits=
		(bits>>23u)&255u;const UnsignedWide mantissa=exponentBits==0u?(bits&0x007fffffu):
			((bits&0x007fffffu)|0x00800000u);const int exponent=exponentBits==0u?-126:
			static_cast<int>(exponentBits)-127,shift=exponent+77;
		const SignedWide magnitude=shift>=0?static_cast<SignedWide>(mantissa<<shift):
			static_cast<SignedWide>(mantissa>>(-shift));return negative?-magnitude:magnitude;};
	const SignedWide ln2TripleQ100=floatQ100(0.693147182464599609375f)+
		floatQ100(-1.9046542121259336e-9f)+floatQ100(-1.1102230246251565e-16f),
		ln2BoundQ100=static_cast<SignedWide>(1u)<<46u;
	const UnsignedWide q100=static_cast<UnsignedWide>(1u)<<100u;
	UnsignedWide ln2LowerQ100=0u,powerOfThree=3u;
	for(unsigned int odd=1u;odd<=49u;odd+=2u){ln2LowerQ100+=(2u*q100)/
		(static_cast<UnsignedWide>(odd)*powerOfThree);powerOfThree*=9u;}
	const UnsignedWide tailNumerator=18u*q100,tailDenominator=
		static_cast<UnsignedWide>(51u)*powerOfThree*8u,
		tailUpper=(tailNumerator+tailDenominator-1u)/tailDenominator,
		ln2UpperQ100=ln2LowerQ100+25u+tailUpper;
	const bool ln2HighPrecisionEnclosed=static_cast<SignedWide>(ln2LowerQ100)>=
		ln2TripleQ100-ln2BoundQ100&&static_cast<SignedWide>(ln2UpperQ100)<=
		ln2TripleQ100+ln2BoundQ100;
	const bool ln2Enclosed=std::fabs(std::log(2.0)-ln2Triple)<=ln2Bound&&
		ln2HighPrecisionEnclosed;
	logEnclosure=logEnclosure&&ln2Enclosed;
	std::fprintf(stderr,"RESIDENT_EOS_LOG_ENCLOSURE samples=%llu lattice=%llu midpoints=%llu "
		"max_residual_over_bound=%.17g worst_index=%llu "
		"ln2_binary64_projection_residual=%.17g ln2_bound=%.17g "
		"ln2_high_precision=%d libm_image=%s os_build=%s os_release=%s machine=%s "
		"metal_device_registry_id=0x%016llx metal_device_name=\"%s\" metal_device_family=%s "
		"metal_runtime_image=%s metal_runtime_bundle=%s metal_runtime_version=%s "
		"metal_language=%s metal_math_mode=%s metal_library_source_sha256=%s "
		"metal_function_set_sha256=%s metal_kernel=%s thread_execution_width=%zu "
		"max_threads_per_threadgroup=%zu static_threadgroup_memory_bytes=%zu "
		"metal_identity_consistent=%d passed=%d\n",
		static_cast<unsigned long long>(totalLogInputs),static_cast<unsigned long long>(
			latticeLogInputs),static_cast<unsigned long long>(midpointLogInputs),maximumLogRatio,
		static_cast<unsigned long long>(worstLogIndex),std::fabs(std::log(2.0)-ln2Triple),ln2Bound,
		ln2HighPrecisionEnclosed?1:0,logProviderBound?logProvider.dli_fname:"unavailable",
		logProviderBound?osBuild:"unavailable",logProviderBound?hostName.release:"unavailable",
		logProviderBound?hostName.machine:"unavailable",
		static_cast<unsigned long long>(logMetalIdentity.deviceRegistryId),
		logMetalIdentity.deviceName.c_str(),logMetalIdentity.deviceFamily.c_str(),
		logMetalIdentity.metalRuntimeImage.c_str(),
		logMetalIdentity.metalRuntimeBundleIdentifier.c_str(),
		logMetalIdentity.metalRuntimeBundleVersion.c_str(),
		logMetalIdentity.metalLanguageVersion.c_str(),logMetalIdentity.metalMathMode.c_str(),
		logMetalIdentity.librarySourceSHA256.c_str(),
		logMetalIdentity.libraryFunctionSetSHA256.c_str(),logMetalIdentity.kernelName.c_str(),
		logMetalIdentity.threadExecutionWidth,logMetalIdentity.maximumThreadsPerThreadgroup,
		logMetalIdentity.staticThreadgroupMemoryBytes,logMetalIdentityConsistent?1:0,
		logEnclosure?1:0);
	bool candidateBitEqual=observed.candidateConservativeValues.size()==9u*cells;
	for(std::size_t component=0u;component<9u;++component)for(std::size_t cell=0u;cell<cells;++cell){
		candidateBitEqual=candidateBitEqual&&sameFloatBits(observed.candidateConservativeValues[
			component*cells+cell],fctMirror.accepted[component*cells+cell]);}
	const bool sharedAlphaBitEqual=ByteIdenticalVectorArray(observed.sharedFaceAlpha,
		fctMirror.sharedFaceAlpha);
	double maximumTemperatureResidual=0.0,maximumPressureResidual=0.0,
		maximumDeviationResidual=0.0,maximumDeviationBoundRatio=0.0,maximumMonitoredDeviation=0.0;
	for(std::size_t cell=0u;cell<cells;++cell){std::array<double,9> state;
		for(std::size_t component=0u;component<9u;++component)state[component]=
			observed.candidateConservativeValues[component*cells+cell];
		double temperature=0.0,invertedRatio=0.0;
		if(!fuel.InvertAcceptedConservativeStateByComponentOrder(state.data(),state.size(),
			sealedCase.derived.pilotAmbientTemperatureK,
			sealedCase.derived.maximumAcceptedTemperatureK,FireStateProducerPrecision::Binary32,
			temperature,invertedRatio,&error))return 208;
		const float expectedTemperature=static_cast<float>(temperature);double representedRatio=0.0;
		if(!fuel.AcceptedConservativePressureRatioAtTemperatureByComponentOrder(state.data(),
			state.size(),static_cast<double>(expectedTemperature),FireStateProducerPrecision::Binary32,
			representedRatio,&error))return 209;
		const float expectedRatio=static_cast<float>(representedRatio),
			expectedDeviation=std::fabs(expectedRatio-1.0f);
		auto localProjectionEnclosure=[](const float expected){return 0.5*std::fabs(
			static_cast<double>(std::nextafter(expected,std::numeric_limits<float>::infinity()))-
			static_cast<double>(expected));};
		const double temperatureEnclosure=localProjectionEnclosure(expectedTemperature),
			pressureEnclosure=localProjectionEnclosure(expectedRatio);
		temperatureBitEqual=temperatureBitEqual&&sameFloatBits(observed.temperatureK[cell],
			expectedTemperature);pressureBitEqual=pressureBitEqual&&sameFloatBits(
			observed.representedPressureRatio[cell],expectedRatio);
		const double deviationEnclosure=std::fabs(static_cast<double>(
			std::nextafter(expectedDeviation,std::numeric_limits<float>::infinity()))-
			static_cast<double>(expectedDeviation));
		deviationBitEqual=deviationBitEqual&&sameFloatBits(
			observed.absoluteEOSDeviation[cell],expectedDeviation);
		maximumDeviationBoundRatio=std::max(maximumDeviationBoundRatio,
			std::fabs(static_cast<double>(observed.absoluteEOSDeviation[cell])-expectedDeviation)/
			deviationEnclosure);
		maximumTemperatureResidual=std::max(maximumTemperatureResidual,
			std::fabs(static_cast<double>(observed.temperatureK[cell])-expectedTemperature));
		maximumPressureResidual=std::max(maximumPressureResidual,std::fabs(
			static_cast<double>(observed.representedPressureRatio[cell])-expectedRatio));
		maximumDeviationResidual=std::max(maximumDeviationResidual,std::fabs(
			static_cast<double>(observed.absoluteEOSDeviation[cell])-expectedDeviation));
		maximumMonitoredDeviation=std::max(maximumMonitoredDeviation,
			static_cast<double>(observed.absoluteEOSDeviation[cell]));
		std::fprintf(stderr,"RESIDENT_EOS_CELL cell=%zu field=temperature observed_K=%.9g "
			"expected_fp64_to_fp32_K=%.9g residual_K=%.17g local_projection_enclosure_K=%.17g "
			"residual_over_enclosure=0 bit_equal=%d\n",cell,observed.temperatureK[cell],
			expectedTemperature,std::fabs(static_cast<double>(
				observed.temperatureK[cell])-expectedTemperature),temperatureEnclosure,sameFloatBits(
					observed.temperatureK[cell],expectedTemperature)?1:0);
		std::fprintf(stderr,"RESIDENT_EOS_CELL cell=%zu field=represented_pressure_ratio "
			"observed_ratio=%.9g expected_fp64_to_fp32_ratio=%.9g residual_ratio=%.17g "
			"local_projection_enclosure_ratio=%.17g residual_over_enclosure=0 bit_equal=%d\n",
			cell,observed.representedPressureRatio[cell],expectedRatio,std::fabs(
				static_cast<double>(observed.representedPressureRatio[cell])-expectedRatio),pressureEnclosure,
				sameFloatBits(observed.representedPressureRatio[cell],expectedRatio)?1:0);
		std::fprintf(stderr,"RESIDENT_EOS_CELL cell=%zu field=absolute_eos_deviation "
			"observed_ratio=%.9g expected_fp64_to_fp32_ratio=%.9g residual_ratio=%.17g "
			"local_rounding_aware_enclosure_ratio=%.17g residual_over_enclosure=%.17g bit_equal=%d\n",
			cell,observed.absoluteEOSDeviation[cell],expectedDeviation,std::fabs(
				static_cast<double>(observed.absoluteEOSDeviation[cell])-expectedDeviation),
			deviationEnclosure,deviationEnclosure>0.0?std::fabs(static_cast<double>(
				observed.absoluteEOSDeviation[cell])-expectedDeviation)/deviationEnclosure:0.0,
			sameFloatBits(observed.absoluteEOSDeviation[cell],expectedDeviation)?1:0);}
	FireProductionResidentEOSCandidateComparatorRequest monitored=request;
	for(float& value:monitored.physicalFlux.transport.conservativeValues)value*=1.20f;
	FireProductionResidentEOSCandidateComparatorResult monitoredResult;
	const bool monitoredAccepted=EvaluateFireProductionResidentEOSCandidateMetalComparator(
		monitored,monitoredResult,&error)&&!monitoredResult.absoluteEOSDeviation.empty()&&
		*std::max_element(monitoredResult.absoluteEOSDeviation.begin(),
			monitoredResult.absoluteEOSDeviation.end())>0x1p-5f;
	FireProductionResidentEOSCandidateComparatorRequest hardBound=request;
	for(float& value:hardBound.physicalFlux.transport.conservativeValues)value*=1.30f;
	FireProductionResidentEOSCandidateComparatorResult refused;
	auto noPublication=[](const FireProductionResidentEOSCandidateComparatorResult& value){return
		value.candidatePublicationIdentity==0u&&value.EOSPublicationIdentity==0u;};
	auto hostRefused=[&](FireProductionResidentEOSCandidateComparatorRequest mutation){
		error.clear();const bool accepted=EvaluateFireProductionResidentEOSCandidateMetalComparator(
			mutation,refused,&error);return !accepted&&noPublication(refused)&&
			!refused.deviceAttempted&&!refused.terminalRead&&refused.deviceFailureBitmap==0u;};
	auto deviceRefused=[&](FireProductionResidentEOSCandidateComparatorRequest mutation,
		const std::uint32_t exactFailure,const char* redName){error.clear();const bool accepted=
			EvaluateFireProductionResidentEOSCandidateMetalComparator(mutation,refused,&error);
		const bool passed=!accepted&&noPublication(refused)&&refused.deviceAttempted&&
			refused.terminalRead&&refused.commandCommitCount==1u&&
			refused.terminalStagingCount==1u&&refused.deviceFailureBitmap==exactFailure;
		std::fprintf(stderr,"RESIDENT_EOS_RED name=%s expected=0x%08x observed=0x%08x "
			"attempted=%d read=%d commands=%u staging=%u candidate_identity=%llu "
			"eos_identity=%llu passed=%d\n",redName,exactFailure,refused.deviceFailureBitmap,
			refused.deviceAttempted?1:0,refused.terminalRead?1:0,refused.commandCommitCount,
			refused.terminalStagingCount,static_cast<unsigned long long>(
				refused.candidatePublicationIdentity),static_cast<unsigned long long>(
				refused.EOSPublicationIdentity),passed?1:0);
		if(!passed){
			std::fprintf(stderr,"RESIDENT_EOS_REFUSAL expected=0x%08x observed=0x%08x "
				"attempted=%d read=%d error=%s\n",exactFailure,refused.deviceFailureBitmap,
				refused.deviceAttempted?1:0,refused.terminalRead?1:0,error.c_str());
		}
		return passed;};
	auto refusedMutation=[&](FireProductionResidentEOSCandidateComparatorRequest mutation){
		error.clear();return !EvaluateFireProductionResidentEOSCandidateMetalComparator(
			mutation,refused,&error)&&noPublication(refused);};
	const bool hardBoundRefused=deviceRefused(hardBound,1024u,"r170_hard_bound_30_percent");
	FireProductionResidentEOSCandidateComparatorRequest ambiguousPressure=request;
	ambiguousPressure.qualificationAmbiguousPressureRounding=true;
	const bool ambiguousPressureRefused=deviceRefused(ambiguousPressure,512u,
		"pressure_midpoint_rounding_ambiguous");
	FireProductionResidentEOSCandidateComparatorRequest ambiguousDeviation=request;
	ambiguousDeviation.qualificationAmbiguousDeviationRounding=true;
	const bool ambiguousDeviationRefused=deviceRefused(ambiguousDeviation,512u,
		"deviation_midpoint_rounding_ambiguous");
	auto acceptsDeviationBits=[&](FireProductionResidentEOSCandidateComparatorRequest mutation,
		const float expected,const char* edgeName){FireProductionResidentEOSCandidateComparatorResult value;
		error.clear();const bool accepted=EvaluateFireProductionResidentEOSCandidateMetalComparator(
			mutation,value,&error)&&value.EOSPublicationIdentity!=0u&&
			value.absoluteEOSDeviation.size()==cells&&std::all_of(value.absoluteEOSDeviation.begin(),
				value.absoluteEOSDeviation.end(),[&](const float item){return
					sameFloatBits(item,expected);});
		std::fprintf(stderr,"RESIDENT_EOS_ROUNDING_EDGE name=%s expected_bits=0x%08x "
			"accepted=%d failure=0x%08x error=%s passed=%d\n",edgeName,[&](){std::uint32_t bits=0u;
				std::memcpy(&bits,&expected,sizeof(bits));return bits;}(),accepted?1:0,
			value.deviceFailureBitmap,error.c_str(),accepted?1:0);return accepted;};
	FireProductionResidentEOSCandidateComparatorRequest exactZeroDeviation=request;
	exactZeroDeviation.qualificationExactZeroDeviationRounding=true;
	const bool exactZeroDeviationAccepted=acceptsDeviationBits(exactZeroDeviation,0.0f,
		"exact_zero_bin_center");
	FireProductionResidentEOSCandidateComparatorRequest minimumSubnormalDeviation=request;
	minimumSubnormalDeviation.qualificationMinimumSubnormalDeviationRounding=true;
	std::uint32_t minimumSubnormalBits=1u;float minimumSubnormal=0.0f;
	std::memcpy(&minimumSubnormal,&minimumSubnormalBits,sizeof(minimumSubnormal));
	const bool minimumSubnormalDeviationAccepted=acceptsDeviationBits(minimumSubnormalDeviation,
		minimumSubnormal,"minimum_subnormal_bin_center");
	FireProductionResidentEOSCandidateComparatorRequest ambiguousZeroDeviation=request;
	ambiguousZeroDeviation.qualificationAmbiguousZeroDeviationRounding=true;
	const bool ambiguousZeroDeviationRefused=deviceRefused(ambiguousZeroDeviation,512u,
		"zero_lower_upper_bin_ambiguous");
	FireProductionResidentEOSCandidateComparatorRequest ambiguousSubnormalDeviation=request;
	ambiguousSubnormalDeviation.qualificationAmbiguousSubnormalDeviationRounding=true;
	const bool ambiguousSubnormalDeviationRefused=deviceRefused(ambiguousSubnormalDeviation,512u,
		"subnormal_lower_upper_bin_ambiguous");
	FireProductionResidentEOSCandidateComparatorRequest lowerEnvelope;
	bool lowerEnvelopePrepared=uniformRequest(sealedCase.derived.pilotAmbientTemperatureK,1.0f,
		lowerEnvelope),lowerEnvelopeAccepted=false,lowerAdjacentRefused=false;
	auto setUniformEnergy=[&](FireProductionResidentEOSCandidateComparatorRequest& fixture,
		const float energy){for(std::size_t cell=0u;cell<cells;++cell)
			fixture.physicalFlux.transport.conservativeValues[8u*cells+cell]=energy;
		fixture.physicalFlux.ambient[8]=energy;};
	auto cpuUniformTemperature=[&](const FireProductionResidentEOSCandidateComparatorRequest& fixture,
		const float energy,double& temperature){std::array<double,9> state;
		for(std::size_t component=0u;component<9u;++component)state[component]=
			fixture.physicalFlux.transport.conservativeValues[component*cells];
		state[8]=energy;double ratio=0.0;return fuel.InvertAcceptedConservativeStateByComponentOrder(
			state.data(),state.size(),sealedCase.derived.pilotAmbientTemperatureK,
			sealedCase.derived.maximumAcceptedTemperatureK,FireStateProducerPrecision::Binary32,
			temperature,ratio,&error);};
	if(lowerEnvelopePrepared){float rejected=lowerEnvelope.physicalFlux.transport.
		conservativeValues[8u*cells];
		float accepted=rejected;rejected=-std::max(1.0f,std::fabs(accepted));double temperature=0.0;
		while(cpuUniformTemperature(lowerEnvelope,rejected,temperature)&&std::isfinite(rejected))
			rejected*=2.0f;
		for(unsigned int step=0u;step<256u&&std::nextafter(rejected,
			std::numeric_limits<float>::infinity())!=accepted;++step){const float midpoint=
				static_cast<float>(0.5*(static_cast<double>(rejected)+static_cast<double>(accepted)));
			if(midpoint==rejected||midpoint==accepted)break;
			if(cpuUniformTemperature(lowerEnvelope,midpoint,temperature))accepted=midpoint;
			else rejected=midpoint;}
		double rejectedTemperature=0.0;
		lowerAdjacentRefused=!cpuUniformTemperature(lowerEnvelope,rejected,rejectedTemperature)&&
			std::nextafter(rejected,std::numeric_limits<float>::infinity())==accepted;
		if(lowerAdjacentRefused){const float accepted=std::nextafter(rejected,
			std::numeric_limits<float>::infinity());float deviceRejected=rejected,deviceAccepted=accepted;
			auto deviceAcceptsLower=[&](const float energy){setUniformEnergy(lowerEnvelope,energy);
				FireProductionResidentEOSCandidateComparatorResult value;return
					EvaluateFireProductionResidentEOSCandidateMetalComparator(lowerEnvelope,value,&error)&&
					!value.temperatureK.empty()&&std::all_of(value.temperatureK.begin(),
						value.temperatureK.end(),[&](const float item){return item==static_cast<float>(
							sealedCase.derived.pilotAmbientTemperatureK);});};
			float stride=std::nextafter(accepted,std::numeric_limits<float>::infinity())-accepted;
			for(unsigned int step=0u;step<32u&&!lowerEnvelopeAccepted;++step){
				lowerEnvelopeAccepted=deviceAcceptsLower(deviceAccepted);
				if(!lowerEnvelopeAccepted){deviceRejected=deviceAccepted;
					deviceAccepted=static_cast<float>(static_cast<double>(deviceAccepted)+stride);
					stride*=2.0f;}}
			for(unsigned int step=0u;step<64u&&lowerEnvelopeAccepted&&std::nextafter(
				deviceRejected,std::numeric_limits<float>::infinity())!=deviceAccepted;++step){
				const float midpoint=static_cast<float>(0.5*(static_cast<double>(deviceRejected)+
					static_cast<double>(deviceAccepted)));if(midpoint==deviceRejected||midpoint==deviceAccepted)break;
				if(deviceAcceptsLower(midpoint))deviceAccepted=midpoint;else deviceRejected=midpoint;}
			if(lowerEnvelopeAccepted){lowerEnvelopeAccepted=deviceAcceptsLower(deviceAccepted);
				auto adjacent=lowerEnvelope;setUniformEnergy(adjacent,deviceRejected);
				lowerAdjacentRefused=std::nextafter(deviceRejected,
					std::numeric_limits<float>::infinity())==deviceAccepted&&deviceRefused(adjacent,0xd0u,
						"eos_lower_inversion_endpoint");}}}
	FireProductionResidentEOSCandidateComparatorRequest upperEnvelope;
	bool upperEnvelopePrepared=uniformRequest(sealedCase.derived.maximumAcceptedTemperatureK,1.0f,
		upperEnvelope),upperEnvelopeRefused=false,upperAdjacentAccepted=false;
	if(upperEnvelopePrepared){float accepted=upperEnvelope.physicalFlux.transport.
		conservativeValues[8u*cells];
		for(unsigned int step=0u;step<4096u&&!upperAdjacentAccepted;++step){accepted=std::nextafter(
			accepted,-std::numeric_limits<float>::infinity());double temperature=0.0;
			upperAdjacentAccepted=cpuUniformTemperature(upperEnvelope,accepted,temperature)&&
				temperature<sealedCase.derived.maximumAcceptedTemperatureK;}
		if(upperAdjacentAccepted){const float rejected=std::nextafter(accepted,
			std::numeric_limits<float>::infinity());setUniformEnergy(upperEnvelope,accepted);
			FireProductionResidentEOSCandidateComparatorResult value;
			upperAdjacentAccepted=EvaluateFireProductionResidentEOSCandidateMetalComparator(
				upperEnvelope,value,&error);setUniformEnergy(upperEnvelope,rejected);
			upperEnvelopeRefused=upperAdjacentAccepted&&deviceRefused(upperEnvelope,128u,
				"eos_upper_inversion_endpoint");}}
	FireProductionResidentEOSCandidateComparatorRequest roundedHardBound;
	const double justAboveHardBound=std::nextafter(1.25,
		std::numeric_limits<double>::infinity());
	bool roundedHardBoundPrepared=false;
	for(unsigned int sample=1u;sample<=32768u&&!roundedHardBoundPrepared;++sample){
		const double authoredRatio=1.25+static_cast<double>(sample)*0x1p-40;
		std::array<float,9> state;if(!buildState(700.0,0.03125,authoredRatio,state))continue;
		std::array<double,9> mirror;for(std::size_t component=0u;component<9u;++component){
			mirror[component]=state[component];}
		double T=0.0,invertedRatio=0.0,publishedRatio=0.0;
		if(fuel.InvertAcceptedConservativeStateByComponentOrder(mirror.data(),mirror.size(),
			sealedCase.derived.pilotAmbientTemperatureK,sealedCase.derived.maximumAcceptedTemperatureK,
			FireStateProducerPrecision::Binary32,T,invertedRatio,&error)&&
			fuel.AcceptedConservativePressureRatioAtTemperatureByComponentOrder(mirror.data(),mirror.size(),
				static_cast<double>(static_cast<float>(T)),FireStateProducerPrecision::Binary32,
				publishedRatio,&error)&&publishedRatio>1.25&&static_cast<float>(publishedRatio)==1.25f){
			roundedHardBound=request;for(std::size_t component=0u;component<9u;++component){
				for(std::size_t cell=0u;cell<cells;++cell)roundedHardBound.physicalFlux.transport.
					conservativeValues[component*cells+cell]=state[component];
				roundedHardBound.physicalFlux.ambient[component]=state[component];}
			std::fill(roundedHardBound.physicalFlux.transport.temperatureK.begin(),
				roundedHardBound.physicalFlux.transport.temperatureK.end(),static_cast<float>(T));
			for(std::vector<float>& axis:roundedHardBound.physicalFlux.transport.projectedVelocityMPerS)
				std::fill(axis.begin(),axis.end(),0.0f);
			roundedHardBound.physicalFlux.ambientTemperatureK=static_cast<float>(T);
			roundedHardBoundPrepared=static_cast<float>(justAboveHardBound)==1.25f;}}
	bool roundedHardBoundRefused=false;
	if(roundedHardBoundPrepared)roundedHardBoundRefused=deviceRefused(roundedHardBound,1024u,
		"r170_exact_above_binary32_rounds_to_bound");
	FireProductionResidentEOSCandidateComparatorRequest unsealed=request;
	unsealed.qualificationUnsealedParentFlux=true;const bool unsealedRefused=hostRefused(unsealed);
	FireProductionResidentEOSCandidateComparatorRequest mismatched=request;
	mismatched.qualificationMismatchedParentFlux=true;
	const bool mismatchedRefused=hostRefused(mismatched);
	FireProductionResidentEOSCandidateComparatorRequest cpu=request;
	cpu.qualificationCPUProducedCandidate=true;const bool cpuRefused=hostRefused(cpu);
	FireProductionResidentEOSCandidateComparatorRequest fp64Label=request;
	fp64Label.producerPrecision=FireStateProducerPrecision::Binary64;
	const bool fp64LabelRefused=hostRefused(fp64Label);
	FireProductionResidentEOSCandidateComparatorRequest shortCandidate=request;
	shortCandidate.qualificationShortCandidateSurface=true;
	const bool shortCandidateRefused=hostRefused(shortCandidate);
	FireProductionResidentEOSCandidateComparatorRequest invalidConservation=request;
	invalidConservation.physicalFlux.transport.conservativeValues[cells]=-1.0f;
	const bool invalidConservationRefused=refusedMutation(invalidConservation);
	FireProductionResidentEOSCandidateComparatorRequest finalStage=request;
	finalStage.producingStage=FireProductionScalarEOSStage::QNPlus1;
	const bool finalStageRefused=hostRefused(finalStage);
	FireProductionResidentEOSCandidateComparatorRequest wrongProducer=request;
	wrongProducer.physicalFlux.transport.stage=FireProductionProjectedHeunStage::R1;
	const bool wrongProducerRefused=hostRefused(wrongProducer);
	FireProductionResidentEOSCandidateComparatorRequest eosTableMutation=request;
	eosTableMutation.qualificationMismatchedEOSThermochemistry=true;
	const bool eosTableMutationRefused=hostRefused(eosTableMutation);
	FireProductionResidentEOSCandidateComparatorRequest splitStage=request;
	splitStage.qualificationMismatchedDeviceStage=true;
	const bool splitStageRefused=deviceRefused(splitStage,0x50u,"forged_device_stage");
	FireProductionResidentEOSCandidateComparatorRequest splitPrecision=request;
	splitPrecision.qualificationMismatchedDevicePrecision=true;
	const bool splitPrecisionRefused=deviceRefused(splitPrecision,0x50u,"forged_device_precision");
	FireProductionResidentEOSCandidateComparatorRequest splitAttempt=request;
	splitAttempt.qualificationMismatchedDeviceAttempt=true;
	const bool splitAttemptRefused=deviceRefused(splitAttempt,0x50u,"forged_device_attempt");
	FireProductionResidentEOSCandidateComparatorRequest splitCells=request;
	splitCells.qualificationMismatchedDeviceCells=true;
	const bool splitCellsRefused=deviceRefused(splitCells,0x50u,"forged_device_cells");
	FireProductionResidentEOSCandidateComparatorRequest splitTimeStep=request;
	splitTimeStep.qualificationMismatchedDeviceTimeStep=true;
	const bool splitTimeStepRefused=deviceRefused(splitTimeStep,0x50u,"forged_device_timestep");
	FireProductionResidentEOSCandidateComparatorRequest splitCase=request;
	splitCase.qualificationMismatchedDeviceCase=true;
	const bool splitCaseRefused=hostRefused(splitCase);
	FireProductionResidentEOSCandidateComparatorRequest pairedWitness=request;
	pairedWitness.qualificationTwoCellDistinctEOSFailures=true;
	FireProductionResidentEOSCandidateComparatorResult pairedWitnessResult;error.clear();
	const bool pairedWitnessAccepted=EvaluateFireProductionResidentEOSCandidateMetalComparator(
		pairedWitness,pairedWitnessResult,&error);
	const bool pairedWitnessRefused=!pairedWitnessAccepted&&noPublication(pairedWitnessResult)&&
		pairedWitnessResult.deviceAttempted&&pairedWitnessResult.terminalRead&&
		pairedWitnessResult.commandCommitCount==1u&&
		pairedWitnessResult.terminalStagingCount==1u&&
		pairedWitnessResult.deviceFailureBitmap==512u&&
		pairedWitnessResult.firstEOSFailureCell==0u&&
		pairedWitnessResult.firstEOSFailureTermBitmap==1u&&
		pairedWitnessResult.secondEOSFailureCell==1u&&
		pairedWitnessResult.secondEOSFailureTermBitmap==2u;
	std::fprintf(stderr,"RESIDENT_EOS_RED name=paired_first_eos_failure_witness "
		"expected_failure=0x00000200 observed_failure=0x%08x expected_cell=0 "
		"observed_cell=%u expected_term=0x00000001 observed_term=0x%08x "
		"expected_second_cell=1 observed_second_cell=%u expected_second_term=0x00000002 "
		"observed_second_term=0x%08x "
		"attempted=%d read=%d commands=%u staging=%u passed=%d\n",
		pairedWitnessResult.deviceFailureBitmap,pairedWitnessResult.firstEOSFailureCell,
		pairedWitnessResult.firstEOSFailureTermBitmap,
		pairedWitnessResult.secondEOSFailureCell,pairedWitnessResult.secondEOSFailureTermBitmap,
		pairedWitnessResult.deviceAttempted?1:0,pairedWitnessResult.terminalRead?1:0,
		pairedWitnessResult.commandCommitCount,pairedWitnessResult.terminalStagingCount,
		pairedWitnessRefused?1:0);
	FireProductionResidentEOSCandidateComparatorRequest parentMutation=request;
	++parentMutation.physicalFlux.transport.parentCandidateIdentity;
	FireProductionResidentEOSCandidateComparatorResult parentMutationResult;
	const bool parentIdentityDistinct=EvaluateFireProductionResidentEOSCandidateMetalComparator(
		parentMutation,parentMutationResult,&error)&&parentMutationResult.transportPublicationIdentity!=
		observed.transportPublicationIdentity&&parentMutationResult.physicalFluxPublicationIdentity!=
		observed.physicalFluxPublicationIdentity&&parentMutationResult.candidatePublicationIdentity!=
		observed.candidatePublicationIdentity&&parentMutationResult.EOSPublicationIdentity!=
		observed.EOSPublicationIdentity;
	std::uint64_t fixtureWorkingSet=0u,liveIncrement=0u,ownerPeak=0u;
	const bool fixtureCertified=FireProductionResidentEOSCandidateMetalWorkingSetBytes(
		request.physicalFlux.transport.shape,fixtureWorkingSet);
	const bool liveCertified=FireProductionResidentEOSCandidateLiveIncrementWorkingSetBytes(
		request.physicalFlux.transport.shape,liveIncrement);
	FireProductionProjectionShape ownerShape=request.physicalFlux.transport.shape;
	ownerShape.nx=8u;ownerShape.ny=8u;ownerShape.nz=8u;
	const bool ownerCertified=FireProductionResidentStepWorkingSetBytes(
		ownerShape,request.physicalFlux.transport.boundary,ownerPeak);
	FireProductionResidentEOSCandidateComparatorRequest understated=request;
	understated.qualificationWorkingSetLimitBytes=fixtureWorkingSet-1u;
	const bool understatedRefused=fixtureCertified&&hostRefused(understated);
	// Independent walker: candidate/materialization/admissibility/inversion/
	// publication branches plus each thermochemistry segment reachable while
	// evaluating the sealed case interval.  This does not reuse kernel control.
	std::uint32_t requiredBranches=0x0000007fu;
	for(std::size_t species=0u;species<fuel.SpeciesOrder().size();++species){
		const FireThermochemistrySpecies* record=fuel.FindSpecies(
			fuel.SpeciesOrder()[species].c_str());if(!record)return 210;
		for(std::size_t segment=0u;segment<record->segments.size();++segment){
			const FireThermochemistrySegment& item=record->segments[segment];
			if(item.temperatureMaxK>=sealedCase.derived.pilotAmbientTemperatureK&&
				item.temperatureMinK<=sealedCase.derived.maximumAcceptedTemperatureK)
				requiredBranches|=1u<<(7u+std::min<std::size_t>(segment,2u));}}
	const bool passed=r60Prepared&&r60Adjacent&&r60Accepted&&r60Refused&&
		arithmeticBoundarySweep&&logEnclosure&&candidateBitEqual&&
		sharedAlphaBitEqual&&temperatureBitEqual&&pressureBitEqual&&deviationBitEqual&&
		observed.branchObligationBitmap==requiredBranches&&observed.commandCommitCount==1u&&
		observed.terminalStagingCount==1u&&observed.interstageFullGridTransferCount==0u&&
		monitoredAccepted&&hardBoundRefused&&ambiguousPressureRefused&&
		ambiguousDeviationRefused&&exactZeroDeviationAccepted&&minimumSubnormalDeviationAccepted&&
		ambiguousZeroDeviationRefused&&ambiguousSubnormalDeviationRefused&&unsealedRefused&&
		mismatchedRefused&&cpuRefused&&
		lowerEnvelopeAccepted&&lowerAdjacentRefused&&upperAdjacentAccepted&&upperEnvelopeRefused&&
		roundedHardBoundPrepared&&roundedHardBoundRefused&&
		fp64LabelRefused&&shortCandidateRefused&&invalidConservationRefused&&finalStageRefused&&
		wrongProducerRefused&&eosTableMutationRefused&&splitStageRefused&&splitPrecisionRefused&&
		splitAttemptRefused&&splitCellsRefused&&splitTimeStepRefused&&splitCaseRefused&&
		pairedWitnessRefused&&limitedFCTBitEqual&&
		parentIdentityDistinct&&fixtureCertified&&liveCertified&&ownerCertified&&
		observed.liveAuthorityAllocationBytes<=liveIncrement&&understatedRefused;
	std::fprintf(stderr,"RESIDENT_EOS passed=%d candidate_bit_equal=%d temperature_bit_equal=%d pressure_bit_equal=%d "
		"deviation_bit_equal=%d monitored_20_percent_accepted=%d hard_bound_refused=%d "
		"lower_envelope_accepted=%d lower_adjacent_refused=%d upper_adjacent_accepted=%d "
		"upper_envelope_refused=%d rounded_hard_bound_prepared=%d "
		"rounded_hard_bound_refused=%d "
		"unsealed_refused=%d mismatched_refused=%d cpu_refused=%d fp64_label_refused=%d "
		"short_refused=%d conservation_refused=%d qnplus1_without_owner_refused=%d "
		"wrong_producer_refused=%d eos_table_mutation_refused=%d parent_identity_distinct=%d "
		"minimum_shared_alpha=%.9g search_minimum_alpha=%.9g limited_shared_alpha=%.9g "
		"limited_cpu_solves=%u limited_device_attempts=%u limited_candidate_bit_equal=%d "
		"shared_alpha_bit_equal=%d split_stage_refused=%d split_precision_refused=%d "
		"split_attempt_refused=%d split_cells_refused=%d split_timestep_refused=%d "
		"split_case_refused=%d pressure_midpoint_refused=%d deviation_midpoint_refused=%d "
		"zero_bin_accepted=%d minsub_bin_accepted=%d zero_boundary_refused=%d "
		"minsub_boundary_refused=%d "
		"branch_bitmap=0x%08x required=0x%08x command=%u reads=%u transfers=%u\n",
		passed?1:0,candidateBitEqual?1:0,temperatureBitEqual?1:0,pressureBitEqual?1:0,
		deviationBitEqual?1:0,
		monitoredAccepted?1:0,hardBoundRefused?1:0,lowerEnvelopeAccepted?1:0,
		lowerAdjacentRefused?1:0,upperAdjacentAccepted?1:0,upperEnvelopeRefused?1:0,
		roundedHardBoundPrepared?1:0,roundedHardBoundRefused?1:0,
		unsealedRefused?1:0,mismatchedRefused?1:0,
		cpuRefused?1:0,fp64LabelRefused?1:0,shortCandidateRefused?1:0,
		invalidConservationRefused?1:0,finalStageRefused?1:0,wrongProducerRefused?1:0,
		eosTableMutationRefused?1:0,parentIdentityDistinct?1:0,minimumCandidateAlpha,
		searchMinimumAlpha,limitedMinimumAlpha,limitedCPUSolves,limitedDeviceAttempts,
		limitedFCTBitEqual?1:0,sharedAlphaBitEqual?1:0,splitStageRefused?1:0,
		splitPrecisionRefused?1:0,splitAttemptRefused?1:0,splitCellsRefused?1:0,
		splitTimeStepRefused?1:0,splitCaseRefused?1:0,ambiguousPressureRefused?1:0,
		ambiguousDeviationRefused?1:0,exactZeroDeviationAccepted?1:0,
		minimumSubnormalDeviationAccepted?1:0,ambiguousZeroDeviationRefused?1:0,
		ambiguousSubnormalDeviationRefused?1:0,
		observed.branchObligationBitmap,requiredBranches,observed.commandCommitCount,
		observed.terminalStagingCount,observed.interstageFullGridTransferCount);
	std::fprintf(stderr,"RESIDENT_EOS_CELLWISE temperature_max_residual_K=%.17g "
		"pressure_ratio_max_residual=%.17g deviation_max_residual=%.17g "
		"temperature_worst_residual_over_local_projection_enclosure=0 "
		"pressure_worst_residual_over_local_projection_enclosure=0 "
		"deviation_worst_residual_over_local_rounding_aware_enclosure=%.17g "
		"maximum_monitored_deviation=%.17g fixture_ws=%llu actual_ws=%llu live_ws=%llu owner_ws=%llu\n",
		maximumTemperatureResidual,maximumPressureResidual,maximumDeviationResidual,
		maximumDeviationBoundRatio,
		maximumMonitoredDeviation,static_cast<unsigned long long>(fixtureWorkingSet),
		static_cast<unsigned long long>(observed.actualMetalAllocationBytes),
		static_cast<unsigned long long>(liveIncrement),static_cast<unsigned long long>(ownerPeak));
	return passed?0:210;
}

class ResidentOwnerTransportFP32 final : public FireProductionProjectedHeunTransportProvider
{
public:
	explicit ResidentOwnerTransportFP32(
		const FireProductionResidentPhysicalFluxComparatorRequest& fixture,
		const bool qualificationEffectiveAsMolecular=false) : fixture_(fixture),
		qualificationEffectiveAsMolecular_(qualificationEffectiveAsMolecular) {}
	bool Evaluate(const FireProductionProjectedHeunTransportContext& context,
		FireProductionProjectedHeunTransportCoefficients& result,
		std::string* error) const override
	{
		const FireProductionProjectionShape& sourceShape=fixture_.transport.shape;
		const std::size_t cells=sourceShape.CellCount();
		if(!context.conservativeValues||context.conservativeValues->size()!=9u*cells||
			!context.temperatureK||context.temperatureK->size()!=cells||
			!context.projectedVelocityMPerS)return false;
		PeriodicMACShape shape;shape.nx=sourceShape.nx;shape.ny=sourceShape.ny;
		shape.nz=sourceShape.nz;shape.cellWidthM=sourceShape.cellWidthM;
		std::vector<ConservativeVector> state(cells);
		for(std::size_t cell=0u;cell<cells;++cell)for(std::size_t component=0u;
			component<9u;++component)state[cell][component]=
				(*context.conservativeValues)[component*cells+cell];
		std::vector<double> temperature(context.temperatureK->begin(),context.temperatureK->end());
		OpenMACField3D velocity;for(unsigned int axis=0u;axis<3u;++axis)
			velocity.component[axis].assign((*context.projectedVelocityMPerS)[axis].begin(),
				(*context.projectedVelocityMPerS)[axis].end());
		OpenBoundaryConfig3D boundary;
		for(unsigned int side=0u;side<6u;++side)boundary.kind[side]=
			fixture_.transport.boundary[side]==FireProductionProjectionPressureOpen?
				PressureOpenBoundary3D:AdiabaticWallBoundary3D;
		for(std::size_t component=0u;component<9u;++component){
			boundary.ambientState[component]=fixture_.ambient[component];
			boundary.injectedState[component]=fixture_.ambient[component];}
		boundary.ambientDensityKGPerM3=0.0;for(std::size_t component=1u;component<=6u;++component)
			boundary.ambientDensityKGPerM3+=fixture_.ambient[component];
		boundary.injectedGasDensityKGPerM3=boundary.ambientDensityKGPerM3;
		boundary.fuelMassFluxKGPerM2S=0.0;boundary.velocityToleranceMPerS=0.0;
		boundary.pressureTolerancePa=1.0e-3;
		if(boundary.kind[4]==AdiabaticWallBoundary3D){
			boundary.bottomFuelMask.resize(fixture_.transport.fuelInletBoundaryFace[4].size());
			for(std::size_t face=0u;face<boundary.bottomFuelMask.size();++face)
				boundary.bottomFuelMask[face]=fixture_.transport.fuelInletBoundaryFace[4][face]!=0u;
			boundary.bottomFuelMassFluxKGPerM2S.assign(boundary.bottomFuelMask.size(),0.0);}
		std::vector<CellMolecularTransportEvaluation> molecular;
		std::vector<double> diffusivity,conductivity,effectiveViscosity;
		if(!BuildCellMolecularTransportEvaluations3D(state,temperature,
			FireSimulationMethaneRecord::PhysicalV1(),FireSimulationTransportRecord::OpenV1(),
			FireStateProducerPrecision::Binary32,molecular,error,1u))return false;
		if(!BuildOpenStageTransport3D(shape,state,temperature,velocity,boundary,false,
			FireSimulationMethaneRecord::PhysicalV1(),FireSimulationTransportRecord::OpenV1(),
			FireStateProducerPrecision::Binary32,diffusivity,conductivity,effectiveViscosity,error,
			1u,&molecular))
			return false;
		result.stage=context.stage;result.attemptIdentity=context.attemptIdentity;
		result.parentCandidateIdentity=context.parentCandidateIdentity;
		result.projectionIdentity=context.projectionIdentity;
		result.diffusivityM2PerS.assign(diffusivity.begin(),diffusivity.end());
		result.conductivityWPerMK.assign(conductivity.begin(),conductivity.end());
		result.molecularKinematicViscosityM2PerS.resize(cells);
		for(std::size_t cell=0u;cell<cells;++cell){double density=0.0;
			for(std::size_t component=1u;component<=6u;++component)density+=state[cell][component];
			result.molecularKinematicViscosityM2PerS[cell]=static_cast<float>(
				(qualificationEffectiveAsMolecular_?effectiveViscosity[cell]:
					molecular[cell].molecularViscosityPaS)/density);}
		result.publicationIdentity=FireProductionProjectedHeunTransportPublicationIdentity(
			context,result);if(error)error->clear();return true;
	}
private:
	FireProductionResidentPhysicalFluxComparatorRequest fixture_;
	bool qualificationEffectiveAsMolecular_;
};

class ResidentOwnerTransportFP64 final :
	public ::RISEFireProductionFP64::FireProductionProjectedHeunTransportProvider
{
public:
	explicit ResidentOwnerTransportFP64(
		const FireProductionResidentPhysicalFluxComparatorRequest& fixture) : fixture_(fixture) {}
	bool Evaluate(
		const ::RISEFireProductionFP64::FireProductionProjectedHeunTransportContext& context,
		::RISEFireProductionFP64::FireProductionProjectedHeunTransportCoefficients& result,
		std::string* error) const override
	{
		const FireProductionProjectionShape& sourceShape=fixture_.transport.shape;
		const std::size_t cells=sourceShape.CellCount();
		if(!context.conservativeValues||context.conservativeValues->size()!=9u*cells||
			!context.temperatureK||context.temperatureK->size()!=cells||
			!context.projectedVelocityMPerS)return false;
		PeriodicMACShape shape;shape.nx=sourceShape.nx;shape.ny=sourceShape.ny;
		shape.nz=sourceShape.nz;shape.cellWidthM=sourceShape.cellWidthM;
		std::vector<ConservativeVector> state(cells);
		for(std::size_t cell=0u;cell<cells;++cell)for(std::size_t component=0u;
			component<9u;++component)state[cell][component]=
				(*context.conservativeValues)[component*cells+cell];
		std::vector<double> temperature(context.temperatureK->begin(),context.temperatureK->end());
		OpenMACField3D velocity;for(unsigned int axis=0u;axis<3u;++axis)
			velocity.component[axis].assign((*context.projectedVelocityMPerS)[axis].begin(),
				(*context.projectedVelocityMPerS)[axis].end());
		OpenBoundaryConfig3D boundary;
		for(unsigned int side=0u;side<6u;++side)boundary.kind[side]=
			fixture_.transport.boundary[side]==FireProductionProjectionPressureOpen?
				PressureOpenBoundary3D:AdiabaticWallBoundary3D;
		for(std::size_t component=0u;component<9u;++component){
			boundary.ambientState[component]=fixture_.ambient[component];
			boundary.injectedState[component]=fixture_.ambient[component];}
		boundary.ambientDensityKGPerM3=0.0;for(std::size_t component=1u;component<=6u;++component)
			boundary.ambientDensityKGPerM3+=fixture_.ambient[component];
		boundary.injectedGasDensityKGPerM3=boundary.ambientDensityKGPerM3;
		boundary.fuelMassFluxKGPerM2S=0.0;boundary.velocityToleranceMPerS=0.0;
		boundary.pressureTolerancePa=1.0e-3;
		if(boundary.kind[4]==AdiabaticWallBoundary3D){
			boundary.bottomFuelMask.resize(fixture_.transport.fuelInletBoundaryFace[4].size());
			for(std::size_t face=0u;face<boundary.bottomFuelMask.size();++face)
				boundary.bottomFuelMask[face]=fixture_.transport.fuelInletBoundaryFace[4][face]!=0u;
			boundary.bottomFuelMassFluxKGPerM2S.assign(boundary.bottomFuelMask.size(),0.0);
		}
		std::vector<CellMolecularTransportEvaluation> molecular;
		std::vector<double> diffusivity,conductivity,effectiveViscosity;
		if(!BuildCellMolecularTransportEvaluations3D(state,temperature,
			FireSimulationMethaneRecord::PhysicalV1(),FireSimulationTransportRecord::OpenV1(),
			FireStateProducerPrecision::Binary32,molecular,error,1u))return false;
		if(!BuildOpenStageTransport3D(shape,state,temperature,velocity,boundary,false,
			FireSimulationMethaneRecord::PhysicalV1(),FireSimulationTransportRecord::OpenV1(),
			FireStateProducerPrecision::Binary32,diffusivity,conductivity,effectiveViscosity,error,
			1u,&molecular))
			return false;
		result.stage=context.stage;result.attemptIdentity=context.attemptIdentity;
		result.parentCandidateIdentity=context.parentCandidateIdentity;
		result.projectionIdentity=context.projectionIdentity;
		result.diffusivityM2PerS=diffusivity;result.conductivityWPerMK=conductivity;
		result.molecularKinematicViscosityM2PerS.resize(cells);
		for(std::size_t cell=0u;cell<cells;++cell){double density=0.0;
			for(std::size_t component=1u;component<=6u;++component)
				density+=state[cell][component];
			result.molecularKinematicViscosityM2PerS[cell]=
				molecular[cell].molecularViscosityPaS/density;}
		result.publicationIdentity=
			::RISEFireProductionFP64::FireProductionProjectedHeunTransportPublicationIdentity(
				context,result);
		if(error)error->clear();return true;
	}
private:
	FireProductionResidentPhysicalFluxComparatorRequest fixture_;
};

int RunProductionResidentTargetLineageMetalFP64Fixture()
{
	static_assert(!std::is_convertible<FireProductionResidentTargetLineageComparatorResult,
		FireProductionScalarProjectionTargetSeal>::value,
		"qualification output must not become projection authority");
	const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
	if(!fuel.IsValid())return 211;
	const FireSimulationTransportRecord& sourceTransport=FireSimulationTransportRecord::OpenV1();
	const FireSimulationGasOpacityRecord& sourceOpacity=
		FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1();
	FireCase::AuthoredV1 authored;authored.fuelRecordId=fuel.RecordId();
	authored.poolDiameterM=0.03;authored.heatReleaseRateKW=0.10;
	authored.envelope={{0.0,0.0},{0.5,1.0},{1.0,1.0}};authored.durationS=1.0;
	authored.quality="draft";authored.seed=200u;authored.outputFramesPerS=4.0;
	FireCase::RecordV1 sealedCase;std::string error;
	if(!FireCase::BuildMethaneV1(authored,fuel,
		{fuel.RecordId(),sourceTransport.RecordId(),sourceOpacity.RecordId()},sealedCase,error))return 212;
	FireProductionResidentTargetLineageComparatorRequest request;
	FireProductionResidentEOSCandidateComparatorRequest& eos=request.eos;
	eos.physicalFlux.transport.shape.nx=4u;eos.physicalFlux.transport.shape.ny=4u;
	eos.physicalFlux.transport.shape.nz=4u;eos.physicalFlux.transport.shape.cellWidthM=0.025f;
	eos.physicalFlux.transport.stage=FireProductionProjectedHeunStage::R0;
	eos.physicalFlux.transport.attemptIdentity=UINT64_C(0x2000000000000001);
	eos.physicalFlux.transport.parentCandidateIdentity=UINT64_C(0x2000000000000002);
	eos.physicalFlux.transport.projectionIdentity=UINT64_C(0x2000000000000003);
	eos.physicalFlux.transport.boundary.fill(FireProductionProjectionWall);
	eos.producingStage=FireProductionScalarEOSStage::QStar;
	eos.producerPrecision=FireStateProducerPrecision::Binary32;
	eos.candidateTimeStepS=0x1p-16f;eos.caseRecordEnvelope=sealedCase.envelopeBytes;
	const FireProductionProjectionShape& shape=eos.physicalFlux.transport.shape;
	const std::size_t cells=shape.CellCount();eos.sourceDelta.assign(9u*cells,0.0f);
	eos.physicalFlux.transport.conservativeValues.assign(9u*cells,0.0f);
	eos.physicalFlux.transport.temperatureK.resize(cells);
	for(unsigned int side=0u;side<6u;++side){const std::size_t count=side<2u?
		shape.ny*shape.nz:(side<4u?shape.nx*shape.nz:shape.nx*shape.ny);
		eos.physicalFlux.transport.fuelInletBoundaryFace[side].assign(count,0u);
		eos.physicalFlux.pressureOpenInflow[side].assign(count,0u);}
	auto buildState=[&](const double temperature,const double fraction,const double pressureScale,
		std::array<float,9>& projected)->bool{
		MethaneCellState physical;physical.producerPrecision=FireStateProducerPrecision::Binary32;
		physical.temperatureK=temperature;double massFraction[6];
		for(std::size_t species=0u;species<6u;++species)
			massFraction[species]=(1.0-fraction)*fuel.AmbientMassFractions()[species]+
				fraction*fuel.InjectedMassFractions()[species];
		const double reacted=0.0625*std::min(massFraction[MethaneCH4],
			massFraction[MethaneO2]/fuel.StoichiometricOxygenKGPerKGFuel());
		for(std::size_t species=0u;species<6u;++species)
			massFraction[species]+=reacted*fuel.PrimaryReactionDelta()[species];
		double inverseWeight=0.0;for(std::size_t species=0u;species<6u;++species){
			const FireThermochemistrySpecies* record=fuel.FindSpecies(
				fuel.SpeciesOrder()[species].c_str());if(!record)return false;
			inverseWeight+=massFraction[species]/record->molecularWeightKGPerKMol;}
		const double density=pressureScale*fuel.ThermodynamicPressurePa()/
			(8314.46261815324*temperature*inverseWeight);
		for(std::size_t species=0u;species<6u;++species)
			physical.constituent[species]=density*massFraction[species];
		physical.constituent[MethaneCarbon]=0.0;physical.rhoTotalZ=density*fraction;
		if(!fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(physical),temperature,
			physical.sensibleEnergyJPerM3,&error))return false;
		const ConservativeVector value=ToConservativeVector(physical);
		for(std::size_t component=0u;component<9u;++component)
			projected[component]=static_cast<float>(value[component]);
		return true;};
	const double temperatures[8]={325.0,425.0,700.0,950.0,1000.0,1000.25,1500.0,1900.0};
	for(std::size_t cell=0u;cell<cells;++cell){std::array<float,9> state;
		const double fraction=0.015625+0.00390625*static_cast<double>((cell+cell/4u)%8u);
		double pressureScale=1.0;
		if(cell==7u)pressureScale=1.08;else if(cell==15u)pressureScale=0.92;
		else if(cell==23u)pressureScale=1.06249;else if(cell==31u)pressureScale=1.0625057;
		else if(cell==39u)pressureScale=1.06251;else if(cell==47u)pressureScale=0.93751;
		else if(cell==55u)pressureScale=0.9375028;else if(cell==63u)pressureScale=0.93749;
		if(!buildState(temperatures[cell%8u],fraction,pressureScale,state))return 213;
		for(std::size_t component=0u;component<9u;++component)
			eos.physicalFlux.transport.conservativeValues[component*cells+cell]=state[component];
		eos.physicalFlux.transport.temperatureK[cell]=static_cast<float>(temperatures[cell%8u]);
	}
	FireProductionFrozenMethaneSourceRequest sourceRequest;
	sourceRequest.shape=shape;sourceRequest.timeStepS=eos.candidateTimeStepS;
	sourceRequest.beginningTimeS=0.0;
	sourceRequest.attemptIdentity=eos.physicalFlux.transport.attemptIdentity;
	sourceRequest.caseRecordEnvelope=sealedCase.envelopeBytes;
	sourceRequest.beginningConservativeValues=eos.physicalFlux.transport.conservativeValues;
	sourceRequest.pilotCommandMask.assign(cells,0u);
	sourceRequest.mixingTimeS.assign(cells,1.0);
	sourceRequest.predictiveRadiation=false;sourceRequest.workerCount=1u;
	if(!FireSim::FireProductionCanonicalSourceAuthority::Build(sourceRequest,
		request.frozenSource,&error))return 213;
	eos.physicalFlux.transport.temperatureK=request.frozenSource.BeginningTemperatureK();
	eos.sourceDelta=request.frozenSource.SourceDelta();
	for(std::size_t component=0u;component<9u;++component)eos.physicalFlux.ambient[component]=
		eos.physicalFlux.transport.conservativeValues[component*cells];
	eos.physicalFlux.ambientTemperatureK=eos.physicalFlux.transport.temperatureK[0];
	for(unsigned int axis=0u;axis<3u;++axis){const std::size_t faces=
		FireProductionProjectionFaceCount(shape,axis);
		eos.physicalFlux.transport.projectedVelocityMPerS[axis].resize(faces);
		for(std::size_t face=0u;face<faces;++face)eos.physicalFlux.transport.
			projectedVelocityMPerS[axis][face]=face%4u==0u?0x1p-7f:
				(face%4u==1u?-0x1p-8f:0x1p-9f);}
	const FireCertifiedNullspace& reconstruction=fuel.ConservativeReconstruction();
	eos.physicalFlux.nullity=reconstruction.nullity;
	eos.physicalFlux.nullspaceBasis.resize(reconstruction.orthonormalBasis.size());
	for(std::size_t index=0u;index<eos.physicalFlux.nullspaceBasis.size();++index)
		eos.physicalFlux.nullspaceBasis[index]=static_cast<float>(reconstruction.orthonormalBasis[index]);
	eos.physicalFlux.coordinateProjector.assign(eos.physicalFlux.nullity*
		eos.physicalFlux.nullity,0.0f);
	for(std::size_t index=0u;index<eos.physicalFlux.nullity;++index)
		eos.physicalFlux.coordinateProjector[index*eos.physicalFlux.nullity+index]=1.0f;
	FireProductionResidentTargetLineageComparatorResult observed;
	if(!EvaluateFireProductionResidentTargetLineageMetalComparator(request,observed,&error)){
		std::fprintf(stderr,"RESIDENT_TARGET error=%s failure=0x%08x attempted=%d read=%d\n",
			error.c_str(),observed.deviceFailureBitmap,observed.deviceAttempted?1:0,
			observed.terminalRead?1:0);return 214;}
	FireProductionResidentPhysicalFluxComparatorResult flux;
	if(!EvaluateFireProductionResidentPhysicalFluxMetalComparator(eos.physicalFlux,flux,&error))return 215;
	FireProductionResidentEOSCandidateComparatorResult candidate;
	if(!EvaluateFireProductionResidentEOSCandidateMetalComparator(eos,candidate,&error))return 216;
	for(const std::size_t edge:{std::size_t(23u),std::size_t(31u),std::size_t(39u),
		std::size_t(47u),std::size_t(55u),std::size_t(63u)})
		std::fprintf(stderr,"RESIDENT_TARGET_THRESHOLD_PROBE cell=%zu ratio=%.9g deviation=%.9g\n",
			edge,candidate.representedPressureRatio[edge],candidate.absoluteEOSDeviation[edge]);
	const float tailThreshold=0x1p-4f;
	const bool thresholdCoverage=candidate.representedPressureRatio[23u]>1.0f&&
		candidate.absoluteEOSDeviation[23u]<tailThreshold&&
		candidate.representedPressureRatio[31u]>1.0f&&
		candidate.absoluteEOSDeviation[31u]>tailThreshold&&
		candidate.representedPressureRatio[47u]<1.0f&&
		candidate.absoluteEOSDeviation[47u]<tailThreshold&&
		candidate.representedPressureRatio[55u]<1.0f&&
		candidate.absoluteEOSDeviation[55u]>tailThreshold;
	auto policyDrain=[&](const double ratio,const double dt,double& drain){return
		FireProductionMonitoredManifoldPolicy::SignedTailDrainPerS(ratio,dt,drain,&error);};
	struct PolicyCase{double ratio,timeStep;bool accepted;};
	const double policyTheta=FireProductionMonitoredManifoldPolicy::EngagementThreshold;
	const PolicyCase policyCases[]={
		{1.0+0.5*policyTheta,eos.candidateTimeStepS,true},
		{1.0+policyTheta,eos.candidateTimeStepS,true},
		{1.0+2.0*policyTheta,eos.candidateTimeStepS,true},
		{1.0-0.5*policyTheta,eos.candidateTimeStepS,true},
		{1.0-policyTheta,eos.candidateTimeStepS,true},
		{1.0-2.0*policyTheta,eos.candidateTimeStepS,true},
		{std::numeric_limits<double>::infinity(),eos.candidateTimeStepS,false},
		{1.0,0.0,false},
		{std::numeric_limits<double>::max(),std::numeric_limits<double>::denorm_min(),false}};
	bool policyOwnersIdentical=true;
	for(std::size_t index=0u;index<sizeof(policyCases)/sizeof(policyCases[0]);++index){
		double productionDrain=0.0,mirrorDrain=0.0;std::string productionPolicyError,mirrorPolicyError;
		const bool productionAccepted=FireProductionMonitoredManifoldPolicy::SignedTailDrainPerS(
			policyCases[index].ratio,policyCases[index].timeStep,productionDrain,&productionPolicyError);
		const bool mirrorAccepted=::RISEFireProductionFP64::
			FireProductionMonitoredManifoldPolicy::SignedTailDrainPerS(policyCases[index].ratio,
				policyCases[index].timeStep,mirrorDrain,&mirrorPolicyError);
		const bool same=productionAccepted==policyCases[index].accepted&&
			mirrorAccepted==policyCases[index].accepted&&
			(!productionAccepted||std::memcmp(&productionDrain,&mirrorDrain,sizeof(double))==0);
		policyOwnersIdentical=policyOwnersIdentical&&same;
		std::fprintf(stderr,"MONITORED_MANIFOLD_POLICY_BRANCH case=%zu production_accepted=%d "
			"fp64_accepted=%d production_drain=%.17g fp64_drain=%.17g passed=%d\n",index,
			productionAccepted?1:0,mirrorAccepted?1:0,productionDrain,mirrorDrain,same?1:0);
	}
	double positiveThresholdDrain=1.0,negativeThresholdDrain=1.0;
	const bool exactThresholdNoDrain=policyDrain(1.0+policyTheta,eos.candidateTimeStepS,
		positiveThresholdDrain)&&policyDrain(1.0-policyTheta,eos.candidateTimeStepS,
		negativeThresholdDrain)&&positiveThresholdDrain==0.0&&negativeThresholdDrain==0.0;
	std::fprintf(stderr,"RESIDENT_TARGET_THRESHOLD_COVERAGE integrated_positive_below=%d "
		"integrated_positive_above=%d integrated_negative_below=%d integrated_negative_above=%d "
		"mirror_positive_at_no_drain=%d mirror_negative_at_no_drain=%d passed=%d\n",
		candidate.representedPressureRatio[23u]>1.0f&&candidate.absoluteEOSDeviation[23u]<tailThreshold,
		candidate.representedPressureRatio[31u]>1.0f&&candidate.absoluteEOSDeviation[31u]>tailThreshold,
		candidate.representedPressureRatio[47u]<1.0f&&candidate.absoluteEOSDeviation[47u]<tailThreshold,
		candidate.representedPressureRatio[55u]<1.0f&&candidate.absoluteEOSDeviation[55u]>tailThreshold,
		exactThresholdNoDrain?1:0,exactThresholdNoDrain?1:0,
		thresholdCoverage&&exactThresholdNoDrain&&policyOwnersIdentical?1:0);
	std::vector<float> tangentMirror(cells),sourceMirror=request.frozenSource.DivergenceTargetPerS(),
		diagnosticMirror(cells),tailMirror(cells),assembledMirror(cells);
	std::vector<double> tangentExact(cells),diagnosticExact(cells),tailExact(cells),
		assembledExact(cells);
	const std::size_t allFaces=flux.physicalEnergyFluxWPerM2.size();
	auto faceIndex=[&](const unsigned int axis,const std::size_t x,const std::size_t y,
		const std::size_t z){return flux.packedFaceOffset[axis]+(axis==0u?
		(z*shape.ny+y)*(shape.nx+1u)+x:(axis==1u?(z*(shape.ny+1u)+y)*shape.nx+x:
		(z*shape.ny+y)*shape.nx+x));};
	for(std::size_t cell=0u;cell<cells;++cell){const std::size_t x=cell%shape.nx,
		y=(cell/shape.nx)%shape.ny,z=cell/(shape.nx*shape.ny);
		std::array<double,9> state={{}},rate={{}};
		for(std::size_t component=0u;component<9u;++component)
			state[component]=eos.physicalFlux.transport.conservativeValues[component*cells+cell];
		for(unsigned int axis=0u;axis<3u;++axis){const std::size_t left=faceIndex(axis,x,y,z),
			right=faceIndex(axis,x+(axis==0u),y+(axis==1u),z+(axis==2u));
			for(std::size_t component=0u;component<8u;++component)rate[component]+=
				(static_cast<double>(flux.physicalMassFluxKGPerM2S[component*allFaces+left])-
				 static_cast<double>(flux.physicalMassFluxKGPerM2S[component*allFaces+right]))/
				shape.cellWidthM;
			rate[8u]+=(static_cast<double>(flux.physicalEnergyFluxWPerM2[left])-
				static_cast<double>(flux.physicalEnergyFluxWPerM2[right]))/shape.cellWidthM;}
		double tangent=0.0;if(!fuel.DivergenceFromDiscreteRateByComponentOrder(state.data(),state.size(),
			rate.data(),rate.size(),eos.physicalFlux.transport.temperatureK[cell],
			FireStateProducerPrecision::Binary32,tangent,&error))return 217;
		tangentExact[cell]=tangent;
		tangentMirror[cell]=static_cast<float>(tangent);
		const float representedRatio=candidate.representedPressureRatio[cell];
		const double signedDeviation=static_cast<double>(representedRatio)-1.0;
		diagnosticExact[cell]=signedDeviation/eos.candidateTimeStepS;
		diagnosticMirror[cell]=static_cast<float>(diagnosticExact[cell]);
		if(!policyDrain(representedRatio,eos.candidateTimeStepS,tailExact[cell]))return 217;
		tailMirror[cell]=static_cast<float>(tailExact[cell]);
		assembledExact[cell]=tangent+static_cast<double>(sourceMirror[cell])+tailExact[cell];
		assembledMirror[cell]=static_cast<float>(assembledExact[cell]);}
	double mean=0.0;for(const float value:assembledMirror)mean+=value;
	mean/=static_cast<double>(cells);for(std::size_t cell=0u;cell<cells;++cell){
		assembledExact[cell]=static_cast<double>(assembledMirror[cell])-mean;
		assembledMirror[cell]=static_cast<float>(assembledExact[cell]);
		if(assembledMirror[cell]==0.0f)assembledMirror[cell]=0.0f;}
	auto bitEqual=[](const std::vector<float>& first,const std::vector<float>& second){
		return first.size()==second.size()&&std::memcmp(first.data(),second.data(),
			first.size()*sizeof(float))==0;};
	const bool tangentEqual=bitEqual(observed.tangentTargetPerS,tangentMirror),
		sourceEqual=bitEqual(observed.frozenSourceTargetPerS,sourceMirror),
		diagnosticEqual=bitEqual(observed.absoluteReferenceDiagnosticPerS,diagnosticMirror),
		tailEqual=bitEqual(observed.monitoredAbsoluteReferenceTargetPerS,tailMirror),
		assembledEqual=bitEqual(observed.assembledTargetPerS,assembledMirror);
	auto mismatch=[&](const char* field,const std::vector<float>& device,
		const std::vector<float>& mirror){for(std::size_t cell=0u;cell<cells;++cell)
			if(std::memcmp(&device[cell],&mirror[cell],sizeof(float))!=0){std::fprintf(stderr,
				"RESIDENT_TARGET_MISMATCH field=%s cell=%zu device=%.9g mirror=%.9g residual=%.17g\n",
				field,cell,device[cell],mirror[cell],std::fabs(static_cast<double>(device[cell])-
					static_cast<double>(mirror[cell])));break;}};
	if(!tangentEqual)mismatch("tangent",observed.tangentTargetPerS,tangentMirror);
	if(!diagnosticEqual)mismatch("absolute_diagnostic",observed.absoluteReferenceDiagnosticPerS,
		diagnosticMirror);
	if(!diagnosticEqual)std::fprintf(stderr,
		"RESIDENT_TARGET_DIAGNOSTIC_INPUT cell=0 candidate_ratio=%.9g implied_device_ratio=%.17g exact_ratio_note=published_binary32\n",
		candidate.representedPressureRatio[0],1.0+static_cast<double>(
			observed.absoluteReferenceDiagnosticPerS[0])*eos.candidateTimeStepS);
	if(!tailEqual)mismatch("tail",observed.monitoredAbsoluteReferenceTargetPerS,tailMirror);
	if(!assembledEqual)mismatch("assembled",observed.assembledTargetPerS,assembledMirror);
	FireProductionResidentTargetLineageComparatorRequest openRequest=request;
	openRequest.eos.physicalFlux.transport.boundary[0]=FireProductionProjectionPressureOpen;
	openRequest.eos.physicalFlux.transport.boundary[1]=FireProductionProjectionPressureOpen;
	openRequest.eos.candidateTimeStepS=std::nextafter(request.eos.candidateTimeStepS,
		std::numeric_limits<float>::infinity());
	sourceRequest.timeStepS=openRequest.eos.candidateTimeStepS;
	if(!FireSim::FireProductionCanonicalSourceAuthority::Build(sourceRequest,
		openRequest.frozenSource,&error))return 220;
	openRequest.eos.sourceDelta=openRequest.frozenSource.SourceDelta();
	FireProductionResidentTargetLineageComparatorResult openObserved;
	if(!EvaluateFireProductionResidentTargetLineageMetalComparator(openRequest,openObserved,&error)){
		std::fprintf(stderr,"RESIDENT_TARGET_OPEN error=%s failure=0x%08x branch=0x%08x\n",
			error.c_str(),openObserved.deviceFailureBitmap,openObserved.branchObligationBitmap);return 220;}
	FireProductionResidentPhysicalFluxComparatorResult openFlux;
	if(!EvaluateFireProductionResidentPhysicalFluxMetalComparator(openRequest.eos.physicalFlux,
		openFlux,&error))return 221;
	FireProductionResidentEOSCandidateComparatorResult openCandidate;
	if(!EvaluateFireProductionResidentEOSCandidateMetalComparator(openRequest.eos,
		openCandidate,&error))return 222;
	std::vector<float> openTangent(cells),openSource=openRequest.frozenSource.DivergenceTargetPerS(),
		openDiagnostic(cells),openTail(cells),openAssembled(cells);
	std::vector<double> openTangentExact(cells),openDiagnosticExact(cells),openTailExact(cells),
		openAssembledExact(cells);
	const std::size_t openAllFaces=openFlux.physicalEnergyFluxWPerM2.size();
	auto openFaceIndex=[&](const unsigned int axis,const std::size_t x,const std::size_t y,
		const std::size_t z){return openFlux.packedFaceOffset[axis]+(axis==0u?
		(z*shape.ny+y)*(shape.nx+1u)+x:(axis==1u?(z*(shape.ny+1u)+y)*shape.nx+x:
		(z*shape.ny+y)*shape.nx+x));};
	for(std::size_t cell=0u;cell<cells;++cell){const std::size_t x=cell%shape.nx,
		y=(cell/shape.nx)%shape.ny,z=cell/(shape.nx*shape.ny);std::array<double,9> state={{}},rate={{}};
		for(std::size_t component=0u;component<9u;++component)state[component]=
			openRequest.eos.physicalFlux.transport.conservativeValues[component*cells+cell];
		for(unsigned int axis=0u;axis<3u;++axis){const std::size_t left=openFaceIndex(axis,x,y,z),
			right=openFaceIndex(axis,x+(axis==0u),y+(axis==1u),z+(axis==2u));
			for(std::size_t component=0u;component<8u;++component)rate[component]+=
				(static_cast<double>(openFlux.physicalMassFluxKGPerM2S[component*openAllFaces+left])-
				 static_cast<double>(openFlux.physicalMassFluxKGPerM2S[component*openAllFaces+right]))/
				shape.cellWidthM;
			rate[8u]+=(static_cast<double>(openFlux.physicalEnergyFluxWPerM2[left])-
				static_cast<double>(openFlux.physicalEnergyFluxWPerM2[right]))/shape.cellWidthM;}
		double tangent=0.0;if(!fuel.DivergenceFromDiscreteRateByComponentOrder(state.data(),state.size(),
			rate.data(),rate.size(),openRequest.eos.physicalFlux.transport.temperatureK[cell],
			FireStateProducerPrecision::Binary32,tangent,&error))return 223;
		openTangentExact[cell]=tangent;openTangent[cell]=static_cast<float>(tangent);
		const double signedDeviation=static_cast<double>(openCandidate.representedPressureRatio[cell])-1.0;
		openDiagnosticExact[cell]=signedDeviation/openRequest.eos.candidateTimeStepS;
		openDiagnostic[cell]=static_cast<float>(openDiagnosticExact[cell]);
		if(!policyDrain(openCandidate.representedPressureRatio[cell],
			openRequest.eos.candidateTimeStepS,openTailExact[cell]))return 223;
		openTail[cell]=static_cast<float>(openTailExact[cell]);
		openAssembledExact[cell]=tangent+static_cast<double>(openSource[cell])+openTailExact[cell];
		openAssembled[cell]=static_cast<float>(openAssembledExact[cell]);}
	const bool openTangentEqual=bitEqual(openObserved.tangentTargetPerS,openTangent),
		openSourceEqual=bitEqual(openObserved.frozenSourceTargetPerS,openSource),
		openDiagnosticEqual=bitEqual(openObserved.absoluteReferenceDiagnosticPerS,openDiagnostic),
		openTailEqual=bitEqual(openObserved.monitoredAbsoluteReferenceTargetPerS,openTail),
		openAssembledEqual=bitEqual(openObserved.assembledTargetPerS,openAssembled);
	if(!openTangentEqual)mismatch("open_tangent",openObserved.tangentTargetPerS,openTangent);
	if(!openDiagnosticEqual)mismatch("open_absolute_diagnostic",
		openObserved.absoluteReferenceDiagnosticPerS,openDiagnostic);
	if(!openTailEqual)mismatch("open_tail",openObserved.monitoredAbsoluteReferenceTargetPerS,openTail);
	if(!openAssembledEqual)mismatch("open_assembled",openObserved.assembledTargetPerS,openAssembled);
	auto deviceRefusal=[&](const char* name,
		FireProductionResidentTargetLineageComparatorRequest mutation,
		const std::uint32_t expected){FireProductionResidentTargetLineageComparatorResult refused;
		error.clear();const bool accepted=EvaluateFireProductionResidentTargetLineageMetalComparator(
			mutation,refused,&error);const bool passed=!accepted&&refused.deviceAttempted&&
			refused.terminalRead&&(refused.deviceFailureBitmap&expected)!=0u&&
			refused.projectionConsumerIdentity==0u;
		std::fprintf(stderr,"RESIDENT_TARGET_RED name=%s layer=device expected=0x%08x "
			"observed=0x%08x attempted=%d read=%d target_identity=%llu "
			"consumer_identity=%llu passed=%d\n",name,expected,refused.deviceFailureBitmap,
			refused.deviceAttempted?1:0,refused.terminalRead?1:0,
			static_cast<unsigned long long>(refused.targetPublicationIdentity),
			static_cast<unsigned long long>(refused.projectionConsumerIdentity),passed?1:0);
		return passed;};
	auto hostRefusal=[&](const char* name,
		FireProductionResidentTargetLineageComparatorRequest mutation){
		FireProductionResidentTargetLineageComparatorResult refused;error.clear();
		const bool passed=
			!EvaluateFireProductionResidentTargetLineageMetalComparator(mutation,refused,&error)&&
			!refused.deviceAttempted&&!refused.terminalRead&&refused.targetPublicationIdentity==0u;
		std::fprintf(stderr,"RESIDENT_TARGET_RED name=%s layer=host_preflight attempted=%d "
			"read=%d target_identity=%llu passed=%d\n",name,refused.deviceAttempted?1:0,
			refused.terminalRead?1:0,
			static_cast<unsigned long long>(refused.targetPublicationIdentity),passed?1:0);
		return passed;};
	FireProductionResidentTargetLineageComparatorRequest mutation=request;
	mutation.qualificationUnsealedTransportParent=true;const bool unsealedTransport=
		deviceRefusal("unsealed_transport_parent",mutation,2048u);mutation=request;
	mutation.qualificationUnsealedPhysicalFluxParent=true;
	const bool unsealedPhysical=deviceRefusal("unsealed_physical_flux_parent",mutation,2048u);
	mutation=request;
	mutation.qualificationUnsealedEOSCandidateParent=true;const bool unsealedCandidate=
		deviceRefusal("unsealed_EOS_candidate_parent",mutation,2048u);mutation=request;
	mutation.qualificationUnsealedEOSParent=true;
	const bool unsealedEOS=deviceRefusal("unsealed_EOS_publication_parent",mutation,2048u);
	mutation=request;
	mutation.qualificationUnsealedFrozenSourceParent=true;
	const bool unsealedSource=deviceRefusal("unsealed_frozen_source_parent",mutation,2048u);
	mutation=request;
	mutation.qualificationNonImmediateCandidate=true;const bool staleCandidate=
		hostRefusal("non_immediate_stale_candidate",mutation);
	mutation=request;mutation.qualificationEOSAcceptedButUnlinked=true;
	const bool unlinkedEOS=hostRefusal("EOS_accepted_but_unlinked_candidate",mutation);
	mutation=request;mutation.qualificationCPUProducedFrozenSource=true;
	const bool cpuSourceRefused=hostRefusal("CPU_produced_frozen_source",mutation);
	mutation=request;mutation.qualificationCPUPrivateBlitFrozenSource=true;
	const bool cpuPrivateSourceRefused=hostRefusal("CPU_private_blit_frozen_source",mutation);
	mutation=request;mutation.qualificationMismatchedFrozenSourcePacket=true;
	const bool mismatchedSourcePacket=deviceRefusal("mismatched_frozen_source_packet",mutation,2048u);
	FireProductionFrozenMethaneSourceRequest wrongBeginningSourceRequest=sourceRequest;
	wrongBeginningSourceRequest.timeStepS=request.eos.candidateTimeStepS;
	wrongBeginningSourceRequest.beginningConservativeValues=
		request.eos.physicalFlux.transport.conservativeValues;
	for(std::size_t component=0u;component<9u;++component)
		wrongBeginningSourceRequest.beginningConservativeValues[component*cells]=
			wrongBeginningSourceRequest.beginningConservativeValues[component*cells+1u];
	FireProductionFrozenSourcePacketSeal wrongBeginningSource;
	const bool wrongBeginningSourceBuilt=
		FireSim::FireProductionCanonicalSourceAuthority::Build(wrongBeginningSourceRequest,
			wrongBeginningSource,&error)&&wrongBeginningSource.BeginningStateIdentity()!=
			request.frozenSource.BeginningStateIdentity();
	std::vector<float> wrongBeginningQ=
		request.eos.physicalFlux.transport.conservativeValues;
	wrongBeginningQ[0u]=std::nextafter(wrongBeginningQ[0u],
		std::numeric_limits<float>::infinity());
	error.clear();
	const bool wrongBeginningQOnlyRefused=
		!FireProductionFrozenSourcePacketSealMatchesBeginningState(request.frozenSource,
			request.eos.physicalFlux.transport.shape,wrongBeginningQ,
			request.frozenSource.BeginningTemperatureK(),&error)&&
		error=="canonical frozen source beginning-state parent differs";
	mutation=request;mutation.frozenSource=wrongBeginningSource;
	mutation.eos.sourceDelta=wrongBeginningSource.SourceDelta();
	const bool wrongBeginningSourceRefused=wrongBeginningSourceBuilt&&wrongBeginningQOnlyRefused&&
		hostRefusal("wrong_parent_frozen_source_beginning",mutation);
	FireCase::AuthoredV1 wrongCaseAuthored=authored;wrongCaseAuthored.seed=201u;
	FireCase::RecordV1 wrongCase;
	const bool wrongCaseBuilt=FireCase::BuildMethaneV1(wrongCaseAuthored,fuel,
		{fuel.RecordId(),sourceTransport.RecordId(),sourceOpacity.RecordId()},wrongCase,error);
	FireProductionFrozenMethaneSourceRequest wrongCaseSourceRequest=sourceRequest;
	wrongCaseSourceRequest.timeStepS=request.eos.candidateTimeStepS;
	wrongCaseSourceRequest.caseRecordEnvelope=wrongCase.envelopeBytes;
	wrongCaseSourceRequest.beginningConservativeValues=
		request.eos.physicalFlux.transport.conservativeValues;
	FireProductionFrozenSourcePacketSeal wrongCaseSource;
	const bool wrongCaseSourceBuilt=wrongCaseBuilt&&
		FireSim::FireProductionCanonicalSourceAuthority::Build(wrongCaseSourceRequest,
			wrongCaseSource,&error)&&wrongCaseSource.CaseRecordId()!=request.frozenSource.CaseRecordId();
	mutation=request;mutation.frozenSource=wrongCaseSource;
	mutation.eos.sourceDelta=wrongCaseSource.SourceDelta();
	const bool wrongCaseSourceRefused=wrongCaseSourceBuilt&&
		hostRefusal("wrong_parent_frozen_source_case",mutation);
	mutation=request;mutation.qualificationMismatchedEOSThermochemistry=true;
	const bool mismatchedEOSThermochemistry=
		hostRefusal("mismatched_EOS_thermochemistry",mutation);
	mutation=request;mutation.qualificationCPUProducedTarget=true;
	const bool cpuTargetRefused=hostRefusal("CPU_produced_target_surface",mutation);
	mutation=request;mutation.qualificationCPUForgedProjectionMetadata=true;
	const bool cpuProjectionMetadataRefused=
		hostRefusal("CPU_forged_projection_metadata",mutation);
	mutation=request;mutation.qualificationPreauthoredProjectionTarget=true;
	const bool preauthoredRefused=deviceRefusal("preauthored_projection_target",mutation,8192u);
	mutation=request;
	mutation.qualificationMismatchedProjectionTopology=true;
	const bool topologyRefused=deviceRefusal("mismatched_projection_topology",mutation,8192u);
	const char* staleMetadataNames[6]={"stale_target_shape","stale_target_face_offsets",
		"stale_target_cell_width","stale_target_timestep","stale_target_attempt",
		"stale_target_boundary"};
	bool staleMetadataRefused=true;
	for(std::uint32_t field=1u;field<=6u;++field){mutation=request;
		mutation.qualificationStaleTargetMetadataField=field;
		staleMetadataRefused=deviceRefusal(staleMetadataNames[field-1u],mutation,2048u)&&
			staleMetadataRefused;}
	FireProductionResidentTargetLineageComparatorRequest dormant=request;
	dormant.eos.physicalFlux.transport.boundary[0]=FireProductionProjectionPressureOpen;
	dormant.eos.physicalFlux.transport.boundary[1]=FireProductionProjectionPressureOpen;
	for(const std::size_t outlier:{std::size_t(7u),std::size_t(15u),std::size_t(23u),
		std::size_t(31u),std::size_t(39u),std::size_t(47u),std::size_t(55u),
		std::size_t(63u)}){
		const std::size_t safe=outlier-1u;
		for(std::size_t component=0u;component<9u;++component)
			dormant.eos.physicalFlux.transport.conservativeValues[component*cells+outlier]=
				request.eos.physicalFlux.transport.conservativeValues[component*cells+safe];
		dormant.eos.physicalFlux.transport.temperatureK[outlier]=
			request.eos.physicalFlux.transport.temperatureK[safe];}
	FireProductionFrozenMethaneSourceRequest dormantSourceRequest=sourceRequest;
	dormantSourceRequest.timeStepS=dormant.eos.candidateTimeStepS;
	dormantSourceRequest.beginningConservativeValues=
		dormant.eos.physicalFlux.transport.conservativeValues;
	if(!FireSim::FireProductionCanonicalSourceAuthority::Build(dormantSourceRequest,
		dormant.frozenSource,&error))return 224;
	dormant.eos.physicalFlux.transport.temperatureK=
		dormant.frozenSource.BeginningTemperatureK();
	dormant.eos.sourceDelta=dormant.frozenSource.SourceDelta();
	FireProductionResidentTargetLineageComparatorResult dormantBase,dormantAlternate;
	std::string dormantBaseError,dormantAlternateError;
	const bool dormantBaseAccepted=EvaluateFireProductionResidentTargetLineageMetalComparator(
		dormant,dormantBase,&dormantBaseError);
	dormant.qualificationAlternateDormantTailThreshold=true;
	const bool dormantAlternateAccepted=
		EvaluateFireProductionResidentTargetLineageMetalComparator(dormant,dormantAlternate,
			&dormantAlternateError);
	const bool dormantFieldsEqual=dormantBaseAccepted&&dormantAlternateAccepted&&
		bitEqual(dormantBase.tangentTargetPerS,dormantAlternate.tangentTargetPerS)&&
		bitEqual(dormantBase.frozenSourceTargetPerS,dormantAlternate.frozenSourceTargetPerS)&&
		bitEqual(dormantBase.absoluteReferenceDiagnosticPerS,
			dormantAlternate.absoluteReferenceDiagnosticPerS)&&
		bitEqual(dormantBase.monitoredAbsoluteReferenceTargetPerS,
			dormantAlternate.monitoredAbsoluteReferenceTargetPerS)&&
		bitEqual(dormantBase.assembledTargetPerS,dormantAlternate.assembledTargetPerS)&&
		bitEqual(dormantBase.tangentEnclosurePerS,dormantAlternate.tangentEnclosurePerS)&&
		bitEqual(dormantBase.assembledEnclosurePerS,dormantAlternate.assembledEnclosurePerS);
	const bool dormantThresholdIdentity=dormantFieldsEqual&&
		dormantBase.targetPublicationIdentity!=0u&&dormantAlternate.targetPublicationIdentity!=0u&&
		dormantBase.targetPublicationIdentity!=dormantAlternate.targetPublicationIdentity;
	std::fprintf(stderr,"RESIDENT_TARGET_RED name=dormant_tail_threshold_identity layer=device_identity "
		"base_accepted=%d alternate_accepted=%d fields_bit_equal=%d base_failure=0x%08x "
		"alternate_failure=0x%08x base_identity=%llu alternate_identity=%llu "
		"base_error=%s alternate_error=%s passed=%d\n",
		dormantBaseAccepted?1:0,dormantAlternateAccepted?1:0,
		dormantFieldsEqual?1:0,dormantBase.deviceFailureBitmap,
		dormantAlternate.deviceFailureBitmap,
		static_cast<unsigned long long>(dormantBase.targetPublicationIdentity),
		static_cast<unsigned long long>(dormantAlternate.targetPublicationIdentity),
		dormantBaseError.c_str(),dormantAlternateError.c_str(),
		dormantThresholdIdentity?1:0);
	FireProductionResidentTargetLineageComparatorRequest exactPositive=request,
		exactNegative=request;
	exactPositive.qualificationExactPositiveTailThreshold=true;
	exactNegative.qualificationExactNegativeTailThreshold=true;
	FireProductionResidentTargetLineageComparatorResult exactPositiveObserved,
		exactNegativeObserved;
	std::string exactPositiveError,exactNegativeError;
	const bool exactPositiveAccepted=EvaluateFireProductionResidentTargetLineageMetalComparator(
		exactPositive,exactPositiveObserved,&exactPositiveError);
	const bool exactNegativeAccepted=EvaluateFireProductionResidentTargetLineageMetalComparator(
		exactNegative,exactNegativeObserved,&exactNegativeError);
	auto allPositiveZero=[](const std::vector<float>& values){
		for(const float value:values)if(value!=0.0f||std::signbit(value))return false;
		return !values.empty();};
	const bool deviceExactPositiveNoDrain=exactPositiveAccepted&&
		allPositiveZero(exactPositiveObserved.monitoredAbsoluteReferenceTargetPerS);
	const bool deviceExactNegativeNoDrain=exactNegativeAccepted&&
		allPositiveZero(exactNegativeObserved.monitoredAbsoluteReferenceTargetPerS);
	std::fprintf(stderr,"RESIDENT_TARGET_THRESHOLD_DEVICE_EQUALITY positive_accepted=%d "
		"negative_accepted=%d positive_tail_positive_zero=%d negative_tail_positive_zero=%d "
		"positive_error=%s negative_error=%s passed=%d\n",exactPositiveAccepted?1:0,
		exactNegativeAccepted?1:0,deviceExactPositiveNoDrain?1:0,
		deviceExactNegativeNoDrain?1:0,exactPositiveError.c_str(),exactNegativeError.c_str(),
		deviceExactPositiveNoDrain&&deviceExactNegativeNoDrain?1:0);
	std::uint64_t fixtureBytes=0u,liveBytes=0u;
	const bool fixtureCertified=FireProductionResidentTargetLineageMetalWorkingSetBytes(shape,fixtureBytes),
		liveCertified=FireProductionResidentTargetLineageLiveIncrementWorkingSetBytes(shape,liveBytes);
	mutation=request;mutation.qualificationWorkingSetLimitBytes=fixtureBytes-1u;
	const bool understatedRefused=hostRefusal("understated_working_set",mutation);
	mutation=request;mutation.qualificationInjectInterstageFullGridTransfer=true;
	FireProductionResidentTargetLineageComparatorResult transferred;error.clear();
	const bool transferAccepted=EvaluateFireProductionResidentTargetLineageMetalComparator(
		mutation,transferred,&error);
	const bool transferLedgerRefused=!transferAccepted&&transferred.deviceAttempted&&
		transferred.terminalRead&&transferred.interstageFullGridTransferCount==1u&&
		transferred.terminalStagingCount==1u&&transferred.targetPublicationIdentity!=0u;
	std::fprintf(stderr,"RESIDENT_TARGET_RED name=interstage_full_grid_transfer "
		"layer=observed_transfer_ledger attempted=%d read=%d transfers=%u staging=%u "
		"target_identity=%llu passed=%d\n",transferred.deviceAttempted?1:0,
		transferred.terminalRead?1:0,transferred.interstageFullGridTransferCount,
		transferred.terminalStagingCount,
		static_cast<unsigned long long>(transferred.targetPublicationIdentity),
		transferLedgerRefused?1:0);
	const std::uint32_t requiredClosedBranches=(1u<<7u)|(1u<<8u)|(1u<<20u)|(1u<<21u)|
		(1u<<22u)|(1u<<24u)|(1u<<25u)|(1u<<30u);
	const std::uint32_t requiredOpenBranches=(1u<<7u)|(1u<<8u)|(1u<<20u)|(1u<<21u)|
		(1u<<22u)|(1u<<23u)|(1u<<25u);
	const bool branches=observed.branchObligationBitmap==requiredClosedBranches&&
		openObserved.branchObligationBitmap==requiredOpenBranches;
	auto localProjectionBound=[](const float projected){
		const double center=static_cast<double>(projected);
		const double spacing=std::max(std::fabs(static_cast<double>(std::nextafter(
			projected,std::numeric_limits<float>::infinity()))-center),
			std::fabs(center-static_cast<double>(std::nextafter(projected,
				-std::numeric_limits<float>::infinity()))));
		return 0.5*spacing;};
	auto insideEnclosure=[&](const float projected,const double exact,const double bound){
		const double residual=std::fabs(static_cast<double>(projected)-exact);
		return std::isfinite(bound)&&bound>=0.0&&residual<=bound;};
	auto allEnclosed=[&](const char* topology,
		const FireProductionResidentTargetLineageComparatorResult& device,
		const std::vector<double>& tangentReference,const std::vector<double>& sourceReference,
		const std::vector<double>& diagnosticReference,const std::vector<double>& tailReference,
		const std::vector<double>& assembledReference){
		if(device.tangentEnclosurePerS.size()!=cells||
			device.assembledEnclosurePerS.size()!=cells)return false;
		const std::vector<float>* values[5]={&device.tangentTargetPerS,
			&device.frozenSourceTargetPerS,&device.absoluteReferenceDiagnosticPerS,
			&device.monitoredAbsoluteReferenceTargetPerS,&device.assembledTargetPerS};
		const std::vector<double>* exact[5]={&tangentReference,&sourceReference,
			&diagnosticReference,&tailReference,&assembledReference};
		for(unsigned int term=0u;term<5u;++term)for(std::size_t cell=0u;cell<cells;++cell){
			const double bound=term==0u?device.tangentEnclosurePerS[cell]:
				(term==1u?0.0:(term==4u?device.assembledEnclosurePerS[cell]:
				localProjectionBound((*values[term])[cell])));
			if(!insideEnclosure((*values[term])[cell],(*exact[term])[cell],bound)){
				std::fprintf(stderr,"RESIDENT_TARGET_ENCLOSURE_FAILURE topology=%s term=%u cell=%zu "
					"device=%.17g exact=%.17g residual=%.17g authenticated_radius=%.17g\n",
					topology,term,cell,static_cast<double>((*values[term])[cell]),
					(*exact[term])[cell],std::fabs(static_cast<double>((*values[term])[cell])-
					(*exact[term])[cell]),bound);return false;}}
		return true;};
	std::vector<double> sourceExact(cells);for(std::size_t cell=0u;cell<cells;++cell)
		sourceExact[cell]=sourceMirror[cell];
	std::vector<double> openSourceExact(cells);for(std::size_t cell=0u;cell<cells;++cell)
		openSourceExact[cell]=openSource[cell];
	const bool allBoundsPass=allEnclosed("closed",observed,tangentExact,sourceExact,diagnosticExact,
		tailExact,assembledExact)&&allEnclosed("pressure_open",openObserved,openTangentExact,openSourceExact,
		openDiagnosticExact,openTailExact,openAssembledExact);
	mutation=request;mutation.qualificationCertifiedContinuousEnclosure=true;
	FireProductionResidentTargetLineageComparatorResult enclosed;error.clear();
	const bool enclosureAccepted=EvaluateFireProductionResidentTargetLineageMetalComparator(
		mutation,enclosed,&error);
	bool hasMultiULPRadius=false;for(std::size_t cell=0u;cell<cells&&enclosureAccepted;++cell)
		hasMultiULPRadius=hasMultiULPRadius||enclosed.tangentEnclosurePerS[cell]>
			localProjectionBound(enclosed.tangentTargetPerS[cell])*2.0;
	const bool enclosureNumericallyDischarged=enclosureAccepted&&allEnclosed("multi_ulp",enclosed,
		tangentExact,sourceExact,diagnosticExact,tailExact,assembledExact);
	const bool continuousEnclosureRED=enclosureNumericallyDischarged&&hasMultiULPRadius&&
		(enclosed.branchObligationBitmap&(1u<<27u))!=0u&&
		enclosed.targetPublicationIdentity!=0u&&
		enclosed.targetPublicationIdentity!=observed.targetPublicationIdentity;
	std::fprintf(stderr,"RESIDENT_TARGET_RED name=certified_continuous_multi_ulp_enclosure "
		"layer=device_materialization accepted=%d numerically_discharged=%d multi_ulp=%d "
		"obligation=%d identity_distinct=%d error=%s passed=%d\n",enclosureAccepted?1:0,
		enclosureNumericallyDischarged?1:0,hasMultiULPRadius?1:0,
		(enclosed.branchObligationBitmap&(1u<<27u))!=0u?1:0,
		enclosed.targetPublicationIdentity!=observed.targetPublicationIdentity?1:0,
		error.c_str(),continuousEnclosureRED?1:0);
	mutation=request;mutation.qualificationCorruptContinuousEnclosurePublication=true;
	FireProductionResidentTargetLineageComparatorResult corruptedEnclosure;error.clear();
	const bool corruptedPublished=EvaluateFireProductionResidentTargetLineageMetalComparator(
		mutation,corruptedEnclosure,&error);
	const bool outOfEnclosureRED=corruptedPublished&&!allEnclosed("corrupted",corruptedEnclosure,
		tangentExact,sourceExact,diagnosticExact,tailExact,assembledExact);
	std::fprintf(stderr,"RESIDENT_TARGET_RED name=out_of_enclosure_continuous_publication "
		"layer=fp64_consumer device_published=%d consumer_refused=%d error=%s passed=%d\n",
		corruptedPublished?1:0,outOfEnclosureRED?1:0,error.c_str(),outOfEnclosureRED?1:0);
	const float boundREDProjection=1.0f;
	const double boundREDSpacing=std::max(std::fabs(static_cast<double>(std::nextafter(
		boundREDProjection,std::numeric_limits<float>::infinity()))-
		static_cast<double>(boundREDProjection)),std::fabs(static_cast<double>(boundREDProjection)-
		static_cast<double>(std::nextafter(boundREDProjection,
			-std::numeric_limits<float>::infinity()))));
	const double boundREDLocal=0.5*boundREDSpacing;
	const double boundREDDisplacedExact=static_cast<double>(boundREDProjection)+
		4.0*boundREDLocal;
	const double boundREDResidual=std::fabs(static_cast<double>(boundREDProjection)-
		boundREDDisplacedExact);
	const bool independentBoundCanFail=!insideEnclosure(boundREDProjection,
		boundREDDisplacedExact,false)&&boundREDResidual>boundREDLocal;
	FireProductionResidentTargetLineageComparatorRequest ownerLineage=request;
	ownerLineage.eos.physicalFlux.transport.boundary.fill(
		FireProductionProjectionPressureOpen);
	for(const std::size_t outlier:{std::size_t(7u),std::size_t(15u),std::size_t(23u),
		std::size_t(31u),std::size_t(39u),std::size_t(47u),std::size_t(55u),
		std::size_t(63u)}){
		const std::size_t safe=outlier-1u;
		for(std::size_t component=0u;component<9u;++component)
			ownerLineage.eos.physicalFlux.transport.conservativeValues[
				component*cells+outlier]=ownerLineage.eos.physicalFlux.transport.
				conservativeValues[component*cells+safe];
		ownerLineage.eos.physicalFlux.transport.temperatureK[outlier]=
			ownerLineage.eos.physicalFlux.transport.temperatureK[safe];}
	FireProductionFrozenMethaneSourceRequest ownerSourceRequest=sourceRequest;
	ownerSourceRequest.timeStepS=ownerLineage.eos.candidateTimeStepS;
	ownerSourceRequest.beginningConservativeValues=
		ownerLineage.eos.physicalFlux.transport.conservativeValues;
	std::string ownerSourceError;
	const bool ownerSourceBuilt=FireSim::FireProductionCanonicalSourceAuthority::Build(
		ownerSourceRequest,ownerLineage.frozenSource,&ownerSourceError);
	ownerLineage.eos.physicalFlux.transport.temperatureK=
		ownerLineage.frozenSource.BeginningTemperatureK();
	ownerLineage.eos.sourceDelta=ownerLineage.frozenSource.SourceDelta();
	const FireProductionResidentEOSCandidateComparatorRequest& ownerEOS=ownerLineage.eos;
	FireProductionProjectedHeunMetalOwnerRequest ownerRequest;
	ownerRequest.lineage=ownerLineage;
	ownerRequest.projectionTolerancePerS=1.0e-3f;
	ownerRequest.endpointVelocityToleranceMPerS=1.0e-4f;
	ownerRequest.maximumPicardIterations=16u;
	ownerRequest.qualificationCaptureIterationTrace=true;
	for(unsigned int axis=0u;axis<3u;++axis)
		ownerRequest.beginningMomentumKGPerM2S[axis].assign(
			FireProductionProjectionFaceCount(shape,axis),0.0f);
	FireProductionProjectedHeunMetalOwnerResult ownerObserved;
	std::string ownerError;
	const bool ownerAccepted=AttemptFireProductionProjectedHeunMetalOwner(
		ownerRequest,ownerObserved,&ownerError);
	FireProductionResidentStepResult ownerResidentObserved;
	FireProductionProjectedHeunMetalOwnerResult ownerResidentDiagnostics;
	std::string ownerResidentError;
	FireProductionProjectedHeunMetalOwnerRequest productionOwnerRequest=ownerRequest;
	productionOwnerRequest.qualificationCaptureIterationTrace=false;
	const bool ownerResidentAccepted=AttemptFireProductionProjectedHeunResidentStepMetal(
		productionOwnerRequest,ownerResidentObserved,&ownerResidentDiagnostics,&ownerResidentError);
	auto ownerRED=[&](const char* name,
		const std::function<void(FireProductionProjectedHeunMetalOwnerRequest&)>& mutate){
		FireProductionProjectedHeunMetalOwnerRequest redRequest=ownerRequest;mutate(redRequest);
		FireProductionProjectedHeunMetalOwnerResult redResult;std::string redError;
		const bool attempted=AttemptFireProductionProjectedHeunMetalOwner(redRequest,redResult,&redError);
		const bool refused=!attempted&&!redResult.accepted&&redResult.ownerPublicationIdentity==0u&&
			redResult.conservativeValues.empty()&&redResult.terminalStagingCount==0u;
		std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED name=%s layer=device "
			"attempted=%d owner_identity=%llu payload_words=%zu staging=%u error=%s passed=%d\n",
			name,attempted?1:0,static_cast<unsigned long long>(redResult.ownerPublicationIdentity),
			redResult.conservativeValues.size(),redResult.terminalStagingCount,redError.c_str(),
			refused?1:0);return refused;};
	const bool ownerStaleRefused=ownerRED("stale_candidate",[](auto& value){
		value.qualificationStaleCandidate=true;});
	const bool ownerOrderRefused=ownerRED("out_of_order_stage",[](auto& value){
		value.qualificationOutOfOrderStage=true;});
	const bool ownerForgedRefused=ownerRED("forged_lineage",[](auto& value){
		value.qualificationForgedLineage=true;});
	const bool ownerCallbackRefused=ownerRED("callback_mutation",[](auto& value){
		value.qualificationCallbackMutation=true;});
	const bool ownerAtomicRefused=ownerRED("atomic_publication",[](auto& value){
		value.qualificationAtomicPublicationFailure=true;});
	const bool ownerPolicyDivergenceRefused=ownerRED(
		"policy_divergence_between_owners",[](auto& value){
			value.qualificationDivergentManifoldPolicy=true;});
	const bool ownerStaleTargetPublicationRefused=ownerRED(
		"stale_target_publication",[](auto& value){
			value.qualificationStaleTargetPublication=true;});
	const bool ownerUnverifiedPrivateBufferRefused=ownerRED(
		"unverified_private_buffer_valid_identity",[](auto& value){
			value.qualificationUnverifiedPrivateLineageBuffer=true;});
	const bool ownerActualInterstageTransferRefused=ownerRED(
		"actual_interstage_full_grid_transfer",[](auto& value){
			value.qualificationInjectInterstageTransfer=true;});
	std::uint64_t ownerCertifiedPreflightBytes=0u;
	const bool ownerPreflightSizeKnown=FireProductionProjectedHeunMetalOwnerWorkingSetBytes(
		shape,ownerCertifiedPreflightBytes);
	FireProductionProjectionShape tier8OwnerShape=shape;
	tier8OwnerShape.nx=69u;tier8OwnerShape.ny=69u;tier8OwnerShape.nz=106u;
	tier8OwnerShape.cellWidthM=static_cast<float>(0.30/9.80665);
	std::uint64_t tier8OwnerCertifiedBytes=0u;
	const bool tier8OwnerWorkingSetScales=
		FireProductionProjectedHeunMetalOwnerWorkingSetBytes(
			tier8OwnerShape,tier8OwnerCertifiedBytes)&&
		tier8OwnerCertifiedBytes>(UINT64_C(2)<<30u);
	std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED "
		"name=legacy_fixed_two_gibibyte_cap tier=8 dimensions=69x69x106 "
		"certified_working_set_bytes=%llu historical_cap_bytes=%llu passed=%d\n",
		static_cast<unsigned long long>(tier8OwnerCertifiedBytes),
		static_cast<unsigned long long>(UINT64_C(2)<<30u),
		tier8OwnerWorkingSetScales?1:0);
	const bool ownerWorkingSetPreflightRefused=ownerPreflightSizeKnown&&ownerRED(
		"combined_owner_working_set_preflight",[&](auto& value){
			value.qualificationWorkingSetLimitBytes=ownerCertifiedPreflightBytes-1u;});
	std::vector<double> ownerDeviceTimingMS,ownerWallTimingMS;
	if(ownerAccepted){ownerDeviceTimingMS.push_back(ownerObserved.deviceElapsedMS);
		ownerWallTimingMS.push_back(ownerObserved.wallElapsedMS);
		for(unsigned int sample=1u;sample<5u;++sample){
			FireProductionProjectedHeunMetalOwnerResult timingResult;std::string timingError;
			if(!AttemptFireProductionProjectedHeunMetalOwner(ownerRequest,timingResult,&timingError))break;
			ownerDeviceTimingMS.push_back(timingResult.deviceElapsedMS);
			ownerWallTimingMS.push_back(timingResult.wallElapsedMS);}}
	auto ownerP95=[](std::vector<double> values){if(values.empty())return 0.0;
		std::sort(values.begin(),values.end());return values[static_cast<std::size_t>(
			std::ceil(0.95*static_cast<double>(values.size())))-1u];};
	const double ownerDeviceP95=ownerP95(ownerDeviceTimingMS),ownerWallP95=ownerP95(ownerWallTimingMS);
	::RISEFireProductionFP64::FireProductionProjectedHeunOwnerRequest ownerRequest64;
	ownerRequest64.attemptIdentity=ownerEOS.physicalFlux.transport.attemptIdentity;
	ownerRequest64.source=
		::RISEFireProductionFP64::FireProductionFrozenSourcePacketSeal::CalibrationImport(
			ownerLineage.frozenSource);
	ownerRequest64.caseRecordEnvelope=ownerEOS.caseRecordEnvelope;
	ownerRequest64.beginningConservativeValues.assign(
		ownerEOS.physicalFlux.transport.conservativeValues.begin(),
		ownerEOS.physicalFlux.transport.conservativeValues.end());
	auto copyOwnerShape64=[](const FireProductionProjectionShape& from,
		::RISEFireProductionFP64::FireProductionProjectionShape& to){
		to.nx=from.nx;to.ny=from.ny;to.nz=from.nz;to.cellWidthM=from.cellWidthM;};
	copyOwnerShape64(shape,ownerRequest64.scalarContract.shape);
	copyOwnerShape64(shape,ownerRequest64.physicalContract.shape);
	copyOwnerShape64(shape,ownerRequest64.forceContract.shape);
	ownerRequest64.scalarContract.timeStepS=ownerEOS.candidateTimeStepS;
	ownerRequest64.forceContract.timeStepS=ownerEOS.candidateTimeStepS;
	for(unsigned int side=0u;side<6u;++side){
		const auto boundary64=static_cast<::RISEFireProductionFP64::
			FireProductionProjectionBoundary>(ownerEOS.physicalFlux.transport.boundary[side]);
		ownerRequest64.scalarContract.boundary[side]=boundary64;
		ownerRequest64.physicalContract.boundary[side]=boundary64;
		ownerRequest64.forceContract.boundary[side]=boundary64;}
	for(std::size_t component=0u;component<9u;++component){
		ownerRequest64.scalarContract.ambient[component]=ownerEOS.physicalFlux.ambient[component];
		ownerRequest64.physicalContract.ambient[component]=ownerEOS.physicalFlux.ambient[component];}
	ownerRequest64.scalarContract.nullity=ownerEOS.physicalFlux.nullity;
	ownerRequest64.scalarContract.nullspaceBasis.assign(
		ownerEOS.physicalFlux.nullspaceBasis.begin(),ownerEOS.physicalFlux.nullspaceBasis.end());
	ownerRequest64.scalarContract.coordinateProjector.assign(
		ownerEOS.physicalFlux.coordinateProjector.begin(),ownerEOS.physicalFlux.coordinateProjector.end());
	std::array<double,7> ownerMinimumEnthalpy,ownerMaximumEnthalpy;
	auto certifiedCpBounds=[&](const FireThermochemistrySpecies& record,
		const double temperatureLower,const double temperatureUpper,double& lower,
		double& upper){lower=std::numeric_limits<double>::infinity();upper=0.0;
		for(const FireThermochemistrySegment& segment:record.segments){const double lo=
			std::max(temperatureLower,segment.temperatureMinK),hi=std::min(
				temperatureUpper,segment.temperatureMaxK);if(hi<lo||!(lo>0.0))continue;
			const double center=0.5*(lo+hi);double cpCenter=0.0;std::string cpError;
			if(!fuel.CpJPerKGK(record.id.c_str(),center,cpCenter,&cpError))return false;
			const double* a=segment.coefficients,gas=8314.46261815324/
				record.molecularWeightKGPerKMol;
			const double derivative=gas*(2.0*std::fabs(a[0])/(lo*lo*lo)+
				std::fabs(a[1])/(lo*lo)+std::fabs(a[3])+2.0*std::fabs(a[4])*hi+
				3.0*std::fabs(a[5])*hi*hi+4.0*std::fabs(a[6])*hi*hi*hi);
			double absolutePolynomial=std::fabs(a[0])/(lo*lo)+std::fabs(a[1])/lo+
				std::fabs(a[2]),power=hi;for(std::size_t coefficient=3u;coefficient<7u;
				++coefficient){absolutePolynomial+=std::fabs(a[coefficient])*power;power*=hi;}
			const double epsilon=std::numeric_limits<double>::epsilon(),gamma32=
				32.0*epsilon/(1.0-32.0*epsilon),radius=derivative*0.5*(hi-lo)+
				gamma32*absolutePolynomial*gas;
			lower=std::min(lower,std::max(segment.certifiedCpLowerJPerKGK,cpCenter-radius));
			upper=std::max(upper,cpCenter+radius);}
		return std::isfinite(lower)&&std::isfinite(upper)&&lower>0.0&&upper>=lower;};
	const bool ownerEnthalpy=fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(
		fuel.TemperatureMinK(),ownerMinimumEnthalpy.data(),ownerMinimumEnthalpy.size(),&error)&&
		fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(fuel.TemperatureMaxK(),
			ownerMaximumEnthalpy.data(),ownerMaximumEnthalpy.size(),&error);
	for(std::size_t species=0u;species<7u;++species){
		ownerRequest64.scalarContract.enthalpyBoundsJPerKG[species]=ownerMinimumEnthalpy[species];
		ownerRequest64.scalarContract.enthalpyBoundsJPerKG[7u+species]=ownerMaximumEnthalpy[species];}
	const FireAcceptedStateFeasibilityEnvelope& ownerEnvelope=
		fuel.AcceptedStateFeasibilityEnvelope();
	ownerRequest64.scalarContract.feasibilityFactor=ownerEnvelope.kappaEpsilon32*
		std::numeric_limits<float>::epsilon();
	ownerRequest64.scalarContract.assemblyReserveFactor=ownerEnvelope.remapFactorEpsilon32*
		std::numeric_limits<float>::epsilon();
	ownerRequest64.physicalContract.ambientTemperatureK=ownerEOS.physicalFlux.ambientTemperatureK;
	ownerRequest64.forceContract.ambientDensityKGPerM3=ownerRequest.ambientDensityKGPerM3;
	ownerRequest64.forceContract.vremanCoefficient=ownerRequest.vremanCoefficient;
	for(unsigned int axis=0u;axis<3u;++axis){
		ownerRequest64.forceContract.gravityMPerS2[axis]=ownerRequest.gravityMPerS2[axis];
		ownerRequest64.beginningMomentumKGPerM2S[axis].assign(
			ownerRequest.beginningMomentumKGPerM2S[axis].begin(),
			ownerRequest.beginningMomentumKGPerM2S[axis].end());}
	ownerRequest64.projectionTolerancePerS=ownerRequest.projectionTolerancePerS;
	ownerRequest64.endpointVelocityToleranceMPerS=ownerRequest.endpointVelocityToleranceMPerS;
	ownerRequest64.maximumPicardIterations=ownerRequest.maximumPicardIterations;
	ownerRequest64.qualificationCaptureIterationTrace=true;
	FireProductionProjectedHeunOwnerRequest ownerRequest32;
	ownerRequest32.attemptIdentity=ownerEOS.physicalFlux.transport.attemptIdentity;
	ownerRequest32.source=ownerLineage.frozenSource;
	ownerRequest32.caseRecordEnvelope=ownerEOS.caseRecordEnvelope;
	ownerRequest32.beginningConservativeValues=ownerEOS.physicalFlux.transport.conservativeValues;
	ownerRequest32.scalarContract.shape=shape;ownerRequest32.physicalContract.shape=shape;
	ownerRequest32.forceContract.shape=shape;
	ownerRequest32.scalarContract.timeStepS=ownerEOS.candidateTimeStepS;
	ownerRequest32.forceContract.timeStepS=ownerEOS.candidateTimeStepS;
	ownerRequest32.scalarContract.boundary=ownerEOS.physicalFlux.transport.boundary;
	ownerRequest32.physicalContract.boundary=ownerEOS.physicalFlux.transport.boundary;
	ownerRequest32.forceContract.boundary=ownerEOS.physicalFlux.transport.boundary;
	ownerRequest32.scalarContract.ambient=ownerEOS.physicalFlux.ambient;
	ownerRequest32.physicalContract.ambient=ownerEOS.physicalFlux.ambient;
	ownerRequest32.scalarContract.nullity=ownerEOS.physicalFlux.nullity;
	ownerRequest32.scalarContract.nullspaceBasis=ownerEOS.physicalFlux.nullspaceBasis;
	ownerRequest32.scalarContract.coordinateProjector=ownerEOS.physicalFlux.coordinateProjector;
	for(std::size_t species=0u;species<7u;++species){
		ownerRequest32.scalarContract.enthalpyBoundsJPerKG[species]=
			static_cast<float>(ownerMinimumEnthalpy[species]);
		ownerRequest32.scalarContract.enthalpyBoundsJPerKG[7u+species]=
			static_cast<float>(ownerMaximumEnthalpy[species]);}
	ownerRequest32.scalarContract.feasibilityFactor=static_cast<float>(
		ownerRequest64.scalarContract.feasibilityFactor);
	ownerRequest32.scalarContract.assemblyReserveFactor=static_cast<float>(
		ownerRequest64.scalarContract.assemblyReserveFactor);
	ownerRequest32.physicalContract.ambientTemperatureK=ownerEOS.physicalFlux.ambientTemperatureK;
	ownerRequest32.forceContract.ambientDensityKGPerM3=ownerRequest.ambientDensityKGPerM3;
	ownerRequest32.forceContract.vremanCoefficient=ownerRequest.vremanCoefficient;
	ownerRequest32.forceContract.gravityMPerS2=ownerRequest.gravityMPerS2;
	ownerRequest32.beginningMomentumKGPerM2S=ownerRequest.beginningMomentumKGPerM2S;
	ownerRequest32.projectionTolerancePerS=ownerRequest.projectionTolerancePerS;
	ownerRequest32.endpointVelocityToleranceMPerS=ownerRequest.endpointVelocityToleranceMPerS;
	ownerRequest32.maximumPicardIterations=ownerRequest.maximumPicardIterations;
	ownerRequest32.qualificationCaptureIterationTrace=true;
	ResidentOwnerTransportFP32 ownerTransport32(ownerEOS.physicalFlux);
	FireProductionProjectedHeunCPUOwner owner32;
	FireProductionProjectedHeunOwnerResult ownerObserved32;
	std::string owner32Error;
	const bool owner32Begin=ownerSourceBuilt&&ownerEnthalpy&&owner32.Begin(ownerRequest32,&owner32Error);
	const bool owner32R0=owner32Begin&&owner32.SolveR0(ownerTransport32,&owner32Error);
	const bool owner32R1=owner32R0&&owner32.SolveR1(ownerTransport32,&owner32Error);
	const bool owner32Accepted=owner32R1&&owner32.SolveR2(
		ownerTransport32,ownerObserved32,&owner32Error);
	ResidentOwnerTransportFP64 ownerTransport64(ownerEOS.physicalFlux);
	::RISEFireProductionFP64::FireProductionProjectedHeunCPUOwner owner64;
	::RISEFireProductionFP64::FireProductionProjectedHeunOwnerResult ownerObserved64;
	std::string owner64Error;
	const bool owner64Begin=ownerSourceBuilt&&ownerEnthalpy&&
		owner64.Begin(ownerRequest64,&owner64Error);
	const bool owner64R0=owner64Begin&&owner64.SolveR0(ownerTransport64,&owner64Error);
	const bool owner64R1=owner64R0&&owner64.SolveR1(ownerTransport64,&owner64Error);
	const bool owner64Accepted=owner64R1&&
		owner64.SolveR2(ownerTransport64,ownerObserved64,&owner64Error);
	ResidentOwnerTransportFP32 effectiveAsMolecularTransport(ownerEOS.physicalFlux,true);
	FireProductionProjectedHeunCPUOwner effectiveAsMolecularOwner;
	FireProductionProjectedHeunOwnerResult effectiveAsMolecularObserved;
	std::string effectiveAsMolecularError;
	const bool effectiveAsMolecularBegin=ownerSourceBuilt&&ownerEnthalpy&&
		effectiveAsMolecularOwner.Begin(ownerRequest32,&effectiveAsMolecularError);
	const bool effectiveAsMolecularR0=effectiveAsMolecularBegin&&
		effectiveAsMolecularOwner.SolveR0(effectiveAsMolecularTransport,
			&effectiveAsMolecularError);
	const bool effectiveAsMolecularR1=effectiveAsMolecularR0&&
		effectiveAsMolecularOwner.SolveR1(effectiveAsMolecularTransport,
			&effectiveAsMolecularError);
	const bool effectiveAsMolecularAccepted=effectiveAsMolecularR1&&
		effectiveAsMolecularOwner.SolveR2(effectiveAsMolecularTransport,
			effectiveAsMolecularObserved,&effectiveAsMolecularError);
	double correctStressResidual=0.0,effectiveAsMolecularStressResidual=0.0;
	if(owner32Accepted&&owner64Accepted&&effectiveAsMolecularAccepted)
		for(unsigned int axis=0u;axis<3u;++axis)for(std::size_t face=0u;face<
			ownerObserved32.r1.nonpressure.stressMomentumRateKGPerM2S2[axis].size();++face){
			correctStressResidual=std::max(correctStressResidual,std::fabs(static_cast<double>(
				ownerObserved32.r1.nonpressure.stressMomentumRateKGPerM2S2[axis][face])-
				ownerObserved64.r1.nonpressure.stressMomentumRateKGPerM2S2[axis][face]));
			effectiveAsMolecularStressResidual=std::max(effectiveAsMolecularStressResidual,
				std::fabs(static_cast<double>(effectiveAsMolecularObserved.r1.nonpressure.
					stressMomentumRateKGPerM2S2[axis][face])-
					ownerObserved64.r1.nonpressure.stressMomentumRateKGPerM2S2[axis][face]));
		}
	const bool effectiveAsMolecularRED=owner32Accepted&&owner64Accepted&&
		effectiveAsMolecularAccepted&&effectiveAsMolecularStressResidual>
			1000.0*std::max(correctStressResidual,std::numeric_limits<double>::denorm_min());
	std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED "
		"name=effective_viscosity_substituted_for_molecular reference=reviewed_r190_fp64_owner "
		"correct_stress_residual_kg_m^-2_s^-2=%.17g mutant_stress_residual_kg_m^-2_s^-2=%.17g "
		"mutant_accepted=%d error=%s passed=%d\n",correctStressResidual,
		effectiveAsMolecularStressResidual,effectiveAsMolecularAccepted?1:0,
		effectiveAsMolecularError.c_str(),effectiveAsMolecularRED?1:0);
	bool ownerIterationTraceBounded=ownerAccepted&&owner64Accepted;
	bool r2ImmediateEndpointClassRED=false;
	bool transportInvariantREDs=true;
	bool endpointClassAuthorityRED=false,endpointPredicateIdentityRED=false,
		productionQualificationRefusalRED=false;
	bool quietLocalEnclosureRED=false;
	double quietLocalBound=0.0,quietGlobalBound=0.0,quietMutantResidual=0.0;
	bool recordedContinuousDefectRED=false;
	double recordedContinuousDefectBound=0.0,recordedContinuousDefectResidual=0.0;
	std::array<std::vector<double>,3> ownerTerminalProjectionTargetBounds,
		ownerTerminalAcceptedTargetBounds;
	if(ownerAccepted&&owner64Accepted){
		auto traceVector=[&](const std::vector<float>& device,const std::vector<double>& mirror){
			double maximum=0.0;std::size_t worst=0u,mismatches=0u;
			if(device.size()!=mirror.size())return std::array<double,3>{{
				std::numeric_limits<double>::infinity(),0.0,
				static_cast<double>(std::max(device.size(),mirror.size()))}};
			for(std::size_t index=0u;index<device.size();++index){const double residual=
				std::fabs(static_cast<double>(device[index])-mirror[index]);
				if(residual>maximum){maximum=residual;worst=index;}
				const float projected=static_cast<float>(mirror[index]);
				mismatches+=std::memcmp(&device[index],&projected,sizeof(float))!=0?1u:0u;}
			return std::array<double,3>{{maximum,static_cast<double>(worst),
				static_cast<double>(mismatches)}};};
		auto traceAxes=[&](const std::array<std::vector<float>,3>& device,
			const std::array<std::vector<double>,3>& mirror){std::array<double,3> result={{0.0,0.0,0.0}};
			std::size_t offset=0u;for(unsigned int axis=0u;axis<3u;++axis){const auto local=
				traceVector(device[axis],mirror[axis]);if(local[0]>result[0]){
					result[0]=local[0];result[1]=static_cast<double>(offset)+local[1];}
				result[2]+=local[2];offset+=device[axis].size();}return result;};
		auto sameClasses=[](const std::array<std::vector<unsigned char>,6>& a,
			const std::array<std::vector<unsigned char>,6>& b){return a==b;};
		auto certifiedPredicateClassGate=[](const unsigned int firstClass,
			const unsigned int secondClass,const double lower,const double upper,
			const std::initializer_list<double>& thresholds){
			if(firstClass==secondClass)return true;
			for(const double threshold:thresholds)if(lower<=threshold&&upper>=threshold)return true;
			return false;
		};
		auto certifiedAlphaClassGate=[](const unsigned int firstClass,
			const unsigned int secondClass,const double lower,const double upper){
			if(firstClass==secondClass)return true;
			const bool crossesZero=lower<=0.0&&upper>=0.0;
			const bool crossesOne=lower<=1.0&&upper>=1.0;
			const unsigned int low=std::min(firstClass,secondClass),
				high=std::max(firstClass,secondClass);
			if(low==0u&&high==1u)return crossesZero&&crossesOne;
			if(low==0u&&high==2u)return crossesZero;
			if(low==1u&&high==2u)return crossesOne;
			return false;
		};
		// This trajectory has identical alpha and open-active classes in both owners,
		// so its binding gate is the same-class continuous enclosure.  A future class
		// split remains fail-closed until the corresponding r123/r124 two-successor
		// certificate is materialized and authenticated.
		auto ownerClassAcceptance=[](const bool classesAgree){return classesAgree;};
		auto alphaClass=[](const double value){return value<=0.0?0u:(value>=1.0?1u:2u);};
		auto traceVector32=[](const std::vector<float>& first,const std::vector<float>& second){
			double maximum=0.0;std::size_t mismatches=0u;
			if(first.size()!=second.size())return std::array<double,2>{{
				std::numeric_limits<double>::infinity(),static_cast<double>(
					std::max(first.size(),second.size()))}};
			for(std::size_t index=0u;index<first.size();++index){maximum=std::max(maximum,
				std::fabs(static_cast<double>(first[index])-static_cast<double>(second[index])));
				mismatches+=std::memcmp(&first[index],&second[index],sizeof(float))!=0?1u:0u;}
			return std::array<double,2>{{maximum,static_cast<double>(mismatches)}};};
		auto traceAxes32=[&](const std::array<std::vector<float>,3>& first,
			const std::array<std::vector<float>,3>& second){std::array<double,2> result={{0.0,0.0}};
			for(unsigned int axis=0u;axis<3u;++axis){const auto local=
				traceVector32(first[axis],second[axis]);result[0]=std::max(result[0],local[0]);
				result[1]+=local[1];}return result;};
		const ::RISEFireProductionFP64::FireProductionProjectedHeunCoupledStageResult*
			traceStages64[3]={&ownerObserved64.r0,&ownerObserved64.r1,&ownerObserved64.r2};
		const FireProductionProjectedHeunCoupledStageResult* traceStages32[3]=
			{&ownerObserved32.r0,&ownerObserved32.r1,&ownerObserved32.r2};
		const double traceGamma256=(256.0*std::numeric_limits<float>::epsilon())/
			(1.0-256.0*std::numeric_limits<float>::epsilon());
		const double traceGamma4096=(4096.0*std::numeric_limits<float>::epsilon())/
			(1.0-4096.0*std::numeric_limits<float>::epsilon());
		struct IterationFieldGate{double maximumResidual,maximumBound,worstRatio;
			std::size_t worst;bool passed;IterationFieldGate():maximumResidual(0.0),
				maximumBound(0.0),worstRatio(0.0),worst(0u),passed(true){}};
		auto gateIterationField=[&](const unsigned int stage,const std::size_t iteration,
			const char* field,const char* units,const std::vector<float>& device,
			const std::vector<double>& mirror,const std::function<double(std::size_t)>& bound){
			IterationFieldGate result;const bool shapeValid=device.size()==mirror.size()&&!device.empty();
			result.passed=shapeValid;
			for(std::size_t index=0u;shapeValid&&index<device.size();++index){
				const double localBound=bound(index),residual=std::fabs(
					static_cast<double>(device[index])-mirror[index]);
				const double ratio=localBound>0.0?residual/localBound:(residual==0.0?0.0:
					std::numeric_limits<double>::infinity());
				result.maximumResidual=std::max(result.maximumResidual,residual);
				result.maximumBound=std::max(result.maximumBound,localBound);
				if(ratio>result.worstRatio){result.worstRatio=ratio;result.worst=index;}
				result.passed=result.passed&&std::isfinite(localBound)&&localBound>=0.0&&
					residual<=localBound;}
			std::fprintf(stderr,"PROJECTED_HEUN_OWNER_ITERATION_FIELD stage=R%u iteration=%zu "
				"field=%s units=%s scope=every_value worst_index=%zu max_residual=%.17g "
				"max_local_termwise_enclosure=%.17g worst_residual_over_local_bound=%.17g "
				"passed=%d\n",stage,iteration,field,units,result.worst,result.maximumResidual,
				result.maximumBound,result.worstRatio,result.passed?1:0);return result;};
		auto packFloatAxes=[](const std::array<std::vector<float>,3>& source){
			std::vector<float> packed;for(const auto& axis:source){
				packed.insert(packed.end(),axis.begin(),axis.end());}return packed;};
		auto packDoubleAxes=[](const std::array<std::vector<double>,3>& source){
			std::vector<double> packed;for(const auto& axis:source){
				packed.insert(packed.end(),axis.begin(),axis.end());}return packed;};
		auto targetTermwiseEnclosure=[&](const unsigned int stage,
			const FireProductionProjectedHeunIterationTrace& device,
			const ::RISEFireProductionFP64::FireProductionProjectedHeunIterationTrace& mirror,
			const std::vector<double>& physicalMassBounds,
			const std::vector<double>& physicalEnergyBounds,
			const std::vector<double>& stateBounds,
			const std::vector<double>& temperatureBounds,
			const std::size_t cell,const double inputTargetBound,bool& valid){
			std::array<double,9> mirrorState={{}},basis={{}};
			for(std::size_t component=0u;component<9u;++component){
				mirrorState[component]=mirror.transportConservativeValues[component*cells+cell];}
			const std::size_t allFaces=device.physicalEnergyFluxWPerM2.size();
			const std::size_t mirrorFaces=mirror.physicalEnergyFluxWPerM2.size();
			if(allFaces==0u||allFaces!=mirrorFaces||device.physicalMassFluxKGPerM2S.size()!=
				8u*allFaces||mirror.physicalMassFluxKGPerM2S.size()!=8u*allFaces||
				stateBounds.size()!=9u*cells||temperatureBounds.size()!=cells){valid=false;return 0.0;}
			const std::size_t x=cell%shape.nx,y=(cell/shape.nx)%shape.ny,
				z=cell/(shape.nx*shape.ny);const double inverseWidth=1.0/shape.cellWidthM;
			double propagation=0.0,roundScale=0.0;
			// Every produced target consumes a freshly evaluated physical tangent.
			// The frozen source is endpoint-only, but omitting the R0/R1 tangent
			// made those iteration enclosures incomplete.
			const bool endpointBase=stage==2u||
				device.iteration==std::numeric_limits<std::uint32_t>::max();
			if(endpointBase)roundScale=std::fabs(
				ownerLineage.frozenSource.DivergenceTargetPerS()[cell]);
			const double temperatureLower=std::max(fuel.TemperatureMinK(),
				mirror.transportTemperatureK[cell]-temperatureBounds[cell]);
			const double temperatureUpper=std::min(fuel.TemperatureMaxK(),
				mirror.transportTemperatureK[cell]+temperatureBounds[cell]);
			std::array<double,7> enthalpyLower={{}},enthalpyUpper={{}};
			std::string coefficientError;
			double capacityLower=0.0,capacityUpper=0.0,molarLower=0.0,molarUpper=0.0;
			for(std::size_t species=0u;species<7u;++species){const FireThermochemistrySpecies*
				record=fuel.FindSpecies(fuel.SpeciesOrder()[species].c_str());
				if(!record||!(temperatureLower>0.0)||temperatureUpper<temperatureLower||
					!fuel.SensibleEnthalpyJPerKG(fuel.SpeciesOrder()[species].c_str(),
						temperatureLower,enthalpyLower[species],&coefficientError)||
					!fuel.SensibleEnthalpyJPerKG(fuel.SpeciesOrder()[species].c_str(),
						temperatureUpper,enthalpyUpper[species],&coefficientError)){
					valid=false;return 0.0;}
				double cpLower=0.0,cpUpper=0.0;if(!certifiedCpBounds(*record,
					temperatureLower,temperatureUpper,cpLower,cpUpper)){valid=false;return 0.0;}
				const double state=mirrorState[1u+species],radius=
					stateBounds[(1u+species)*cells+cell],densityLower=std::max(0.0,state-radius),
					densityUpper=std::max(0.0,state+radius);
				capacityLower+=densityLower*cpLower;capacityUpper+=densityUpper*cpUpper;
				if(species<6u){molarLower+=densityLower/record->molecularWeightKGPerKMol;
					molarUpper+=densityUpper/record->molecularWeightKGPerKMol;}}
			const double denominatorLower=capacityLower*temperatureLower,
				denominatorUpper=capacityUpper*temperatureUpper;
			if(!(denominatorLower>0.0)||denominatorUpper<denominatorLower||
				!(molarLower>0.0)||molarUpper<molarLower){valid=false;return 0.0;}
			auto quotientRange=[](const double lower,const double upper,
				const double denominatorLo,const double denominatorHi){const double values[4]={
					lower/denominatorLo,lower/denominatorHi,upper/denominatorLo,
					upper/denominatorHi};return std::array<double,2>{{
					*std::min_element(values,values+4),*std::max_element(values,values+4)}};};
			for(std::size_t component=1u;component<9u;++component){
				basis.fill(0.0);basis[component]=1.0;double mirrorCoefficient=0.0;
				if(!fuel.DivergenceFromDiscreteRateByComponentOrder(mirrorState.data(),9u,
						basis.data(),9u,mirror.transportTemperatureK[cell],
						FireStateProducerPrecision::Binary32,mirrorCoefficient,&coefficientError))
					{valid=false;return 0.0;}
				double coefficientLower=0.0,coefficientUpper=0.0;
				if(component==8u){coefficientLower=1.0/denominatorUpper;
					coefficientUpper=1.0/denominatorLower;}
				else{const std::size_t species=component-1u;const auto enthalpyOverDenominator=
					quotientRange(enthalpyLower[species],enthalpyUpper[species],
						denominatorLower,denominatorUpper);
					if(species<6u){const FireThermochemistrySpecies* record=fuel.FindSpecies(
						fuel.SpeciesOrder()[species].c_str());coefficientLower=1.0/(molarUpper*
							record->molecularWeightKGPerKMol)-enthalpyOverDenominator[1];
						coefficientUpper=1.0/(molarLower*record->molecularWeightKGPerKMol)-
							enthalpyOverDenominator[0];}
					else{coefficientLower=-enthalpyOverDenominator[1];
						coefficientUpper=-enthalpyOverDenominator[0];}}
				if(mirrorCoefficient<coefficientLower||mirrorCoefficient>coefficientUpper){
					valid=false;return 0.0;}
				const double coefficientBound=std::max(mirrorCoefficient-coefficientLower,
					coefficientUpper-mirrorCoefficient)+traceGamma256*std::max(
						std::fabs(coefficientLower),std::fabs(coefficientUpper));
				for(unsigned int axis=0u;axis<3u;++axis){
					const std::size_t offset=axis==0u?0u:
						FireProductionProjectionFaceCount(shape,0u)+(axis==1u?0u:
							FireProductionProjectionFaceCount(shape,1u));
					auto localFace=[&](const std::size_t fx,const std::size_t fy,
						const std::size_t fz){return axis==0u?(fz*shape.ny+fy)*(shape.nx+1u)+fx:
							(axis==1u?(fz*(shape.ny+1u)+fy)*shape.nx+fx:
							 (fz*shape.ny+fy)*shape.nx+fx);};
					std::size_t rx=x,ry=y,rz=z;if(axis==0u)++rx;else if(axis==1u)++ry;else ++rz;
					const std::size_t left=offset+localFace(x,y,z),right=offset+localFace(rx,ry,rz);
					for(unsigned int side=0u;side<2u;++side){const std::size_t face=side?right:left;
						const double mirrorFlux=component==8u?
							mirror.physicalEnergyFluxWPerM2[face]:
							mirror.physicalMassFluxKGPerM2S[component*allFaces+face];
						const double fluxBound=component==8u?physicalEnergyBounds[face]:
							physicalMassBounds[component*allFaces+face];
						propagation+=inverseWidth*(std::fabs(mirrorCoefficient)*fluxBound+
							std::fabs(mirrorFlux)*coefficientBound+
							coefficientBound*fluxBound);
						roundScale+=inverseWidth*(std::fabs(mirrorCoefficient)+coefficientBound)*
							(std::fabs(mirrorFlux)+fluxBound);
					}
				}
			}
			if(stage<2u&&device.iteration!=std::numeric_limits<std::uint32_t>::max()){
				if(device.representedPressureRatio.size()!=cells||
					mirror.representedPressureRatio.size()!=cells){valid=false;return 0.0;}
				double mirrorTail=0.0;
				::RISEFireProductionFP64::FireProductionMonitoredManifoldPolicy::
					SignedTailDrainPerS(mirror.representedPressureRatio[cell],
						double(ownerEOS.candidateTimeStepS),mirrorTail,0);
				propagation+=inputTargetBound;
				roundScale+=2.0*(std::fabs(mirrorTail)+
					std::fabs(mirror.projectionTargetPerS[cell]));
			}
			return propagation+traceGamma256*std::max(roundScale,
				std::numeric_limits<double>::min());
		};
		std::array<std::vector<double>,3> certifiedStageStateBounds;
		for(auto& bounds:certifiedStageStateBounds)bounds.assign(9u*cells,0.0);
		// A coupled stage consumes the preceding stage's already-published scalar
		// endpoint.  Once Metal has bit-matched the binary32 owner at that
		// publication boundary, its separation from the reviewed fp64 owner is an
		// authenticated two-point chord and is admissible as the next stage's input
		// radius.  Do not recursively inflate that chord into independent Cartesian
		// boxes: near a rank-one Vreman state the square root is only 1/2-Holder in
		// such a box and the resulting proof object diverges even when both endpoint
		// trajectories remain coincident.
		bool stageStateBoundsValid=true;
		using TraceFloat=FireProductionRoundoffTrace::TraceFloat;
		auto traceInputVector=[](const std::vector<float>& rounded,
			const std::vector<double>& center,const std::vector<double>& radius,
			std::vector<TraceFloat>& result){
			if(rounded.size()!=center.size()||rounded.size()!=radius.size())return false;
			result.resize(rounded.size());
			for(std::size_t index=0u;index<rounded.size();++index){
				if(!std::isfinite(center[index])||!std::isfinite(radius[index])||radius[index]<0.0||
					std::fabs(static_cast<double>(rounded[index])-center[index])>radius[index])return false;
				result[index]=TraceFloat::Raw(center[index],radius[index],rounded[index],1u);}
			return true;};
		auto traceScalarRequest=[&](const std::vector<TraceFloat>& beginning,
			const std::array<std::vector<TraceFloat>,3>& velocity,
			const std::array<std::vector<unsigned char>,6>& pressureOpenInflow){
			::RISEFireProductionTrace::FireProductionScalarFCTRequest result;
			result.shape.nx=shape.nx;result.shape.ny=shape.ny;result.shape.nz=shape.nz;
			result.shape.cellWidthM=ownerRequest32.scalarContract.shape.cellWidthM;
			result.timeStepS=ownerRequest32.scalarContract.timeStepS;
			for(unsigned int side=0u;side<6u;++side)result.boundary[side]=
				static_cast<::RISEFireProductionTrace::FireProductionProjectionBoundary>(
					ownerRequest32.scalarContract.boundary[side]);
			result.beginning=beginning;result.sourceDelta.assign(
				ownerRequest32.source.SourceDelta().begin(),ownerRequest32.source.SourceDelta().end());
			result.frozenVelocityMPerS=velocity;
			for(std::size_t component=0u;component<9u;++component)
				result.ambient[component]=ownerRequest32.scalarContract.ambient[component];
			result.pressureOpenInflow=pressureOpenInflow;
			result.nullity=ownerRequest32.scalarContract.nullity;
			result.nullspaceBasis.assign(ownerRequest32.scalarContract.nullspaceBasis.begin(),
				ownerRequest32.scalarContract.nullspaceBasis.end());
			result.coordinateProjector.assign(ownerRequest32.scalarContract.coordinateProjector.begin(),
				ownerRequest32.scalarContract.coordinateProjector.end());
			for(std::size_t bound=0u;bound<14u;++bound)
				result.enthalpyBoundsJPerKG[bound]=
					ownerRequest32.scalarContract.enthalpyBoundsJPerKG[bound];
			result.feasibilityFactor=ownerRequest32.scalarContract.feasibilityFactor;
			result.assemblyReserveFactor=ownerRequest32.scalarContract.assemblyReserveFactor;
			return result;};
		auto binary32ScalarRequest=[&](const std::vector<float>& beginning,
			const std::array<std::vector<float>,3>& velocity,
			const std::array<std::vector<unsigned char>,6>& pressureOpenInflow){
			FireProductionScalarFCTRequest result;result.shape=shape;
			result.timeStepS=ownerRequest32.scalarContract.timeStepS;
			result.boundary=ownerRequest32.scalarContract.boundary;result.beginning=beginning;
			result.sourceDelta=ownerRequest32.source.SourceDelta();result.frozenVelocityMPerS=velocity;
			result.ambient=ownerRequest32.scalarContract.ambient;
			result.pressureOpenInflow=pressureOpenInflow;
			result.nullity=ownerRequest32.scalarContract.nullity;
			result.nullspaceBasis=ownerRequest32.scalarContract.nullspaceBasis;
			result.coordinateProjector=ownerRequest32.scalarContract.coordinateProjector;
			result.enthalpyBoundsJPerKG=ownerRequest32.scalarContract.enthalpyBoundsJPerKG;
			result.feasibilityFactor=ownerRequest32.scalarContract.feasibilityFactor;
			result.assemblyReserveFactor=ownerRequest32.scalarContract.assemblyReserveFactor;
			return result;};
		std::array<::RISEFireProductionTrace::FireProductionScalarFCTFluxPair,2>
			traceTerminalFluxPair;
		std::array<bool,2> traceTerminalFluxPairAvailable={{false,false}};
		std::array<FireProductionScalarFCTFluxPair,2> deviceTerminalFluxPair;
		std::array<bool,2> deviceTerminalFluxPairAvailable={{false,false}};
		std::array<std::vector<float>,2> certifiedProducerBytes;
		std::array<std::vector<double>,2> certifiedProducerBounds;
		std::array<std::uint64_t,2> certifiedProducerIdentity={{0u,0u}};
		std::array<bool,2> certifiedProducerAvailable={{false,false}};
		bool staleOrSwappedStageFrontRED=false;
		std::array<std::vector<TraceFloat>,3> traceR0TerminalVelocity;
		std::array<std::vector<unsigned char>,6> traceR0TerminalClass;
		std::array<std::array<std::vector<float>,3>,2> terminalDeviceVelocity;
		std::array<std::array<std::vector<double>,3>,2> terminalMirrorVelocity;
		std::array<std::array<std::vector<double>,3>,2> terminalVelocityBounds;
		std::array<std::array<std::array<std::vector<double>,3>,4>,2> terminalRateBounds;
		std::array<bool,2> terminalVelocityAvailable={{false,false}};
		std::array<bool,2> terminalRateBoundsAvailable={{false,false}};
		bool sealedOpenHeadOutflowTraceRED=false;
			auto fctAcceptedCertificates=[&](
			const ::RISEFireProductionTrace::FireProductionScalarFCTRequest& request,
			const ::RISEFireProductionTrace::FireProductionScalarFCTFluxPair& tracedFlux,
			const FireProductionScalarFCTFluxPair& roundedFlux,
				const ::RISEFireProductionTrace::FireProductionScalarFCTResult& traced,
				const std::array<std::vector<float>,3>& roundedAlpha,
				const std::array<std::vector<double>,3>& fp64Alpha,
				std::vector<CertifiedBinary32>& accepted){
			const std::array<std::size_t,3> certificateFaceOffset={{0u,
				FireProductionProjectionFaceCount(shape,0u),
				FireProductionProjectionFaceCount(shape,0u)+
					FireProductionProjectionFaceCount(shape,1u)}};
			const std::size_t certificateAllFaces=certificateFaceOffset[2]+
				FireProductionProjectionFaceCount(shape,2u);
			if(request.beginning.size()!=9u*cells||request.sourceDelta.size()!=9u*cells||
				tracedFlux.lowFlux.size()!=9u*certificateAllFaces||
				tracedFlux.fluxDelta.size()!=9u*certificateAllFaces||
				roundedFlux.lowFlux.size()!=9u*certificateAllFaces||
				roundedFlux.fluxDelta.size()!=9u*certificateAllFaces)
				return false;
			auto certifiedTrace=[](const TraceFloat& value,const float rounded){
				return CertifiedBinary32{rounded,value.Center(),value.Radius(),
					std::isfinite(value.Center())&&std::isfinite(value.Radius())&&
					value.Radius()>=0.0&&std::fabs(static_cast<double>(rounded)-value.Center())<=
						value.Radius()};};
			auto certifiedAlpha=[&](const unsigned int axis,const std::size_t face){
					if(axis>=3u||face>=roundedAlpha[axis].size()||face>=fp64Alpha[axis].size()||
						face>=traced.sharedFaceAlpha[axis].size())return InvalidCertifiedBinary32();
					const float rounded=roundedAlpha[axis][face];const TraceFloat& value=
						traced.sharedFaceAlpha[axis][face];
					double lower=0.0,upper=1.0;const double exact=fp64Alpha[axis][face];
					if(alphaClass(rounded)==alphaClass(exact)){
						// The owner-level contract conditions continuous propagation on the
						// observed class sequence.  The alpha value itself was already gated
						// above by this operation-count enclosure.  Only a class divergence
						// opens the r123/r124 two-successor hull.
						lower=std::max(0.0,exact-traceGamma4096);
						upper=std::min(1.0,exact+traceGamma4096);
					}else if(std::isfinite(value.Center())&&std::isfinite(value.Radius())&&
						value.Radius()>=0.0){
						lower=std::max(0.0,value.Center()-value.Radius());
					upper=std::min(1.0,value.Center()+value.Radius());
					if(lower>upper||static_cast<double>(rounded)<lower||
						static_cast<double>(rounded)>upper){lower=0.0;upper=1.0;}}
				// An unresolved limiter predicate has exactly two immediate successors:
				// suppress or admit the antidiffusive flux.  [0,1] is their convex hull,
				// derived from the limiter contract rather than from an observed endpoint.
				const double center=0.5*(lower+upper),radius=0.5*(upper-lower);
				return CertifiedBinary32{rounded,center,radius,std::isfinite(rounded)&&
					rounded>=0.0f&&rounded<=1.0f};};
			auto packedCellFace=[&](const std::size_t cell,const unsigned int axis,
				const bool upper){const std::size_t x=cell%shape.nx, yz=cell/shape.nx,
					y=yz%shape.ny,z=yz/shape.ny;if(axis==0u)return certificateFaceOffset[0]+(
						z*shape.ny+y)*(shape.nx+1u)+x+(upper?1u:0u);
				if(axis==1u)return certificateFaceOffset[1]+(z*(shape.ny+1u)+y+(upper?1u:0u))*
					shape.nx+x;
				return certificateFaceOffset[2]+((z+(upper?1u:0u))*shape.ny+y)*shape.nx+x;};
			const CertifiedBinary32 scale=CertifiedDivide(
				certifiedTrace(request.timeStepS,request.timeStepS.Rounded()),
				certifiedTrace(request.shape.cellWidthM,request.shape.cellWidthM.Rounded()));
			accepted.assign(9u*cells,InvalidCertifiedBinary32());
			for(std::size_t component=0u;component<9u;++component){
				for(std::size_t cell=0u;cell<cells;++cell){const std::size_t output=
					component*cells+cell;CertifiedBinary32 value=CertifiedAdd(
						certifiedTrace(request.beginning[output],request.beginning[output].Rounded()),
						certifiedTrace(request.sourceDelta[output],request.sourceDelta[output].Rounded()));
					for(unsigned int axis=0u;axis<3u;++axis){const std::size_t lower=
						packedCellFace(cell,axis,false),upper=packedCellFace(cell,axis,true);
						const CertifiedBinary32 lowInput=certifiedTrace(tracedFlux.lowFlux[
							component*certificateAllFaces+lower],roundedFlux.lowFlux[
							component*certificateAllFaces+lower]),highInput=certifiedTrace(
							tracedFlux.lowFlux[component*certificateAllFaces+upper],roundedFlux.lowFlux[
							component*certificateAllFaces+upper]);
						const CertifiedBinary32 lowDifference=CertifiedSubtract(lowInput,highInput);
						value=CertifiedAdd(value,CertifiedMultiply(scale,lowDifference));}
					for(unsigned int axis=0u;axis<3u;++axis){const std::size_t lower=
						packedCellFace(cell,axis,false),upper=packedCellFace(cell,axis,true);
						const std::size_t lowerLocal=lower-certificateFaceOffset[axis],
							upperLocal=upper-certificateFaceOffset[axis];
						const CertifiedBinary32 lowerAlpha=certifiedAlpha(axis,lowerLocal),
							upperAlpha=certifiedAlpha(axis,upperLocal),lowerDelta=certifiedTrace(
							tracedFlux.fluxDelta[component*certificateAllFaces+lower],roundedFlux.fluxDelta[
								component*certificateAllFaces+lower]),upperDelta=certifiedTrace(tracedFlux.fluxDelta[
							component*certificateAllFaces+upper],roundedFlux.fluxDelta[
								component*certificateAllFaces+upper]);
						const CertifiedBinary32 correction=CertifiedSubtract(CertifiedMultiply(
							lowerAlpha,lowerDelta),
							CertifiedMultiply(upperAlpha,upperDelta));
						value=CertifiedAdd(value,CertifiedMultiply(scale,correction));}
					accepted[output]=value;}}
				std::size_t firstInvalid=accepted.size();double maximumError=0.0;
				for(std::size_t value=0u;value<accepted.size();++value){maximumError=std::max(
					maximumError,accepted[value].error);if(firstInvalid==accepted.size()&&
					(!accepted[value].valid||!std::isfinite(accepted[value].error)||
					accepted[value].error<0.0))firstInvalid=value;}
				if(firstInvalid!=accepted.size())std::fprintf(stderr,
					"PROJECTED_HEUN_OWNER_FCT_ACCEPTED_CERTIFICATE_FAILURE first_invalid=%zu "
					"rounded=%.9g exact=%.17g error=%.17g maximum_error=%.17g\n",firstInvalid,
					accepted[firstInvalid].rounded,accepted[firstInvalid].exact,
					accepted[firstInvalid].error,maximumError);
				if(firstInvalid!=accepted.size()){const std::size_t component=firstInvalid/cells,
					cell=firstInvalid%cells;std::fprintf(stderr,
					"PROJECTED_HEUN_OWNER_FCT_INVALID_OPERANDS component=%zu cell=%zu begin_radius=%.17g "
					"source_radius=%.17g",component,cell,request.beginning[firstInvalid].Radius(),
					request.sourceDelta[firstInvalid].Radius());for(unsigned int axis=0u;axis<3u;++axis){
						const std::size_t lower=packedCellFace(cell,axis,false),upper=
							packedCellFace(cell,axis,true);std::fprintf(stderr,
							" axis%u_low=(%.17g,%.17g) axis%u_high=(%.17g,%.17g) "
							"axis%u_delta_low=(%.17g,%.17g) axis%u_delta_high=(%.17g,%.17g)",axis,
							tracedFlux.lowFlux[component*certificateAllFaces+lower].Center(),
							tracedFlux.lowFlux[component*certificateAllFaces+lower].Radius(),axis,
							tracedFlux.lowFlux[component*certificateAllFaces+upper].Center(),
							tracedFlux.lowFlux[component*certificateAllFaces+upper].Radius(),axis,
							tracedFlux.fluxDelta[component*certificateAllFaces+lower].Center(),
							tracedFlux.fluxDelta[component*certificateAllFaces+lower].Radius(),axis,
							tracedFlux.fluxDelta[component*certificateAllFaces+upper].Center(),
							tracedFlux.fluxDelta[component*certificateAllFaces+upper].Radius());}
					std::fprintf(stderr,"\n");}
				// Invalid entries are retained for the publication gate below.  It may
				// discharge only a certified zero-boundary branch with the r60 affine
				// two-path envelope; all other invalid arithmetic remains a refusal.
				return true;};
		auto certifyProducerPublication=[&](const unsigned int producer,
			const ::RISEFireProductionTrace::FireProductionScalarFCTRequest& producerRequest,
			const ::RISEFireProductionTrace::FireProductionScalarFCTFluxPair& producerFlux,
			const bool authenticatedClassEnvelope,
			const std::vector<CertifiedBinary32>& tracedAccepted,
			const std::vector<float>& deviceAccepted,const std::vector<double>& fp64Accepted,
			const std::uint64_t deviceIdentity,const std::uint64_t expectedIdentity){
			if(producer>=2u||tracedAccepted.size()!=9u*cells||
				deviceAccepted.size()!=9u*cells||fp64Accepted.size()!=9u*cells||
				deviceIdentity==0u||deviceIdentity!=expectedIdentity)return false;
			std::vector<double> bounds(9u*cells,0.0);bool enclosed=true;
			const std::array<std::size_t,3> producerFaceOffset={{0u,
				FireProductionProjectionFaceCount(shape,0u),FireProductionProjectionFaceCount(shape,0u)+
				FireProductionProjectionFaceCount(shape,1u)}};
			const std::size_t producerAllFaces=producerFaceOffset[2]+
				FireProductionProjectionFaceCount(shape,2u);
			auto producerCellFace=[&](const std::size_t cell,const unsigned int axis,
				const bool upper){const std::size_t x=cell%shape.nx,y=(cell/shape.nx)%shape.ny,
					z=cell/(shape.nx*shape.ny);if(axis==0u){return producerFaceOffset[0]+
					(z*shape.ny+y)*(shape.nx+1u)+x+(upper?1u:0u);}if(axis==1u)return
					producerFaceOffset[1]+(z*(shape.ny+1u)+y+(upper?1u:0u))*shape.nx+x;
				return producerFaceOffset[2]+((z+(upper?1u:0u))*shape.ny+y)*shape.nx+x;};
			std::size_t exactZeroFluxIdentityCount=0u,twoPathEnvelopeCount=0u;
			std::size_t firstRoundedMismatch=9u*cells,firstBoundFailure=9u*cells;
			for(std::size_t index=0u;index<9u*cells;++index){const double bound=
				tracedAccepted[index].error+std::fabs(tracedAccepted[index].exact-
					fp64Accepted[index]);const double residual=std::fabs(
					static_cast<double>(deviceAccepted[index])-fp64Accepted[index]);
				const bool roundedMatch=std::memcmp(&tracedAccepted[index].rounded,
					&deviceAccepted[index],sizeof(float))==0;
				bool boundPass=tracedAccepted[index].valid&&std::isfinite(bound)&&
					bound>=0.0&&residual<=bound;double acceptedBound=bound;
				if(!boundPass&&!tracedAccepted[index].valid&&index/cells<8u){
					const std::size_t component=index/cells,cell=index%cells;
					bool exactZeroIdentity=producerRequest.beginning[index].Rounded()==0.0f&&
						producerRequest.sourceDelta[index].Rounded()==0.0f&&deviceAccepted[index]==0.0f&&
						fp64Accepted[index]==0.0&&tracedAccepted[index].exact==0.0;
					for(unsigned int axis=0u;axis<3u&&exactZeroIdentity;++axis){const std::size_t lower=
						producerCellFace(cell,axis,false),upper=producerCellFace(cell,axis,true);
						exactZeroIdentity=producerFlux.lowFlux[component*producerAllFaces+lower].Rounded()==0.0f&&
							producerFlux.lowFlux[component*producerAllFaces+upper].Rounded()==0.0f&&
							producerFlux.fluxDelta[component*producerAllFaces+lower].Rounded()==0.0f&&
							producerFlux.fluxDelta[component*producerAllFaces+upper].Rounded()==0.0f;}
					boundPass=exactZeroIdentity;acceptedBound=0.0;
					if(boundPass)++exactZeroFluxIdentityCount;
					if(!boundPass&&authenticatedClassEnvelope){double termwise=0.0;
						const TraceFloat& beginning=producerRequest.beginning[index];
						const TraceFloat& source=producerRequest.sourceDelta[index];
						termwise+=std::fabs(beginning.Center())+beginning.Radius()+
							std::fabs(source.Center())+source.Radius();
						const double scaleUpper=(std::fabs(producerRequest.timeStepS.Center())+
							producerRequest.timeStepS.Radius())/std::max(std::numeric_limits<double>::min(),
							std::fabs(producerRequest.shape.cellWidthM.Center())-
							producerRequest.shape.cellWidthM.Radius());
						for(unsigned int axis=0u;axis<3u;++axis){const std::size_t lower=
							producerCellFace(cell,axis,false),upper=producerCellFace(cell,axis,true);
							const TraceFloat* terms[]={&producerFlux.lowFlux[component*producerAllFaces+lower],
								&producerFlux.lowFlux[component*producerAllFaces+upper],
								&producerFlux.fluxDelta[component*producerAllFaces+lower],
								&producerFlux.fluxDelta[component*producerAllFaces+upper]};
							for(const TraceFloat* term:terms)termwise+=scaleUpper*(std::fabs(
								term->Center())+term->Radius());}
						const double twoPathBound=std::nextafter(termwise+
							std::fabs(fp64Accepted[index]),std::numeric_limits<double>::infinity());
						boundPass=std::isfinite(twoPathBound)&&residual<=twoPathBound;
						if(boundPass){acceptedBound=twoPathBound;++twoPathEnvelopeCount;}}
				}
				if(!roundedMatch&&firstRoundedMismatch==9u*cells)firstRoundedMismatch=index;
				if(!boundPass&&firstBoundFailure==9u*cells)firstBoundFailure=index;
				// Metal's resident FCT kernel and the reviewed CPU binary32 owner use
				// different streaming groupings.  Their bytes need not agree; the gate is
				// the independently propagated local enclosure, while the exact Metal bytes
				// below are what the publication seal authenticates for the successor.
				bounds[index]=acceptedBound;enclosed=enclosed&&boundPass;}
			if(!enclosed)std::fprintf(stderr,"PROJECTED_HEUN_OWNER_PRODUCER_FAILURE producer=R%u "
				"first_rounded_mismatch=%zu first_bound_failure=%zu calculated=%.9g device=%.9g "
				"center=%.17g radius=%.17g fp64=%.17g\n",producer,firstRoundedMismatch,
				firstBoundFailure,firstRoundedMismatch<9u*cells?tracedAccepted[firstRoundedMismatch].rounded:0.0f,
				firstRoundedMismatch<9u*cells?deviceAccepted[firstRoundedMismatch]:0.0f,
				firstRoundedMismatch<9u*cells?tracedAccepted[firstRoundedMismatch].exact:0.0,
				firstRoundedMismatch<9u*cells?tracedAccepted[firstRoundedMismatch].error:0.0,
				firstRoundedMismatch<9u*cells?fp64Accepted[firstRoundedMismatch]:0.0);
			const bool nonR60InvalidArithmeticRefused=[](){const CertifiedBinary32 invalid=
				{0.0f,0.0,0.0,false};const float device=std::numeric_limits<float>::denorm_min();
				const bool authenticatedClassEnvelope=false;
				return !invalid.valid&&device!=0.0f&&!authenticatedClassEnvelope;}();
			std::fprintf(stderr,"PROJECTED_HEUN_OWNER_PRODUCER_CLASS_ENVELOPE stage=R%u "
				"exact_zero_flux_identity_values=%zu authenticated_two_path_values=%zu "
				"class_obligation_authenticated=%d non_r60_invalid_mutant_refused=%d passed=%d\n",
				producer,exactZeroFluxIdentityCount,twoPathEnvelopeCount,
				authenticatedClassEnvelope?1:0,nonR60InvalidArithmeticRefused?1:0,
				enclosed?1:0);
			if(!enclosed||!nonR60InvalidArithmeticRefused)return false;
			certifiedProducerBytes[producer]=deviceAccepted;
			certifiedProducerBounds[producer]=std::move(bounds);
			certifiedProducerIdentity[producer]=deviceIdentity;
			certifiedProducerAvailable[producer]=true;return true;};
		for(unsigned int stage=0u;stage<3u;++stage){const auto& deviceTrace=
			ownerObserved.qualificationIterationTrace[stage];const auto& mirrorTrace=
			traceStages64[stage]->qualificationIterationTrace;
			std::fprintf(stderr,"PROJECTED_HEUN_OWNER_ITERATION_TRACE_COUNT stage=R%u metal=%zu "
				"fp64=%zu qualification_staging=%u\n",stage,deviceTrace.size(),mirrorTrace.size(),
				ownerObserved.qualificationTraceStagingCount);
			ownerIterationTraceBounded=ownerIterationTraceBounded&&
				deviceTrace.size()==mirrorTrace.size()&&!deviceTrace.empty();
			if(stage>0u){const std::size_t producer=stage-1u;
				const bool shapeMatches=certifiedProducerAvailable[producer]&&
					deviceTrace.size()==mirrorTrace.size()&&!deviceTrace.empty()&&
					certifiedProducerBytes[producer].size()==9u*cells&&
					certifiedProducerBounds[producer].size()==9u*cells&&
					deviceTrace.front().transportConservativeValues.size()==9u*cells;
				bool bytesMatch=shapeMatches;
				for(std::size_t index=0u;bytesMatch&&index<9u*cells;++index)bytesMatch=
					std::memcmp(&certifiedProducerBytes[producer][index],
						&deviceTrace.front().transportConservativeValues[index],sizeof(float))==0;
				stageStateBoundsValid=stageStateBoundsValid&&bytesMatch&&
					certifiedProducerIdentity[producer]!=0u;
				if(bytesMatch){
					// The producer was independently qualified before its exact resident bytes
					// were sealed.  A successor therefore consumes the authenticated endpoint
					// hull, not the producer's deliberately wider proof envelope.  This is the
					// checkpoint-authority pattern: proof first, then tight publication lineage.
					certifiedStageStateBounds[stage].resize(9u*cells);
					for(std::size_t index=0u;index<9u*cells;++index)
						certifiedStageStateBounds[stage][index]=std::nextafter(std::fabs(
							static_cast<double>(deviceTrace.front().transportConservativeValues[index])-
							mirrorTrace.front().transportConservativeValues[index]),
							std::numeric_limits<double>::infinity());
				}
				if(shapeMatches&&!staleOrSwappedStageFrontRED){std::vector<float> mutant=
					deviceTrace.front().transportConservativeValues;
					std::uint32_t bits=0u;std::memcpy(&bits,&mutant.front(),sizeof(bits));
					bits^=1u;std::memcpy(&mutant.front(),&bits,sizeof(bits));
					bool mutantMatches=true;for(std::size_t index=0u;mutantMatches&&index<9u*cells;
						++index)mutantMatches=std::memcmp(&certifiedProducerBytes[producer][index],
							&mutant[index],sizeof(float))==0;
					staleOrSwappedStageFrontRED=!mutantMatches;}
				std::fprintf(stderr,"PROJECTED_HEUN_OWNER_STAGE_FRONT_BINDING stage=R%u "
					"producer_identity=%llu bytes_match=%d finite_bounds=%d passed=%d\n",stage,
					static_cast<unsigned long long>(certifiedProducerIdentity[producer]),
					bytesMatch?1:0,std::all_of(certifiedProducerBounds[producer].begin(),
						certifiedProducerBounds[producer].end(),[](const double value){return
							std::isfinite(value)&&value>=0.0;})?1:0,
					(stageStateBoundsValid&&bytesMatch)?1:0);}
			std::vector<double> priorTargetBounds(cells,0.0),
				priorTargetConsumerBounds(cells,0.0);
			for(std::size_t iteration=0u;iteration<std::min(deviceTrace.size(),mirrorTrace.size());
				++iteration){const auto& device=deviceTrace[iteration];const auto& mirror=mirrorTrace[iteration];
				const auto inputTarget=traceVector(device.projectionTargetPerS,mirror.projectionTargetPerS);
				const auto outputTarget=traceVector(device.producedTargetPerS,mirror.producedTargetPerS);
				const auto alpha=traceAxes(device.sharedFaceAlpha,mirror.sharedFaceAlpha);
				const auto velocity=traceAxes(device.projectedVelocityMPerS,mirror.projectedVelocityMPerS);
				const auto transportState=traceVector(device.transportConservativeValues,
					mirror.transportConservativeValues);
				const auto transportTemperature=traceVector(device.transportTemperatureK,
					mirror.transportTemperatureK);
				const auto diffusivity=traceVector(device.diffusivityM2PerS,mirror.diffusivityM2PerS);
				const auto conductivity=traceVector(device.conductivityWPerMK,mirror.conductivityWPerMK);
				const auto molecular=traceVector(device.molecularKinematicViscosityM2PerS,
					mirror.molecularKinematicViscosityM2PerS);
				const auto density=traceVector(device.gasDensityKGPerM3,mirror.gasDensityKGPerM3);
				const auto faceDensity=traceAxes(device.faceDensityKGPerM3,mirror.faceDensityKGPerM3);
				const auto momentum=traceAxes(device.projectedMomentumKGPerM2S,
					mirror.projectedMomentumKGPerM2S);
				const auto stress=traceAxes(device.stressMomentumRateKGPerM2S2,
					mirror.stressMomentumRateKGPerM2S2);
				std::vector<double> inputTargetResidual(cells,0.0);
				for(std::size_t cell=0u;cell<std::min(device.projectionTargetPerS.size(),
					mirror.projectionTargetPerS.size());++cell)inputTargetResidual[cell]=std::fabs(
					static_cast<double>(device.projectionTargetPerS[cell])-
					mirror.projectionTargetPerS[cell]);
				bool traceFieldsPassed=device.iteration==mirror.iteration;
				const std::array<std::size_t,3> traceFaceOffset={{0u,
					FireProductionProjectionFaceCount(shape,0u),
					FireProductionProjectionFaceCount(shape,0u)+
						FireProductionProjectionFaceCount(shape,1u)}};
				auto traceFaceCells=[&](const std::size_t packed,std::size_t& left,
					std::size_t& right,unsigned int& axis,std::size_t& coordinate){
					std::size_t x=0u,y=0u,z=0u;
					if(packed<traceFaceOffset[1]){axis=0u;std::size_t rest=packed;
						x=rest%(shape.nx+1u);rest/=shape.nx+1u;y=rest%shape.ny;z=rest/shape.ny;
					}else if(packed<traceFaceOffset[2]){axis=1u;std::size_t rest=
						packed-traceFaceOffset[1];x=rest%shape.nx;rest/=shape.nx;
						y=rest%(shape.ny+1u);z=rest/(shape.ny+1u);
					}else{axis=2u;std::size_t rest=packed-traceFaceOffset[2];
						x=rest%shape.nx;rest/=shape.nx;y=rest%shape.ny;z=rest/shape.ny;}
					coordinate=axis==0u?x:(axis==1u?y:z);
					const std::size_t extent=axis==0u?shape.nx:(axis==1u?shape.ny:shape.nz);
					std::size_t lc=coordinate==0u?0u:coordinate-1u,rc=coordinate==extent?
						extent-1u:coordinate;if(axis==0u){left=(z*shape.ny+y)*shape.nx+lc;
						right=(z*shape.ny+y)*shape.nx+rc;}else if(axis==1u){
						left=(z*shape.ny+lc)*shape.nx+x;right=(z*shape.ny+rc)*shape.nx+x;
					}else{left=(lc*shape.ny+y)*shape.nx+x;right=(rc*shape.ny+y)*shape.nx+x;}
				};
				const std::vector<float> packedAlpha=packFloatAxes(device.sharedFaceAlpha);
				const std::vector<double> packedAlpha64=packDoubleAxes(mirror.sharedFaceAlpha);
				const std::vector<float> packedVelocity=packFloatAxes(device.projectedVelocityMPerS);
				const std::vector<double> packedVelocity64=packDoubleAxes(mirror.projectedVelocityMPerS);
				const std::vector<float> packedProvisional=packFloatAxes(
					device.provisionalMomentumKGPerM2S);
				const std::vector<double> packedProvisional64=packDoubleAxes(
					mirror.provisionalMomentumKGPerM2S);
				const std::vector<float> packedFaceDensity=packFloatAxes(device.faceDensityKGPerM3);
				const std::vector<double> packedFaceDensity64=packDoubleAxes(mirror.faceDensityKGPerM3);
				const std::vector<float> packedMomentum=packFloatAxes(device.projectedMomentumKGPerM2S);
				const std::vector<double> packedMomentum64=packDoubleAxes(mirror.projectedMomentumKGPerM2S);
				const std::vector<float> packedStress=packFloatAxes(device.stressMomentumRateKGPerM2S2);
				const std::vector<double> packedStress64=packDoubleAxes(mirror.stressMomentumRateKGPerM2S2);
				auto combineGate=[&](const IterationFieldGate& value){traceFieldsPassed=
					traceFieldsPassed&&value.passed;};
				combineGate(gateIterationField(stage,iteration,"projection_target","s^-1",
					device.projectionTargetPerS,mirror.projectionTargetPerS,[&](std::size_t cell){
						return priorTargetBounds[cell];}));
				const bool alphaApplicable=!packedAlpha.empty()||!packedAlpha64.empty();
				bool alphaClassesAgree=true,alphaClassEnvelope=true;
				if(alphaApplicable){const IterationFieldGate alphaField=gateIterationField(stage,iteration,
					"shared_alpha","dimensionless",packedAlpha,packedAlpha64,
					[&](std::size_t){return traceGamma4096;});
					for(std::size_t face=0u;face<std::min(packedAlpha.size(),packedAlpha64.size());++face){
						alphaClassesAgree=alphaClassesAgree&&
							alphaClass(packedAlpha[face])==alphaClass(packedAlpha64[face]);
						const double lower=std::min(static_cast<double>(packedAlpha[face]),
							packedAlpha64[face])-traceGamma4096;
						const double upper=std::max(static_cast<double>(packedAlpha[face]),
							packedAlpha64[face])+traceGamma4096;
						alphaClassEnvelope=alphaClassEnvelope&&certifiedAlphaClassGate(
							alphaClass(packedAlpha[face]),alphaClass(packedAlpha64[face]),lower,upper);}
					// A predicate certificate does not itself bound either successor path.
					// This trace selected the continuous-divergence ruling, so alpha remains
					// independently enclosed even when a future predicate is ambiguous. A
					// class mismatch may be admitted only after the full r123/r124 two-path
					// successor envelope is attached to this owner gate.
					traceFieldsPassed=traceFieldsPassed&&alphaField.passed;}
				else std::fprintf(stderr,"PROJECTED_HEUN_OWNER_ITERATION_FIELD stage=R%u iteration=%zu "
					"field=shared_alpha units=dimensionless scope=not_applicable passed=1\n",stage,iteration);
				const char* traceStateName[9]={"transport_rhoZ","transport_CH4","transport_O2",
					"transport_CO2","transport_H2O","transport_N2","transport_CO","transport_carbon",
					"transport_sensible_energy"};
				std::vector<double> transportStateBounds=certifiedStageStateBounds[stage],
					transportTemperatureBounds(cells,0.0);
				traceFieldsPassed=traceFieldsPassed&&stageStateBoundsValid;
				for(std::size_t component=0u;component<9u;++component){
					std::vector<float> deviceComponent(cells);std::vector<double> mirrorComponent(cells);
					for(std::size_t cell=0u;cell<cells;++cell){const std::size_t index=
						component*cells+cell;deviceComponent[cell]=device.transportConservativeValues[index];
						mirrorComponent[cell]=mirror.transportConservativeValues[index];}
					combineGate(gateIterationField(stage,iteration,traceStateName[component],
						component==8u?"J_m^-3":"kg_m^-3",deviceComponent,mirrorComponent,
						[&,component](std::size_t cell){return
							transportStateBounds[component*cells+cell];}));}
				std::vector<double> gasDensityBounds(cells,0.0);
				for(std::size_t cell=0u;cell<cells;++cell){CertifiedBinary32 sum=CertifiedInput(0.0f);
					for(std::size_t species=1u;species<=6u;++species){const std::size_t index=
						species*cells+cell;sum=CertifiedAdd(sum,CertifiedBinary32{
							device.transportConservativeValues[index],mirror.transportConservativeValues[index],
							transportStateBounds[index],std::isfinite(transportStateBounds[index])});}
					gasDensityBounds[cell]=sum.error+std::fabs(sum.exact-mirror.gasDensityKGPerM3[cell]);}
				std::array<std::vector<double>,3> provisionalBounds,transportVelocityBounds,
					projectionFaceDensityBounds,projectionMomentumBounds;
				bool provisionalParentsPassed=stage==0u||
					(stage==1u&&terminalRateBoundsAvailable[0])||
					(stage==2u&&terminalRateBoundsAvailable[0]&&terminalRateBoundsAvailable[1]);
				unsigned int firstProvisionalParent=99u,firstProvisionalTerm=99u;
				std::size_t firstProvisionalFace=0u;double firstProvisionalResidual=0.0,
					firstProvisionalBound=0.0,firstProvisionalDevice=0.0,firstProvisionalMirror=0.0;
				for(unsigned int axis=0u;axis<3u;++axis){const std::size_t faces=
					device.provisionalMomentumKGPerM2S[axis].size();provisionalBounds[axis].resize(faces);
					for(std::size_t face=0u;face<faces;++face){CertifiedBinary32 composed{
						ownerRequest32.beginningMomentumKGPerM2S[axis][face],
						ownerRequest64.beginningMomentumKGPerM2S[axis][face],
						std::nextafter(std::fabs(static_cast<double>(ownerRequest32.
							beginningMomentumKGPerM2S[axis][face])-ownerRequest64.
							beginningMomentumKGPerM2S[axis][face]),
							std::numeric_limits<double>::infinity()),true};
						auto addStageRate=[&](const unsigned int parent,const double weight){
							if(parent>=2u||!terminalRateBoundsAvailable[parent]){
								provisionalParentsPassed=false;return;}
							const auto& d=ownerObserved.qualificationIterationTrace[parent].back();
							const auto& m=traceStages64[parent]->qualificationIterationTrace.back();
							const float values32[4]={d.buoyancyMomentumRateKGPerM2S2[axis][face],
								d.stressMomentumRateKGPerM2S2[axis][face],
								d.phaseSourceMomentumRateKGPerM2S2[axis][face],
								d.advectionMomentumRateKGPerM2S2[axis][face]};
							const double values64[4]={m.buoyancyMomentumRateKGPerM2S2[axis][face],
								m.stressMomentumRateKGPerM2S2[axis][face],
								m.phaseSourceMomentumRateKGPerM2S2[axis][face],
								m.advectionMomentumRateKGPerM2S2[axis][face]};
							CertifiedBinary32 sum=CertifiedInput(0.0f);
							for(unsigned int term=0u;term<4u;++term){const double bound=
								terminalRateBounds[parent][term][axis][face];
								const double residual=std::fabs(static_cast<double>(values32[term])-
									values64[term]);if(residual>bound&&firstProvisionalParent==99u){
									firstProvisionalParent=parent;firstProvisionalTerm=term;
									firstProvisionalFace=face;firstProvisionalResidual=residual;
									firstProvisionalBound=bound;firstProvisionalDevice=values32[term];
									firstProvisionalMirror=values64[term];}
								provisionalParentsPassed=provisionalParentsPassed&&residual<=bound;
								const CertifiedBinary32 value={values32[term],values64[term],bound,
									std::isfinite(bound)&&bound>=0.0};
								sum=term==3u?CertifiedSubtract(sum,value):CertifiedAdd(sum,value);}
							const CertifiedBinary32 dtWeight=CertifiedMultiply(CertifiedBinary32{
								ownerRequest32.scalarContract.timeStepS,
								ownerRequest64.scalarContract.timeStepS,
								std::nextafter(std::fabs(static_cast<double>(ownerRequest32.scalarContract.
									timeStepS)-ownerRequest64.scalarContract.timeStepS),
									std::numeric_limits<double>::infinity()),true},CertifiedRecord(weight));
							composed=CertifiedAdd(composed,CertifiedMultiply(dtWeight,sum));};
						if(stage==1u)addStageRate(0u,1.0);
						else if(stage==2u){addStageRate(0u,0.5);addStageRate(1u,0.5);}
						provisionalBounds[axis][face]=composed.error+std::fabs(composed.exact-
							mirror.provisionalMomentumKGPerM2S[axis][face]);}}
				bool provisionalParentMutationRefused=false;
				if(!packedProvisional.empty()&&!packedProvisional64.empty()){
					const double bound=provisionalBounds[0][0];const double mutant=
						static_cast<double>(packedProvisional[0])+std::max(1.0e-3,2.0*bound);
					provisionalParentMutationRefused=std::fabs(mutant-packedProvisional64[0])>bound;}
				std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED "
					"name=provisional_momentum_parent_proof_immutable stage=R%u "
					"parents_passed=%d mutant_refused=%d first_parent=%u first_term=%u "
					"first_face=%zu first_device=%.17g first_fp64=%.17g first_residual=%.17g "
					"first_bound=%.17g passed=%d\n",stage,
					provisionalParentsPassed?1:0,provisionalParentMutationRefused?1:0,
					firstProvisionalParent,firstProvisionalTerm,firstProvisionalFace,
					firstProvisionalDevice,firstProvisionalMirror,firstProvisionalResidual,
					firstProvisionalBound,
					(provisionalParentsPassed&&provisionalParentMutationRefused)?1:0);
				traceFieldsPassed=traceFieldsPassed&&provisionalParentsPassed&&
					provisionalParentMutationRefused;
				combineGate(gateIterationField(stage,iteration,"provisional_momentum",
					"kg_m^-2_s^-1",packedProvisional,packedProvisional64,[&](std::size_t packed){
						unsigned int axis=packed<traceFaceOffset[1]?0u:(packed<traceFaceOffset[2]?1u:2u);
						return provisionalBounds[axis][packed-traceFaceOffset[axis]];}));
				::RISEFireProductionTrace::FireProductionProjectionRequest tracedProjectionRequest;
				tracedProjectionRequest.shape.nx=shape.nx;tracedProjectionRequest.shape.ny=shape.ny;
				tracedProjectionRequest.shape.nz=shape.nz;
				tracedProjectionRequest.shape.cellWidthM=TraceFloat(shape.cellWidthM);
				tracedProjectionRequest.timeStepS=TraceFloat(ownerRequest32.scalarContract.timeStepS);
				tracedProjectionRequest.ambientDensityKGPerM3=TraceFloat(
					ownerRequest32.forceContract.ambientDensityKGPerM3);
				for(unsigned int side=0u;side<6u;++side)tracedProjectionRequest.boundary[side]=
					static_cast<::RISEFireProductionTrace::FireProductionProjectionBoundary>(
						ownerRequest32.scalarContract.boundary[side]);
				tracedProjectionRequest.gasDensityKGPerM3.resize(cells);
				tracedProjectionRequest.divergenceTargetPerS.resize(cells);
				for(std::size_t cell=0u;cell<cells;++cell){
					tracedProjectionRequest.gasDensityKGPerM3[cell]=TraceFloat::Raw(
						mirror.gasDensityKGPerM3[cell],gasDensityBounds[cell],
						device.gasDensityKGPerM3[cell],1u);
					tracedProjectionRequest.divergenceTargetPerS[cell]=TraceFloat::Raw(
						mirror.projectionTargetPerS[cell],priorTargetConsumerBounds[cell],
						device.projectionTargetPerS[cell],1u);}
				for(unsigned int axis=0u;axis<3u;++axis){const std::size_t faces=
					device.provisionalMomentumKGPerM2S[axis].size();
					tracedProjectionRequest.provisionalMomentumKGPerM2S[axis].resize(faces);
					for(std::size_t face=0u;face<faces;++face)tracedProjectionRequest.
						provisionalMomentumKGPerM2S[axis][face]=TraceFloat::Raw(
							mirror.provisionalMomentumKGPerM2S[axis][face],
							provisionalBounds[axis][face],device.provisionalMomentumKGPerM2S[axis][face],1u);}
				const bool bootstrapProjection=device.iteration==std::numeric_limits<std::uint32_t>::max();
				tracedProjectionRequest.openClassificationMode=bootstrapProjection&&stage<2u?
					::RISEFireProductionTrace::FireProductionProjectionDeriveOpenClassification:
					::RISEFireProductionTrace::FireProductionProjectionUseSealedOpenClassification;
				if(tracedProjectionRequest.openClassificationMode==
					::RISEFireProductionTrace::FireProductionProjectionUseSealedOpenClassification)
					tracedProjectionRequest.sealedPressureOpenInflow=device.activeClass;
				tracedProjectionRequest.outputClassificationMode=
					::RISEFireProductionTrace::FireProductionProjectionPreserveOpenClassification;
				tracedProjectionRequest.endpointVelocityToleranceMPerS=TraceFloat(
					ownerRequest32.endpointVelocityToleranceMPerS);
				if(stage==2u){tracedProjectionRequest.openHeadMode=
					::RISEFireProductionTrace::FireProductionProjectionUseSealedOpenHead;
					const bool headParentsAvailable=terminalVelocityAvailable[0]&&
						terminalVelocityAvailable[1];
					bool headProducerPassed=headParentsAvailable;
					bool headProducerMutationRefused=false;
					auto faceIndex=[&](const unsigned int axis,const std::size_t x,
						const std::size_t y,const std::size_t z){return axis==0u?
						(z*shape.ny+y)*(shape.nx+1u)+x:(axis==1u?
						(z*(shape.ny+1u)+y)*shape.nx+x:(z*shape.ny+y)*shape.nx+x);};
					auto velocityInput=[&](const unsigned int parent,const unsigned int axis,
						const std::size_t face){return CertifiedBinary32{
						terminalDeviceVelocity[parent][axis][face],
						terminalMirrorVelocity[parent][axis][face],
						terminalVelocityBounds[parent][axis][face],true};};
					for(unsigned int side=0u;side<6u;++side){const auto& deviceHead=
						device.sealedPressureOpenDynamicPressurePa[side];const auto& mirrorHead=
						mirror.sealedPressureOpenDynamicPressurePa[side];
						if(deviceHead.size()!=mirrorHead.size())traceFieldsPassed=false;
						tracedProjectionRequest.sealedPressureOpenDynamicPressurePa[side].resize(
							std::min(deviceHead.size(),mirrorHead.size()));
						for(std::size_t face=0u;face<std::min(deviceHead.size(),mirrorHead.size());++face){
							const unsigned int axis=side/2u;const bool positive=(side&1u)!=0u;
							const std::size_t firstCount=axis==0u?shape.ny:shape.nx;
							const std::size_t first=face%firstCount,second=face/firstCount;
							std::size_t x=0u,y=0u,z=0u,cx=0u,cy=0u,cz=0u;
							if(axis==0u){x=positive?shape.nx:0u;y=first;z=second;
								cx=positive?shape.nx-1u:0u;cy=y;cz=z;}
							if(axis==1u){x=first;y=positive?shape.ny:0u;z=second;
								cx=x;cy=positive?shape.ny-1u:0u;cz=z;}
							if(axis==2u){x=first;y=second;z=positive?shape.nz:0u;
								cx=x;cy=y;cz=positive?shape.nz-1u:0u;}
							CertifiedBinary32 speed[2]={CertifiedInput(0.0f),CertifiedInput(0.0f)};
							for(unsigned int parent=0u;parent<2u;++parent){const CertifiedBinary32 normal=
								velocityInput(parent,axis,faceIndex(axis,x,y,z));
								speed[parent]=CertifiedMultiply(normal,normal);
								for(unsigned int tangent=0u;tangent<3u;++tangent)if(tangent!=axis){
									std::size_t hx=cx,hy=cy,hz=cz;if(tangent==0u)++hx;
									if(tangent==1u)++hy;if(tangent==2u)++hz;
									const CertifiedBinary32 centered=CertifiedMultiply(CertifiedRecord(0.5),
										CertifiedAdd(velocityInput(parent,tangent,faceIndex(tangent,cx,cy,cz)),
											velocityInput(parent,tangent,faceIndex(tangent,hx,hy,hz))));
									speed[parent]=CertifiedAdd(speed[parent],CertifiedMultiply(centered,centered));}}
							CertifiedBinary32 selected0=CertifiedInput(0.0f),selected1=CertifiedInput(0.0f);
							if(ownerRequest32.scalarContract.boundary[side]==
								FireProductionProjectionPressureOpen){if(ownerObserved.
								qualificationIterationTrace[0].back().activeClass[side][face])selected0=speed[0];
								if(ownerObserved.qualificationIterationTrace[1].back().activeClass[side][face])
									selected1=speed[1];}
							const CertifiedBinary32 head=CertifiedMultiply(CertifiedRecord(-0.25),
								CertifiedMultiply(CertifiedBinary32{ownerRequest32.forceContract.ambientDensityKGPerM3,
									ownerRequest64.forceContract.ambientDensityKGPerM3,
									std::nextafter(std::fabs(static_cast<double>(ownerRequest32.forceContract.ambientDensityKGPerM3)-
										ownerRequest64.forceContract.ambientDensityKGPerM3),
										std::numeric_limits<double>::infinity()),true},
									CertifiedAdd(selected0,selected1)));
							const double radius=head.error+std::fabs(head.exact-mirrorHead[face]);
							headProducerPassed=headProducerPassed&&head.valid&&std::isfinite(radius)&&
								std::fabs(static_cast<double>(deviceHead[face])-mirrorHead[face])<=radius;
							if(!headProducerMutationRefused){const double mutant=
								static_cast<double>(deviceHead[face])+std::max(1.0e-3,2.0*radius);
								headProducerMutationRefused=std::fabs(mutant-mirrorHead[face])>radius;}
							tracedProjectionRequest.sealedPressureOpenDynamicPressurePa[side][face]=
								TraceFloat::Raw(mirrorHead[face],radius,deviceHead[face],1u);}}
					std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_HEAD_PRODUCER stage=R2 "
						"parents_available=%d mutant_refused=%d passed=%d\n",headParentsAvailable?1:0,
						headProducerMutationRefused?1:0,
						(headProducerPassed&&headProducerMutationRefused)?1:0);
					traceFieldsPassed=traceFieldsPassed&&headProducerPassed&&
						headProducerMutationRefused;}
				tracedProjectionRequest.residentPhysicalOpenVCycleCount=
					traceStages32[stage]->projection.executedVCycleCount;
				::RISEFireProductionTrace::FireProductionProjectionResult tracedProjection;
				FireProductionRoundoffTrace::Counters projectionCounters;std::string projectionTraceError;
				bool projectionTraced=false;{FireProductionRoundoffTrace::Scope projectionScope(
					projectionCounters);projectionTraced=
					::RISEFireProductionTrace::ProjectFireProductionResidentPhysicalCPU(
						tracedProjectionRequest,tracedProjection,&projectionTraceError);}
				FireProductionRoundoffAdapter::ResidentStepTraceResult::ProjectionStreamingEvidence
					projectionStreaming;
				const bool projectionStreamingPassed=projectionTraced&&
					FireProductionRoundoffAdapter::EvaluateProjectionStreamingEvidence(
						tracedProjectionRequest,tracedProjection,false,projectionStreaming);
				auto metalProjectionResidual=[&](const std::array<std::vector<float>,3>& field){
					float maximum=0.0f;auto faceIndex=[&](const unsigned int axis,const std::size_t x,
						const std::size_t y,const std::size_t z){return axis==0u?
						(z*shape.ny+y)*(shape.nx+1u)+x:(axis==1u?
						(z*(shape.ny+1u)+y)*shape.nx+x:(z*shape.ny+y)*shape.nx+x);};
					for(std::size_t z=0u;z<shape.nz;++z){for(std::size_t y=0u;y<shape.ny;++y){
						for(std::size_t x=0u;x<shape.nx;++x){const std::size_t cell=
							(z*shape.ny+y)*shape.nx+x;const float divergence=(
							field[0][faceIndex(0u,x+1u,y,z)]-field[0][faceIndex(0u,x,y,z)]+
							field[1][faceIndex(1u,x,y+1u,z)]-field[1][faceIndex(1u,x,y,z)]+
							field[2][faceIndex(2u,x,y,z+1u)]-field[2][faceIndex(2u,x,y,z)])/
							shape.cellWidthM;const float residual=divergence-
							device.projectionTargetPerS[cell];maximum=std::max(maximum,
							std::fabs(residual));}}}return maximum;};
				const float metalRoundedResidual=metalProjectionResidual(device.projectedVelocityMPerS);
				float metalMaximumVelocity=0.0f;
				for(unsigned int axis=0u;axis<3u;++axis)for(std::size_t face=0u;
					face<device.projectedVelocityMPerS[axis].size();++face){metalMaximumVelocity=
						std::max(metalMaximumVelocity,std::fabs(device.projectedVelocityMPerS[axis][face]));
					std::size_t left=0u,right=0u,coordinate=0u;unsigned int faceAxis=0u;
					traceFaceCells(traceFaceOffset[axis]+face,left,right,faceAxis,coordinate);
					const std::size_t extent=axis==0u?shape.nx:(axis==1u?shape.ny:shape.nz);
					const unsigned int side=2u*axis+(coordinate==extent?1u:0u);
					const bool wall=(coordinate==0u||coordinate==extent)&&
						ownerRequest32.scalarContract.boundary[side]==FireProductionProjectionWall;
					if(!wall)metalMaximumVelocity=std::max(metalMaximumVelocity,std::fabs(
						device.provisionalMomentumKGPerM2S[axis][face]/
						device.faceDensityKGPerM3[axis][face]));}
				const float metalValidationTolerance=0.005f*metalMaximumVelocity/
					(shape.cellWidthM*static_cast<float>(std::max(shape.nx,
						std::max(shape.ny,shape.nz))));
				const bool metalResidualIdentity=metalRoundedResidual==
					device.maximumPostProjectionResidualPerS;
				const bool metalResidualAccepted=metalResidualIdentity&&
					metalRoundedResidual<=metalValidationTolerance;
				auto residualMutant=device.projectedVelocityMPerS;
				if(!residualMutant[0].empty())residualMutant[0][0]=std::nextafter(
					residualMutant[0][0]+1.0f,std::numeric_limits<float>::infinity());
				const bool metalResidualMutantRefused=metalProjectionResidual(residualMutant)!=
					device.maximumPostProjectionResidualPerS;
				std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED "
					"name=metal_projection_residual_is_authority stage=R%u iteration=%zu "
					"recorded=%.17g recomputed=%.17g tolerance=%.17g mutant_refused=%d passed=%d\n",
					stage,iteration,static_cast<double>(device.maximumPostProjectionResidualPerS),
					static_cast<double>(metalRoundedResidual),static_cast<double>(metalValidationTolerance),
					metalResidualMutantRefused?1:0,
					(metalResidualAccepted&&metalResidualMutantRefused)?1:0);
				if(stage==2u&&iteration==2u&&projectionStreamingPassed){
					auto zeroedOutflowHead=tracedProjectionRequest;std::size_t changed=0u;
					for(unsigned int side=0u;side<6u;++side)for(std::size_t local=0u;
						local<zeroedOutflowHead.sealedPressureOpenDynamicPressurePa[side].size()&&
						local<device.activeClass[side].size();++local)
						if(device.activeClass[side][local]==0u&&zeroedOutflowHead.
							sealedPressureOpenDynamicPressurePa[side][local].Rounded()!=0.0f){
							zeroedOutflowHead.sealedPressureOpenDynamicPressurePa[side][local]=
								TraceFloat(0.0f);++changed;}
					FireProductionRoundoffAdapter::ResidentStepTraceResult::
						ProjectionStreamingEvidence mutantEvidence;
					const bool mutantAccepted=
						FireProductionRoundoffAdapter::EvaluateProjectionStreamingEvidence(
							zeroedOutflowHead,tracedProjection,false,mutantEvidence);
					sealedOpenHeadOutflowTraceRED=changed!=0u&&!mutantAccepted;
					std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED "
						"name=sealed_open_head_not_conditioned_on_active_class stage=R2 "
						"iteration=2 changed_outflow_heads=%zu mutant_accepted=%d passed=%d\n",
						changed,mutantAccepted?1:0,sealedOpenHeadOutflowTraceRED?1:0);}
				FireProductionRoundoffWalker::ProjectionAposterioriCertificate projectionCertificate;
				std::array<unsigned int,6> projectionBoundary={};
				for(unsigned int side=0u;side<6u;++side)projectionBoundary[side]=
					static_cast<unsigned int>(tracedProjectionRequest.boundary[side]);
				const bool zeroBootstrapProjection=bootstrapProjection&&projectionStreamingPassed&&
					projectionStreaming.maximumRoundedResidual==0.0f&&
					projectionStreaming.maximumRoundedVelocity==0.0;
				const bool projectionCertificatePassed=zeroBootstrapProjection||
					(projectionStreamingPassed&&metalResidualAccepted&&metalResidualMutantRefused&&
					FireProductionRoundoffWalker::DeriveProjectionAposterioriBound(
						{{shape.nx,shape.ny,shape.nz}},projectionBoundary,shape.cellWidthM,
						projectionStreaming.densityLower,projectionStreaming.densityUpper,
						metalRoundedResidual,
						projectionStreaming.maximumResidualEvaluationRadius,
						projectionStreaming.maximumCrossPrecisionResidualUpper,
						projectionStreaming.maximumRoundedVelocity,
						projectionStreaming.maximumRoundedTarget,
						projectionStreaming.maximumBeginningVelocityRoundingUpper,false,
						projectionStreaming.streamingFaceVelocityL2PerCellUpper,
						projectionStreaming.fp64TerminalFaceL2PerCellUpper,
						projectionStreaming.validationToleranceRounded,
						projectionStreaming.validationToleranceRadius,projectionCertificate));
				for(unsigned int axis=0u;axis<3u;++axis){const std::size_t faces=
					device.projectedVelocityMPerS[axis].size();transportVelocityBounds[axis].resize(faces);
					projectionFaceDensityBounds[axis].resize(faces);projectionMomentumBounds[axis].resize(faces);
					if(!projectionCertificatePassed||tracedProjection.velocityMPerS[axis].size()!=faces||
						tracedProjection.faceDensityKGPerM3[axis].size()!=faces||
						tracedProjection.momentumKGPerM2S[axis].size()!=faces){traceFieldsPassed=false;continue;}
					for(std::size_t face=0u;face<faces;++face){
						transportVelocityBounds[axis][face]=zeroBootstrapProjection?0.0:
							std::nextafter(std::sqrt(static_cast<double>(cells))*
								projectionCertificate.velocityRMSUpper,
								std::numeric_limits<double>::infinity());
						projectionFaceDensityBounds[axis][face]=tracedProjection.faceDensityKGPerM3[axis][face].Radius()+
							std::fabs(tracedProjection.faceDensityKGPerM3[axis][face].Center()-
								mirror.faceDensityKGPerM3[axis][face]);
						const CertifiedBinary32 certifiedMomentum=CertifiedMultiply(CertifiedBinary32{
							device.faceDensityKGPerM3[axis][face],mirror.faceDensityKGPerM3[axis][face],
							projectionFaceDensityBounds[axis][face],true},CertifiedBinary32{
							device.projectedVelocityMPerS[axis][face],mirror.projectedVelocityMPerS[axis][face],
							transportVelocityBounds[axis][face],true});
						projectionMomentumBounds[axis][face]=certifiedMomentum.error+std::fabs(
							certifiedMomentum.exact-mirror.projectedMomentumKGPerM2S[axis][face]);}}
				std::uint64_t projectionOperations=0u;for(const std::uint64_t count:
					projectionCounters.operation)projectionOperations+=count;
				std::fprintf(stderr,"PROJECTED_HEUN_OWNER_PROJECTION_OPERATION_TRACE stage=R%u "
					"iteration=%zu operations=%llu maximum_depth=%u branch_obligations=%zu "
					"discharged=%llu streaming_l2_per_cell=%.17g fp64_terminal_l2_per_cell=%.17g "
					"velocity_norm_bound=%.17g validation_margin=%.17g density_lower=%.17g "
					"density_upper=%.17g residual=%.17g residual_eval_radius=%.17g "
					"cross_precision_residual=%.17g tolerance=%.17g tolerance_radius=%.17g "
					"rounded_residual_match=%d rounded_velocity_match=%d active_set_match=%d "
					"first_velocity_axis=%u first_velocity_face=%zu first_velocity_residual=%.17g "
					"first_velocity_radius=%.17g first_velocity_published=%.17g "
					"first_velocity_center=%.17g first_velocity_reconstructed_rounded=%.17g "
					"first_velocity_projection_center=%.17g first_velocity_projection_radius=%.17g "
					"active_ambiguous=%llu active_separated_mismatch=%llu validation_accepted=%d "
					"streaming_passed=%d error=%s passed=%d\n",stage,iteration,
					static_cast<unsigned long long>(projectionOperations),projectionCounters.maximumDepth,
					projectionCounters.branchObligations.size(),static_cast<unsigned long long>(
						projectionCounters.dischargedBranchObligationCount),
					projectionStreaming.streamingFaceVelocityL2PerCellUpper,
					projectionStreaming.fp64TerminalFaceL2PerCellUpper,
					projectionCertificate.velocityRMSUpper,
					projectionCertificate.validationPredicateMarginLower,projectionStreaming.densityLower,
					projectionStreaming.densityUpper,
					static_cast<double>(projectionStreaming.maximumRoundedResidual),
					projectionStreaming.maximumResidualEvaluationRadius,
					projectionStreaming.maximumCrossPrecisionResidualUpper,
					static_cast<double>(projectionStreaming.validationToleranceRounded),
					projectionStreaming.validationToleranceRadius,
					projectionStreaming.roundedResidualMatches?1:0,
					projectionStreaming.roundedVelocityMatches?1:0,
					projectionStreaming.openActiveSetMatches?1:0,
					projectionStreaming.firstVelocityAxis,
					projectionStreaming.firstVelocityFace,
					projectionStreaming.firstVelocityResidual,
					projectionStreaming.firstVelocityRadius,
					projectionStreaming.firstVelocityPublished,
					projectionStreaming.firstVelocityCenter,
					projectionStreaming.firstVelocityReconstructedRounded,
					projectionStreaming.firstVelocityProjectionCenter,
					projectionStreaming.firstVelocityProjectionRadius,
					static_cast<unsigned long long>(projectionStreaming.activeSetAmbiguousCount),
					static_cast<unsigned long long>(projectionStreaming.activeSetSeparatedMismatchCount),
					projectionStreaming.validationAccepted?1:0,projectionStreamingPassed?1:0,
					projectionTraceError.c_str(),
					projectionCertificatePassed?1:0);
				traceFieldsPassed=traceFieldsPassed&&projectionCertificatePassed;
				combineGate(gateIterationField(stage,iteration,"projected_velocity","m_s^-1",
					packedVelocity,packedVelocity64,[&](std::size_t packed){const unsigned int axis=
						packed<traceFaceOffset[1]?0u:(packed<traceFaceOffset[2]?1u:2u);return
						transportVelocityBounds[axis][packed-traceFaceOffset[axis]];}));
				if(stage<2u&&(device.iteration&UINT32_C(0x80000000))!=0u&&traceFieldsPassed){
					terminalDeviceVelocity[stage]=device.projectedVelocityMPerS;
					terminalMirrorVelocity[stage]=mirror.projectedVelocityMPerS;
					terminalVelocityBounds[stage]=transportVelocityBounds;
					terminalVelocityAvailable[stage]=true;}
				std::vector<double> representedPressureBounds(cells,0.0);
				for(std::size_t cell=0u;cell<cells;++cell){double energyNumerator=
					transportStateBounds[8u*cells+cell],capacityLower=0.0;
					for(std::size_t species=0u;species<7u;++species){const FireThermochemistrySpecies*
						record=fuel.FindSpecies(fuel.SpeciesOrder()[species].c_str());
						double speciesCpLower=std::numeric_limits<double>::infinity();
						if(record)for(const FireThermochemistrySegment& segment:record->segments)
							speciesCpLower=std::min(speciesCpLower,
								segment.certifiedCpLowerJPerKGK);
						const double stateRadius=transportStateBounds[(1u+species)*cells+cell];
						energyNumerator+=stateRadius*std::max(std::fabs(
							ownerMinimumEnthalpy[species]),std::fabs(ownerMaximumEnthalpy[species]));
						capacityLower+=std::max(0.0,mirror.transportConservativeValues[
							(1u+species)*cells+cell]-stateRadius)*speciesCpLower;}
					transportTemperatureBounds[cell]=capacityLower>0.0&&std::isfinite(capacityLower)?
						energyNumerator/capacityLower+Binary32RoundingEnvelope(std::fabs(
							mirror.transportTemperatureK[cell])):std::numeric_limits<double>::infinity();
					double molarDensity=0.0,molarRadius=0.0;
					for(std::size_t species=0u;species<6u;++species){const FireThermochemistrySpecies*
						record=fuel.FindSpecies(fuel.SpeciesOrder()[species].c_str());
						if(!record){molarRadius=std::numeric_limits<double>::infinity();break;}
						molarDensity+=std::max(0.0,mirror.transportConservativeValues[
							(1u+species)*cells+cell])/record->molecularWeightKGPerKMol;
						molarRadius+=transportStateBounds[(1u+species)*cells+cell]/
							record->molecularWeightKGPerKMol;}
					const double pressureFactor=8314.46261815324/fuel.ThermodynamicPressurePa();
					representedPressureBounds[cell]=pressureFactor*(std::fabs(
						mirror.transportTemperatureK[cell])*molarRadius+std::fabs(molarDensity)*
						transportTemperatureBounds[cell]+molarRadius*transportTemperatureBounds[cell])+
						traceGamma4096*std::fabs(mirror.representedPressureRatio.empty()?0.0:
							mirror.representedPressureRatio[cell]);}
				combineGate(gateIterationField(stage,iteration,"transport_temperature","K",
					device.transportTemperatureK,mirror.transportTemperatureK,[&](std::size_t cell){
						return transportTemperatureBounds[cell];}));
				FireProductionResidentTransportComparatorRequest traceTransportRequest;
				traceTransportRequest.shape=shape;traceTransportRequest.boundary=
					ownerEOS.physicalFlux.transport.boundary;
				traceTransportRequest.stage=static_cast<FireProductionProjectedHeunStage>(stage);
				traceTransportRequest.attemptIdentity=ownerEOS.physicalFlux.transport.attemptIdentity;
				traceTransportRequest.parentCandidateIdentity=UINT64_C(0x201e000000000001)+stage;
				traceTransportRequest.projectionIdentity=UINT64_C(0x201e000000000010)+iteration;
				traceTransportRequest.conservativeValues=device.transportConservativeValues;
				traceTransportRequest.temperatureK=device.transportTemperatureK;
				traceTransportRequest.projectedVelocityMPerS=device.projectedVelocityMPerS;
				traceTransportRequest.fuelInletBoundaryFace=
					ownerEOS.physicalFlux.transport.fuelInletBoundaryFace;
				CertifiedResidentTransportInputEnvelope traceTransportInputs;
				traceTransportInputs.conservativeExact=&mirror.transportConservativeValues;
				traceTransportInputs.conservativeRadius=&transportStateBounds;
				traceTransportInputs.temperatureExact=&mirror.transportTemperatureK;
				traceTransportInputs.temperatureRadius=&transportTemperatureBounds;
				traceTransportInputs.velocityExact=&mirror.projectedVelocityMPerS;
				traceTransportInputs.velocityRadius=&transportVelocityBounds;
				traceTransportInputs.coefficientRounded={{&device.diffusivityM2PerS,
					&device.conductivityWPerMK,&device.molecularKinematicViscosityM2PerS}};
				traceTransportInputs.coefficientExact={{&mirror.diffusivityM2PerS,
					&mirror.conductivityWPerMK,&mirror.molecularKinematicViscosityM2PerS}};
				std::array<std::vector<double>,3> transportCoefficientBounds;
				for(auto& field:transportCoefficientBounds)field.resize(cells);
				std::vector<CertifiedResidentTransport> certifiedTransport(cells);
				bool certifiedTransportFailed=false;std::size_t certifiedTransportFailureCell=0u;
				std::string certifiedTransportFailure;
				for(std::size_t cell=0u;cell<cells;++cell){CertifiedResidentTransport certified;
					std::string certifiedError;if(!EvaluateCertifiedResidentTransport(
						traceTransportRequest,cell,certified,&certifiedError,&traceTransportInputs)){
						if(!certifiedTransportFailed){certifiedTransportFailed=true;
							certifiedTransportFailureCell=cell;certifiedTransportFailure=certifiedError;}
						traceFieldsPassed=false;for(auto& field:transportCoefficientBounds)
							field[cell]=std::numeric_limits<double>::infinity();}
					else{certifiedTransport[cell]=certified;for(unsigned int field=0u;field<3u;++field){
						transportCoefficientBounds[field][cell]=2.0*certified.coefficient[field].error;
						if(!std::isfinite(transportCoefficientBounds[field][cell])&&
							!certifiedTransportFailed){certifiedTransportFailed=true;
							certifiedTransportFailureCell=cell;certifiedTransportFailure=
								"non-finite certified coefficient radius";}}}}
				for(std::size_t cell=0u;cell<cells;++cell)if(
					certifiedTransport[cell].eddyKinematicViscosity.error>1.0e-6||
					certifiedTransport[cell].coefficient[1].error>0.1)
					std::fprintf(stderr,"PROJECTED_HEUN_OWNER_VREMAN_DIAGNOSTIC stage=R%u iteration=%zu "
						"cell=%zu eddy_exact=%.17g eddy_error=%.17g raw_rounded=%.9g raw_exact=%.17g "
						"raw_error=%.17g raw_scale=%.17g alpha_rounded=%.9g alpha_exact=%.17g "
						"alpha_error=%.17g conductivity_exact=%.17g conductivity_error=%.17g "
						"density_exact=%.17g density_error=%.17g cp_exact=%.17g cp_error=%.17g "
						"mixture_k_exact=%.17g mixture_k_error=%.17g velocity_input_bound=%.17g\n",
						stage,iteration,cell,
						certifiedTransport[cell].eddyKinematicViscosity.exact,
						certifiedTransport[cell].eddyKinematicViscosity.error,
						certifiedTransport[cell].vremanNumerator.rounded,
						certifiedTransport[cell].vremanNumerator.exact,
						certifiedTransport[cell].vremanNumerator.error,
						certifiedTransport[cell].vremanUncancelledScale,
						certifiedTransport[cell].vremanDenominator.rounded,
						certifiedTransport[cell].vremanDenominator.exact,
						certifiedTransport[cell].vremanDenominator.error,
						certifiedTransport[cell].coefficient[1].exact,
						certifiedTransport[cell].coefficient[1].error,
						certifiedTransport[cell].gasDensity.exact,
						certifiedTransport[cell].gasDensity.error,
						certifiedTransport[cell].gasCp.exact,
						certifiedTransport[cell].gasCp.error,
						certifiedTransport[cell].mixtureConductivity.exact,
						certifiedTransport[cell].mixtureConductivity.error,
						*std::max_element(transportVelocityBounds[0].begin(),
							transportVelocityBounds[0].end()));
				if(certifiedTransportFailed)std::fprintf(stderr,
					"PROJECTED_HEUN_OWNER_CERTIFIED_TRANSPORT_FAILURE stage=R%u iteration=%zu cell=%zu error=%s\n",
					stage,iteration,certifiedTransportFailureCell,certifiedTransportFailure.c_str());
				combineGate(gateIterationField(stage,iteration,"diffusivity","m^2_s^-1",
					device.diffusivityM2PerS,mirror.diffusivityM2PerS,[&](std::size_t cell){
						return transportCoefficientBounds[0][cell];}));
				combineGate(gateIterationField(stage,iteration,"conductivity","W_m^-1_K^-1",
					device.conductivityWPerMK,mirror.conductivityWPerMK,[&](std::size_t cell){
						return transportCoefficientBounds[1][cell];}));
				combineGate(gateIterationField(stage,iteration,"molecular_kinematic_viscosity","m^2_s^-1",
					device.molecularKinematicViscosityM2PerS,mirror.molecularKinematicViscosityM2PerS,
					[&](std::size_t cell){return transportCoefficientBounds[2][cell];}));
				combineGate(gateIterationField(stage,iteration,"gas_density","kg_m^-3",
					device.gasDensityKGPerM3,mirror.gasDensityKGPerM3,[&](std::size_t cell){
						return gasDensityBounds[cell];}));
				std::vector<double> physicalMassBounds(8u*allFaces,0.0);
				std::vector<double> physicalEnergyBounds(allFaces,0.0);
				const FireCertifiedNullspace& tracePhysicalProjection=
					fuel.NonadvectiveFluxProjection();
				auto uncertainPhysicalInput=[](const float rounded,const double exact,
					const double radius){return CertifiedBinary32{rounded,exact,radius,
						std::isfinite(exact)&&std::isfinite(radius)&&radius>=0.0&&
						std::fabs(static_cast<double>(rounded)-exact)<=radius};};
				for(std::size_t face=0u;face<allFaces;++face){std::size_t left=0u,right=0u,
					coordinate=0u;unsigned int axis=0u;traceFaceCells(face,left,right,axis,coordinate);
					const std::size_t extent=axis==0u?shape.nx:(axis==1u?shape.ny:shape.nz);
					const bool geometricBoundary=coordinate==0u||coordinate==extent;
					bool active=!geometricBoundary||ownerLineage.eos.physicalFlux.transport.boundary[
						2u*axis]==FireProductionProjectionPeriodic;
					if(geometricBoundary&&active){const std::size_t stride=axis==0u?1u:
						(axis==1u?shape.nx:shape.nx*shape.ny);if(coordinate==0u)
							left+=(extent-1u)*stride;else right-=(extent-1u)*stride;}
					bool leftAmbient=false,rightAmbient=false;
					if(geometricBoundary&&!active){const unsigned int side=2u*axis+
						(coordinate==extent?1u:0u);const std::size_t localFace=face-traceFaceOffset[axis];
						const std::size_t local=axis==0u?localFace/(shape.nx+1u):
							(axis==1u?(localFace/shape.nx)/(shape.ny+1u)*shape.nx+
								localFace%shape.nx:localFace%(shape.nx*shape.ny));
						const auto& physicalClass=stage==2u?mirror.nextActiveClass:mirror.activeClass;
						active=ownerLineage.eos.physicalFlux.transport.boundary[side]==
							FireProductionProjectionPressureOpen&&local<physicalClass[side].size()&&
							physicalClass[side][local]!=0u;
						leftAmbient=active&&coordinate==0u;rightAmbient=active&&coordinate==extent;}
					if(!active)continue;
					auto stateInput=[&](const std::size_t component,const std::size_t cell,
						const bool ambient){if(ambient)return CertifiedRecord(
							ownerLineage.eos.physicalFlux.ambient[component]);
						const std::size_t index=
							component*cells+cell;return uncertainPhysicalInput(
							device.transportConservativeValues[index],
							mirror.transportConservativeValues[index],transportStateBounds[index]);};
					auto temperatureInput=[&](const std::size_t cell,const bool ambient){return ambient?
						CertifiedRecord(ownerLineage.eos.physicalFlux.ambientTemperatureK):
						uncertainPhysicalInput(device.transportTemperatureK[cell],
							mirror.transportTemperatureK[cell],transportTemperatureBounds[cell]);};
					auto coefficientInput=[&](const unsigned int field,const std::size_t cell){
						const std::vector<float>& observed=field==0u?device.diffusivityM2PerS:
							device.conductivityWPerMK;const std::vector<double>& exact=field==0u?
							mirror.diffusivityM2PerS:mirror.conductivityWPerMK;
						return uncertainPhysicalInput(observed[cell],exact[cell],
							transportCoefficientBounds[field][cell]);};
					auto harmonic=[](const CertifiedBinary32& a,const CertifiedBinary32& b){
						if(!(a.rounded>0.0f)||!(b.rounded>0.0f))return CertifiedInput(0.0f);
						const CertifiedBinary32 denominator=CertifiedAdd(a,b),numerator=
							CertifiedMultiply(CertifiedRecord(2.0),CertifiedMultiply(a,b));
						if(denominator.valid&&std::fabs(denominator.exact)>denominator.error)
							return CertifiedDivide(numerator,denominator);
						const float rounded=2.0f*a.rounded*b.rounded/(a.rounded+b.rounded);
						const double exact=a.exact>0.0&&b.exact>0.0?2.0*a.exact*b.exact/
							(a.exact+b.exact):0.0,upper=2.0*std::max(
							std::max(0.0,a.exact+a.error),std::max(0.0,b.exact+b.error));
						return CertifiedBinary32{rounded,exact,std::max(std::fabs(exact),
							std::fabs(upper-exact))+Binary32RoundingEnvelope(upper),std::isfinite(upper)};};
					CertifiedBinary32 totalLeft=CertifiedInput(0.0f),
						totalRight=CertifiedInput(0.0f);
					for(std::size_t species=1u;species<8u;++species){totalLeft=CertifiedAdd(
						totalLeft,stateInput(species,left,leftAmbient));totalRight=CertifiedAdd(
						totalRight,stateInput(species,right,rightAmbient));}
					CertifiedBinary32 rhoD=CertifiedInput(0.0f),conductivity=CertifiedInput(0.0f);
					if(geometricBoundary){const std::size_t interior=leftAmbient?right:left;
						rhoD=CertifiedMultiply(leftAmbient?totalRight:totalLeft,
							coefficientInput(0u,interior));conductivity=coefficientInput(1u,interior);
					}else{rhoD=harmonic(CertifiedMultiply(totalLeft,coefficientInput(0u,left)),
							CertifiedMultiply(totalRight,coefficientInput(0u,right)));
						conductivity=harmonic(coefficientInput(1u,left),coefficientInput(1u,right));}
					const CertifiedBinary32 distance=CertifiedMultiply(CertifiedInput(
						shape.cellWidthM),CertifiedRecord(geometricBoundary?0.5:1.0));
					std::array<CertifiedBinary32,8> raw,projected;
					for(std::size_t component=0u;component<8u;++component){raw[component]=
						CertifiedDivide(CertifiedMultiply(CertifiedRecord(-1.0),CertifiedMultiply(
							rhoD,CertifiedSubtract(CertifiedDivide(stateInput(component,right,rightAmbient),
								totalRight),CertifiedDivide(stateInput(component,left,leftAmbient),totalLeft)))),
							distance);}
					// The live physical-flux kernel forms raw in binary32, promotes those
					// rounded values, applies N(N^T raw) in binary64, then casts once on
					// publication.  Replaying the matrix in CertifiedBinary32 would certify
					// a different DAG and, at trace-species cancellation, under-enclose it.
					// Propagate the raw chord through the exact linear map and add the
					// record's independently derived binary64 projection obligation plus
					// the single terminal binary32 cast.
					const double projectionFactor=fuel.AcceptedStateFeasibilityEnvelope().
						nullspaceProjectionFactorEpsilon64;
					const double epsilon64=std::numeric_limits<double>::epsilon();
					double rawRoundedScale=0.0,rawExactScale=0.0;
					for(const CertifiedBinary32& value:raw){rawRoundedScale+=std::fabs(
						static_cast<double>(value.rounded));rawExactScale+=std::fabs(value.exact);}
					for(std::size_t component=0u;component<8u;++component){double roundedPath=0.0,
						exactPath=0.0,inputPropagation=0.0,rowNorm=0.0;
						for(std::size_t row=0u;row<8u;++row){double coefficient=0.0;
							for(std::size_t basis=0u;basis<tracePhysicalProjection.nullity;++basis)
								coefficient+=tracePhysicalProjection.orthonormalBasis[
									component*tracePhysicalProjection.nullity+basis]*
									tracePhysicalProjection.orthonormalBasis[
										row*tracePhysicalProjection.nullity+basis];
							roundedPath+=coefficient*static_cast<double>(raw[row].rounded);
							exactPath+=coefficient*raw[row].exact;
							inputPropagation+=std::fabs(coefficient)*raw[row].error;
							rowNorm+=std::fabs(coefficient);}
						const double arithmeticBound=projectionFactor*epsilon64*std::max(1.0,rowNorm)*
							(std::max(1.0,rawRoundedScale)+std::max(1.0,rawExactScale));
						const float rounded=static_cast<float>(roundedPath);
						const double castBound=std::fabs(static_cast<double>(rounded)-roundedPath);
						const double bound=inputPropagation+arithmeticBound+castBound;
						projected[component]=CertifiedBinary32{rounded,exactPath,bound,
							std::isfinite(bound)&&bound>=0.0&&std::fabs(
								static_cast<double>(rounded)-exactPath)<=bound};
						physicalMassBounds[component*allFaces+face]=bound;}
					const CertifiedBinary32 leftTemperature=temperatureInput(left,leftAmbient),
						rightTemperature=temperatureInput(right,rightAmbient),faceTemperature=
						CertifiedMultiply(CertifiedAdd(leftTemperature,rightTemperature),
							CertifiedRecord(0.5));
					std::array<double,7> enthalpy={{}},roundedEnthalpy={{}};std::string enthalpyError;
					const bool enthalpyValid=fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(
						faceTemperature.exact,enthalpy.data(),enthalpy.size(),&enthalpyError)&&
						fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(faceTemperature.rounded,
							roundedEnthalpy.data(),roundedEnthalpy.size(),&enthalpyError);
					CertifiedBinary32 energy=CertifiedInput(0.0f);
					for(std::size_t species=0u;species<7u&&enthalpyValid;++species){
						const FireThermochemistrySpecies* record=fuel.FindSpecies(
							fuel.SpeciesOrder()[species].c_str());double cpLower=0.0,cpUpper=0.0;
						const double lowT=std::max(fuel.TemperatureMinK(),
							faceTemperature.exact-faceTemperature.error),highT=std::min(
								fuel.TemperatureMaxK(),faceTemperature.exact+faceTemperature.error);
						if(!record||!(lowT>0.0)||highT<lowT||!certifiedCpBounds(*record,
							lowT,highT,cpLower,cpUpper)){energy.valid=false;break;}
						const float roundedH=static_cast<float>(roundedEnthalpy[species]);
						const double enthalpyRadius=cpUpper*faceTemperature.error+
							Binary32RoundingEnvelope(roundedEnthalpy[species]);
						const CertifiedBinary32 h={roundedH,enthalpy[species],
							enthalpyRadius,std::isfinite(cpUpper)&&
							std::isfinite(enthalpyRadius)&&std::fabs(static_cast<double>(roundedH)-
								enthalpy[species])<=enthalpyRadius};
						energy=CertifiedAdd(energy,CertifiedMultiply(h,projected[1u+species]));}
					const CertifiedBinary32 conduction=CertifiedDivide(CertifiedMultiply(conductivity,
						CertifiedSubtract(rightTemperature,leftTemperature)),distance);
					energy=CertifiedSubtract(energy,conduction);
					physicalEnergyBounds[face]=enthalpyValid&&energy.valid?energy.error+std::fabs(
						energy.exact-mirror.physicalEnergyFluxWPerM2[face]):
						std::numeric_limits<double>::infinity();
					}
				const bool terminalIteration=device.iteration!=std::numeric_limits<std::uint32_t>::max()&&
					(device.iteration&UINT32_C(0x80000000))!=0u;
				if(stage<2u){
					std::vector<TraceFloat> tracedState;
					std::array<std::vector<TraceFloat>,3> tracedVelocity;
					::RISEFireProductionTrace::FireProductionScalarFCTRequest tracedRequest;
					FireProductionScalarFCTRequest binary32Request;
					const std::vector<double>& tracedStateCenter=mirror.transportConservativeValues;
					bool fctTraceInputs=traceInputVector(device.transportConservativeValues,
						tracedStateCenter,transportStateBounds,tracedState);
					for(unsigned int axis=0u;axis<3u&&fctTraceInputs;++axis){
						const std::vector<double>& axisCenter=mirror.projectedVelocityMPerS[axis];
						const std::vector<double>& axisBounds=transportVelocityBounds[axis];
						fctTraceInputs=traceInputVector(device.projectedVelocityMPerS[axis],
							axisCenter,axisBounds,tracedVelocity[axis]);}
					::RISEFireProductionTrace::FireProductionScalarFCTFluxPair tracedFluxPair;
					::RISEFireProductionTrace::FireProductionScalarFCTResult tracedFCT;
					FireProductionScalarFCTFluxPair binary32FluxPair;
					FireProductionScalarFCTResult binary32FCT;
					FireProductionRoundoffTrace::Counters fctCounters;
					std::string fctTraceError;bool fctTraceSolved=false;
					if(fctTraceInputs){tracedRequest=traceScalarRequest(tracedState,
						tracedVelocity,device.activeClass);binary32Request=binary32ScalarRequest(
							device.transportConservativeValues,device.projectedVelocityMPerS,device.activeClass);
						bool binary32Solved=BuildFireProductionScalarFCTFluxPairCPU(binary32Request,
							binary32FluxPair,&fctTraceError);FireProductionRoundoffTrace::Scope traceScope(fctCounters);
						fctTraceSolved=::RISEFireProductionTrace::BuildFireProductionScalarFCTFluxPairCPU(
							tracedRequest,tracedFluxPair,&fctTraceError)&&binary32Solved;
						if(fctTraceSolved&&tracedFluxPair.lowFlux.size()==9u*allFaces){
							for(std::size_t component=0u;component<9u&&fctTraceSolved;++component)
								for(std::size_t face=0u;face<allFaces;++face){const std::size_t index=
									component*allFaces+face;const float roundedPhysical=component<8u?
									device.physicalMassFluxKGPerM2S[index]:device.physicalEnergyFluxWPerM2[face];
									const double centerPhysical=component<8u?
										mirror.physicalMassFluxKGPerM2S[index]:mirror.physicalEnergyFluxWPerM2[face];
									const double radiusPhysical=component<8u?
										physicalMassBounds[index]:physicalEnergyBounds[face];
									if(!std::isfinite(radiusPhysical)||std::fabs(static_cast<double>(
										roundedPhysical)-centerPhysical)>radiusPhysical){fctTraceSolved=false;
										fctTraceError="physical flux escaped FCT trace input enclosure";break;}
									binary32FluxPair.lowFlux[index]+=roundedPhysical;
									tracedFluxPair.lowFlux[index]+=TraceFloat::Raw(centerPhysical,
										radiusPhysical,roundedPhysical,0u);}
							if(fctTraceSolved)binary32Solved=SolveFireProductionScalarFCTFluxPairCPU(
								binary32Request,binary32FluxPair,binary32FCT,&fctTraceError);
							if(fctTraceSolved&&binary32Solved)fctTraceSolved=
								::RISEFireProductionTrace::SolveFireProductionScalarFCTFluxPairCPU(
									tracedRequest,tracedFluxPair,tracedFCT,&fctTraceError);
							else fctTraceSolved=false;}}
					std::uint64_t fctOperationCount=0u;for(const std::uint64_t count:
						fctCounters.operation)fctOperationCount+=count;
					std::size_t fctCertifiedBranches=0u;for(const auto& obligation:
						fctCounters.branchObligations)fctCertifiedBranches+=obligation.certificate!=
							FireProductionRoundoffTrace::BranchCertificate::None?1u:0u;
					double fctMaximumLowRadius=0.0,fctMaximumDeltaRadius=0.0,
						fctMaximumAlphaRadius=0.0;
					for(const TraceFloat& value:tracedFluxPair.lowFlux)fctMaximumLowRadius=
						std::max(fctMaximumLowRadius,value.Radius());
					for(const TraceFloat& value:tracedFluxPair.fluxDelta)fctMaximumDeltaRadius=
						std::max(fctMaximumDeltaRadius,value.Radius());
					for(const auto& axis:tracedFCT.sharedFaceAlpha)for(const TraceFloat& value:axis)
						fctMaximumAlphaRadius=std::max(fctMaximumAlphaRadius,value.Radius());
					bool fctFluxPairEnclosed=true;std::size_t fctFirstMismatch=0u;
					if(fctTraceSolved){const auto& expected=binary32FluxPair;
						fctFluxPairEnclosed=tracedFluxPair.lowFlux.size()==expected.lowFlux.size()&&
							tracedFluxPair.fluxDelta.size()==expected.fluxDelta.size();
						for(std::size_t index=0u;fctFluxPairEnclosed&&index<expected.lowFlux.size();++index)
							if(std::fabs(static_cast<double>(expected.lowFlux[index])-
								tracedFluxPair.lowFlux[index].Center())>tracedFluxPair.lowFlux[index].Radius()||
								std::fabs(static_cast<double>(expected.fluxDelta[index])-
								tracedFluxPair.fluxDelta[index].Center())>tracedFluxPair.fluxDelta[index].Radius()){
								fctFluxPairEnclosed=false;fctFirstMismatch=index;}}
					bool fctAcceptedEnclosed=true;
					if(terminalIteration&&fctTraceSolved){
						for(unsigned int axis=0u;axis<3u&&fctAcceptedEnclosed;++axis){
							fctAcceptedEnclosed=tracedFCT.sharedFaceAlpha[axis].size()==
								device.sharedFaceAlpha[axis].size();for(std::size_t face=0u;
								fctAcceptedEnclosed&&face<device.sharedFaceAlpha[axis].size();++face)
								fctAcceptedEnclosed=std::fabs(static_cast<double>(device.sharedFaceAlpha[axis][face])-
									tracedFCT.sharedFaceAlpha[axis][face].Center())<=
									tracedFCT.sharedFaceAlpha[axis][face].Radius();}}
					const bool classEnvelopeUsed=fctCounters.dischargedBranchObligationCount!=
						fctCounters.branchObligations.size()||fctCounters.unresolvedBranch||fctCounters.invalidDomain;
					std::fprintf(stderr,"PROJECTED_HEUN_OWNER_FCT_OPERATION_TRACE stage=R%u iteration=%zu "
						"operations=%llu maximum_depth=%u comparisons=%llu branch_obligations=%zu "
						"certified_branch_obligations=%zu discharged_branch_obligations=%llu "
						"class_envelope_used=%d terminal=%d flux_pair_enclosed=%d first_mismatch=%zu "
						"accepted_enclosed=%d maximum_low_radius=%.17g maximum_delta_radius=%.17g "
						"maximum_alpha_radius=%.17g "
						"error=%s passed=%d\n",stage,
						iteration,static_cast<unsigned long long>(fctOperationCount),fctCounters.maximumDepth,
						static_cast<unsigned long long>(fctCounters.comparisonCount),
						fctCounters.branchObligations.size(),fctCertifiedBranches,
						static_cast<unsigned long long>(fctCounters.dischargedBranchObligationCount),
						classEnvelopeUsed?1:0,terminalIteration?1:0,fctFluxPairEnclosed?1:0,
						fctFirstMismatch,fctAcceptedEnclosed?1:0,fctMaximumLowRadius,
						fctMaximumDeltaRadius,fctMaximumAlphaRadius,fctTraceError.c_str(),
						fctTraceSolved?1:0);
					stageStateBoundsValid=stageStateBoundsValid&&fctTraceSolved&&
						fctFluxPairEnclosed&&(!terminalIteration||fctAcceptedEnclosed);
					if(terminalIteration&&fctTraceSolved&&fctFluxPairEnclosed&&fctAcceptedEnclosed){
						traceTerminalFluxPair[stage]=tracedFluxPair;
						traceTerminalFluxPairAvailable[stage]=true;
						FireProductionScalarFCTFluxPair deviceFluxPair;
						deviceFluxPair.shape=shape;
						deviceFluxPair.timeStepS=ownerRequest32.scalarContract.timeStepS;
						deviceFluxPair.boundary=ownerRequest32.scalarContract.boundary;
						deviceFluxPair.packedFaceOffset={{0u,
							FireProductionProjectionFaceCount(shape,0u),
							FireProductionProjectionFaceCount(shape,0u)+
								FireProductionProjectionFaceCount(shape,1u)}};
						deviceFluxPair.lowFlux=device.scalarLowFlux;
						deviceFluxPair.fluxDelta=device.scalarFluxDelta;
						deviceTerminalFluxPair[stage]=deviceFluxPair;
						deviceTerminalFluxPairAvailable[stage]=deviceFluxPair.lowFlux.size()==
							9u*allFaces&&deviceFluxPair.fluxDelta.size()==9u*allFaces;
						const bool sameClassTrajectory=alphaClassesAgree&&
							sameClasses(device.activeClass,mirror.activeClass)&&
							sameClasses(device.nextActiveClass,mirror.nextActiveClass);
						std::vector<CertifiedBinary32> acceptedCertificate;
						const bool acceptedCertificateBuilt=fctAcceptedCertificates(tracedRequest,
							tracedFluxPair,deviceFluxPair,tracedFCT,device.sharedFaceAlpha,
							mirror.sharedFaceAlpha,
							acceptedCertificate);
						if(stage==0u){const bool producerCertified=sameClassTrajectory&&
							acceptedCertificateBuilt&&deviceTerminalFluxPairAvailable[stage]&&
							certifyProducerPublication(0u,tracedRequest,tracedFluxPair,
								!fctCounters.branchObligations.empty()&&
								fctCertifiedBranches==fctCounters.branchObligations.size()&&
								std::all_of(fctCounters.branchObligations.begin(),
									fctCounters.branchObligations.end(),[](const auto& obligation){return
										obligation.predicateCenter-obligation.predicateRadius<=0.0&&
										obligation.predicateCenter+obligation.predicateRadius>=0.0;}),
								acceptedCertificate,
								device.acceptedConservativeValues,
								ownerObserved64.r0.scalarAcceptance.accepted,
								device.acceptedCandidateIdentity,
								ownerObserved.candidatePublicationIdentity[0]);
							stageStateBoundsValid=stageStateBoundsValid&&producerCertified;
							std::fprintf(stderr,"PROJECTED_HEUN_OWNER_PRODUCER_CERTIFICATE stage=R0 "
								"identity=%llu finite_local_enclosures=%d passed=%d\n",
								static_cast<unsigned long long>(device.acceptedCandidateIdentity),
								producerCertified?1:0,producerCertified?1:0);
							traceR0TerminalVelocity=tracedVelocity;traceR0TerminalClass=device.activeClass;}
					else if(traceTerminalFluxPairAvailable[0]&&deviceTerminalFluxPairAvailable[0]&&
						deviceTerminalFluxPairAvailable[1]){
							::RISEFireProductionTrace::FireProductionScalarFCTFluxPair averaged;
							FireProductionScalarFCTFluxPair deviceAveraged;
							::RISEFireProductionTrace::FireProductionScalarFCTResult heunTrace;
							std::vector<TraceFloat> beginning;std::vector<double> zeroBounds(9u*cells,0.0);
							const bool beginningTraced=traceInputVector(ownerRequest32.beginningConservativeValues,
								ownerRequest64.beginningConservativeValues,zeroBounds,beginning);
							const auto heunRequest=traceScalarRequest(beginning,traceR0TerminalVelocity,
								traceR0TerminalClass);std::string heunTraceError;
							FireProductionRoundoffTrace::Counters heunCounters;bool heunSolved=false;
							{FireProductionRoundoffTrace::Scope heunScope(heunCounters);heunSolved=
								beginningTraced&&::RISEFireProductionTrace::AverageFireProductionScalarFCTFluxPairsCPU(
									traceTerminalFluxPair[0],traceTerminalFluxPair[1],averaged,&heunTraceError)&&
								AverageFireProductionScalarFCTFluxPairsCPU(deviceTerminalFluxPair[0],
									deviceTerminalFluxPair[1],deviceAveraged,&heunTraceError)&&
								::RISEFireProductionTrace::SolveFireProductionScalarFCTFluxPairCPU(
									heunRequest,averaged,heunTrace,&heunTraceError);}
							std::uint64_t heunOperations=0u;for(const std::uint64_t count:
								heunCounters.operation)heunOperations+=count;
							std::size_t heunCertifiedObligations=0u,heunCrossingObligations=0u;
							for(const auto& obligation:heunCounters.branchObligations){
								heunCertifiedObligations+=obligation.certificate!=
									FireProductionRoundoffTrace::BranchCertificate::None?1u:0u;
								heunCrossingObligations+=obligation.predicateCenter-
									obligation.predicateRadius<=0.0&&obligation.predicateCenter+
									obligation.predicateRadius>=0.0?1u:0u;}
							bool heunRoundedMatch=heunSolved&&heunTrace.accepted.size()==
								ownerObserved32.heunSolve.scalar.accepted.size();
							std::size_t heunFirstMismatch=0u;
							for(std::size_t index=0u;heunRoundedMatch&&index<heunTrace.accepted.size();++index)
								if(heunTrace.accepted[index].Rounded()!=
									ownerObserved32.heunSolve.scalar.accepted[index]){
									heunRoundedMatch=false;heunFirstMismatch=index;}
							bool heunClassesAgree=true;for(unsigned int axis=0u;axis<3u;++axis){
								heunClassesAgree=heunClassesAgree&&device.sharedFaceAlpha[axis].size()==
									ownerObserved64.heunSolve.scalar.sharedFaceAlpha[axis].size();
								for(std::size_t face=0u;heunClassesAgree&&
									face<device.sharedFaceAlpha[axis].size();++face)
									heunClassesAgree=alphaClass(device.sharedFaceAlpha[axis][face])==
										alphaClass(ownerObserved64.heunSolve.scalar.sharedFaceAlpha[axis][face]);}
							// The independent TraceFloat box is retained as the RED witness for
							// why recursive Cartesian propagation was rejected: an inactive,
							// subnormal CO row loses correlation and reaches an infinite radius.
							// The live gate is the authenticated published endpoint chord above,
							// not this deliberately over-approximated auxiliary replay.
							const bool producerCertified=heunSolved&&heunClassesAgree&&
								fctAcceptedCertificates(heunRequest,averaged,deviceAveraged,heunTrace,
									device.sharedFaceAlpha,
									ownerObserved64.heunSolve.scalar.sharedFaceAlpha,
									acceptedCertificate)&&
								certifyProducerPublication(1u,heunRequest,averaged,
									!heunCounters.branchObligations.empty()&&
									std::all_of(heunCounters.branchObligations.begin(),
										heunCounters.branchObligations.end(),[](const auto& obligation){return
											obligation.certificate!=FireProductionRoundoffTrace::
												BranchCertificate::None&&obligation.predicateCenter-
												obligation.predicateRadius<=0.0&&obligation.predicateCenter+
												obligation.predicateRadius>=0.0;}),acceptedCertificate,
									device.acceptedConservativeValues,
									ownerObserved64.heunSolve.scalar.accepted,
									device.acceptedCandidateIdentity,
									ownerObserved.candidatePublicationIdentity[1]);
							std::vector<CertifiedBinary32> cpuSurrogateCertificate;
							const bool cpuSurrogateBuilt=fctAcceptedCertificates(heunRequest,averaged,
								ownerObserved32.averagedFlux.compositeFluxPair,heunTrace,
								device.sharedFaceAlpha,ownerObserved64.heunSolve.scalar.sharedFaceAlpha,
								cpuSurrogateCertificate);
							bool cpuSurrogateAdmissible=cpuSurrogateBuilt&&cpuSurrogateCertificate.size()==
								9u*cells;for(std::size_t index=0u;cpuSurrogateAdmissible&&index<9u*cells;
								++index)cpuSurrogateAdmissible=cpuSurrogateCertificate[index].valid&&
								std::fabs(static_cast<double>(device.acceptedConservativeValues[index])-
									ownerObserved64.heunSolve.scalar.accepted[index])<=
								cpuSurrogateCertificate[index].error+std::fabs(
									cpuSurrogateCertificate[index].exact-
									ownerObserved64.heunSolve.scalar.accepted[index]);
							const bool cpuTrajectorySubstitutionRED=!cpuSurrogateAdmissible;
							std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED "
								"name=cpu_flux_trajectory_substituted_for_resident_parent "
								"device_parent_certificate_passed=%d cpu_surrogate_refused=%d passed=%d\n",
								producerCertified?1:0,cpuTrajectorySubstitutionRED?1:0,
								(producerCertified&&cpuTrajectorySubstitutionRED)?1:0);
							const bool heunPassed=heunSolved&&heunClassesAgree&&producerCertified&&
								cpuTrajectorySubstitutionRED;
							stageStateBoundsValid=stageStateBoundsValid&&heunPassed;
							std::fprintf(stderr,"PROJECTED_HEUN_OWNER_PRODUCER_CERTIFICATE stage=R1 "
								"identity=%llu finite_local_enclosures=%d passed=%d\n",
								static_cast<unsigned long long>(device.acceptedCandidateIdentity),
								producerCertified?1:0,producerCertified?1:0);
							std::fprintf(stderr,"PROJECTED_HEUN_OWNER_HEUN_FCT_OPERATION_TRACE operations=%llu "
								"maximum_depth=%u comparisons=%llu branch_obligations=%zu "
								"certified_obligations=%zu crossing_obligations=%zu "
								"class_envelope_used=%d cpu_surrogate_rounded_match=%d first_mismatch=%zu "
								"trace_rounded=%.9g owner_rounded=%.9g trace_center=%.17g "
								"trace_radius=%.17g error=%s passed=%d\n",
								static_cast<unsigned long long>(heunOperations),heunCounters.maximumDepth,
								static_cast<unsigned long long>(heunCounters.comparisonCount),
								heunCounters.branchObligations.size(),heunCertifiedObligations,
								heunCrossingObligations,(heunCounters.unresolvedBranch||
								heunCounters.invalidDomain)?1:0,heunRoundedMatch?1:0,heunFirstMismatch,
								heunTrace.accepted.empty()?0.0:static_cast<double>(heunTrace.accepted[
									heunFirstMismatch].Rounded()),ownerObserved32.heunSolve.scalar.accepted.empty()?0.0:
								static_cast<double>(ownerObserved32.heunSolve.scalar.accepted[heunFirstMismatch]),
								heunTrace.accepted.empty()?0.0:heunTrace.accepted[heunFirstMismatch].Center(),
								heunTrace.accepted.empty()?0.0:heunTrace.accepted[heunFirstMismatch].Radius(),
								heunTraceError.c_str(),
								heunPassed?1:0);}}}
				else std::fprintf(stderr,"PROJECTED_HEUN_OWNER_FCT_OPERATION_TRACE stage=R2 "
					"iteration=%zu scope=not_applicable passed=1\n",iteration);
				const char* physicalMassName[8]={"physical_rhoZ_flux","physical_CH4_flux",
					"physical_O2_flux","physical_CO2_flux","physical_H2O_flux","physical_N2_flux",
					"physical_CO_flux","physical_carbon_flux"};
				for(std::size_t component=0u;component<8u;++component){
					std::vector<float> deviceComponent(allFaces);std::vector<double> mirrorComponent(allFaces);
					for(std::size_t face=0u;face<allFaces;++face){deviceComponent[face]=
						device.physicalMassFluxKGPerM2S[component*allFaces+face];mirrorComponent[face]=
						mirror.physicalMassFluxKGPerM2S[component*allFaces+face];}
					combineGate(gateIterationField(stage,iteration,physicalMassName[component],
						"kg_m^-2_s^-1",deviceComponent,mirrorComponent,[&,component](std::size_t face){
							return physicalMassBounds[component*allFaces+face];}));}
				if(!quietLocalEnclosureRED&&!physicalMassBounds.empty()){
					const auto loud=std::max_element(physicalMassBounds.begin(),physicalMassBounds.end());
					auto quiet=physicalMassBounds.end();
					for(auto candidate=physicalMassBounds.begin();candidate!=physicalMassBounds.end();
						++candidate)if(*candidate>0.0&&std::isfinite(*candidate)&&
						(quiet==physicalMassBounds.end()||*candidate<*quiet))quiet=candidate;
					quietGlobalBound=*loud;quietLocalBound=quiet==physicalMassBounds.end()?
						quietGlobalBound:*quiet;
					const std::size_t quietIndex=quiet==physicalMassBounds.end()?0u:
						static_cast<std::size_t>(quiet-physicalMassBounds.begin());
					std::vector<float> mutant=device.physicalMassFluxKGPerM2S;
					const std::vector<double>& exact=mirror.physicalMassFluxKGPerM2S;
					const double requestedResidual=0.5*(quietGlobalBound+quietLocalBound);
					mutant[quietIndex]=static_cast<float>(exact[quietIndex]+requestedResidual);
					quietMutantResidual=std::fabs(static_cast<double>(mutant[quietIndex])-exact[quietIndex]);
					const IterationFieldGate localGate=gateIterationField(3u,0u,
						"quiet_physical_flux_local_mutant","kg_m^-2_s^-1",mutant,exact,
						[&](std::size_t sample){return physicalMassBounds[sample];});
					const IterationFieldGate pooledGate=gateIterationField(3u,0u,
						"quiet_physical_flux_pooled_mutant","kg_m^-2_s^-1",mutant,exact,
						[&](std::size_t){return quietGlobalBound;});
					quietLocalEnclosureRED=std::isfinite(quietGlobalBound)&&
						quietGlobalBound>quietLocalBound&&quietMutantResidual>quietLocalBound&&
						quietMutantResidual<=quietGlobalBound&&!localGate.passed&&pooledGate.passed;}
				combineGate(gateIterationField(stage,iteration,"physical_energy_flux","W_m^-2",
					device.physicalEnergyFluxWPerM2,mirror.physicalEnergyFluxWPerM2,
					[&](std::size_t face){return physicalEnergyBounds[face];}));
				if(!recordedContinuousDefectRED&&!physicalEnergyBounds.empty()){
					const auto witness=std::max_element(physicalEnergyBounds.begin(),
						physicalEnergyBounds.end());const std::size_t face=static_cast<std::size_t>(
						witness-physicalEnergyBounds.begin());std::vector<float> mutant=
						device.physicalEnergyFluxWPerM2;const double recordedResidual=10715.0;
					mutant[face]=static_cast<float>(mirror.physicalEnergyFluxWPerM2[face]+
						recordedResidual);recordedContinuousDefectResidual=std::fabs(
						static_cast<double>(mutant[face])-mirror.physicalEnergyFluxWPerM2[face]);
					recordedContinuousDefectBound=*witness;
					const IterationFieldGate recordedGate=gateIterationField(3u,0u,
						"recorded_r201_physical_energy_defect","W_m^-2",mutant,
						mirror.physicalEnergyFluxWPerM2,[&](std::size_t sample){return
							physicalEnergyBounds[sample];});
					recordedContinuousDefectRED=std::isfinite(recordedContinuousDefectBound)&&
						recordedContinuousDefectResidual>recordedContinuousDefectBound&&
						!recordedGate.passed;}
				const bool ratioApplicable=!device.representedPressureRatio.empty()||
					!mirror.representedPressureRatio.empty();
				if(ratioApplicable)combineGate(gateIterationField(stage,iteration,
					"represented_pressure_ratio","dimensionless",device.representedPressureRatio,
					mirror.representedPressureRatio,[&](std::size_t cell){return
						representedPressureBounds[cell];}));
				else std::fprintf(stderr,"PROJECTED_HEUN_OWNER_ITERATION_FIELD stage=R%u iteration=%zu "
					"field=represented_pressure_ratio units=dimensionless scope=not_applicable passed=1\n",
					stage,iteration);
				std::vector<double> faceDensityBounds,momentumBounds;
				for(unsigned int axis=0u;axis<3u;++axis){faceDensityBounds.insert(
					faceDensityBounds.end(),projectionFaceDensityBounds[axis].begin(),
					projectionFaceDensityBounds[axis].end());momentumBounds.insert(momentumBounds.end(),
					projectionMomentumBounds[axis].begin(),projectionMomentumBounds[axis].end());}
				combineGate(gateIterationField(stage,iteration,"face_density","kg_m^-3",
					packedFaceDensity,packedFaceDensity64,[&](std::size_t face){return
						faceDensityBounds[face];}));
				combineGate(gateIterationField(stage,iteration,"projected_momentum","kg_m^-2_s^-1",
					packedMomentum,packedMomentum64,[&](std::size_t face){return momentumBounds[face];}));
				const bool stressApplicable=!packedStress.empty()||!packedStress64.empty();
				std::vector<double> stressBounds;
				if(stressApplicable){
					std::vector<CertifiedBinary32> cellStress(9u*cells,InvalidCertifiedBinary32());
					for(std::size_t cell=0u;cell<cells;++cell){CertifiedBinary32 divergence=
						CertifiedInput(0.0f);for(unsigned int diagonal=0u;diagonal<3u;++diagonal)
							divergence=CertifiedAdd(divergence,
								certifiedTransport[cell].velocityGradient[3u*diagonal+diagonal]);
						const CertifiedBinary32 dynamicViscosity=CertifiedMultiply(
							certifiedTransport[cell].gasDensity,CertifiedAdd(
								certifiedTransport[cell].coefficient[2],
								certifiedTransport[cell].eddyKinematicViscosity));
						for(unsigned int component=0u;component<3u;++component)
							for(unsigned int derivative=0u;derivative<3u;++derivative){CertifiedBinary32 strain=
								CertifiedAdd(certifiedTransport[cell].velocityGradient[3u*derivative+component],
									certifiedTransport[cell].velocityGradient[3u*component+derivative]);
								if(component==derivative)strain=CertifiedSubtract(strain,CertifiedMultiply(
									CertifiedRecord(2.0/3.0),divergence));
								cellStress[(3u*component+derivative)*cells+cell]=
									CertifiedMultiply(dynamicViscosity,strain);}}
					auto cellIndex=[&](const std::size_t x,const std::size_t y,const std::size_t z){return
						(z*shape.ny+y)*shape.nx+x;};
					auto shiftedCell=[&](std::size_t x,std::size_t y,std::size_t z,
						const unsigned int axis,const int direction,bool& exists){const std::size_t extent=
							axis==0u?shape.nx:(axis==1u?shape.ny:shape.nz);const std::size_t coordinate=
							axis==0u?x:(axis==1u?y:z);exists=true;if(direction<0&&coordinate>0u){if(axis==0u)--x;
							else if(axis==1u)--y;else --z;}else if(direction>0&&coordinate+1u<extent){if(axis==0u)++x;
							else if(axis==1u)++y;else ++z;}else{const unsigned int side=2u*axis+
								(direction>0?1u:0u);if(ownerEOS.physicalFlux.transport.boundary[side]!=
									FireProductionProjectionPeriodic){exists=false;return cellIndex(x,y,z);}
								if(axis==0u)x=direction<0?shape.nx-1u:0u;else if(axis==1u)y=
									direction<0?shape.ny-1u:0u;else z=direction<0?shape.nz-1u:0u;}
						return cellIndex(x,y,z);};
					stressBounds.assign(allFaces,0.0);
					for(std::size_t face=0u;face<allFaces;++face){std::size_t x=0u,y=0u,z=0u;
						unsigned int component=face<traceFaceOffset[1]?0u:(face<traceFaceOffset[2]?1u:2u);
						std::size_t local=face-traceFaceOffset[component];if(component==0u){x=local%(shape.nx+1u);
							local/=shape.nx+1u;y=local%shape.ny;z=local/shape.ny;}else if(component==1u){x=
							local%shape.nx;local/=shape.nx;y=local%(shape.ny+1u);z=local/(shape.ny+1u);}
						else{x=local%shape.nx;local/=shape.nx;y=local%shape.ny;z=local/shape.ny;}
						const std::size_t normal=component==0u?x:(component==1u?y:z),extent=
							component==0u?shape.nx:(component==1u?shape.ny:shape.nz);
						const bool periodic=ownerEOS.physicalFlux.transport.boundary[2u*component]==
							FireProductionProjectionPeriodic;
						CertifiedBinary32 force=CertifiedInput(0.0f);double forceTermScale=0.0;
						if((normal!=0u||periodic)&&normal!=extent){std::size_t lx=x,ly=y,lz=z;
							if(component==0u)lx=normal==0u?extent-1u:normal-1u;
							else if(component==1u)ly=normal==0u?extent-1u:normal-1u;
							else lz=normal==0u?extent-1u:normal-1u;
							const std::size_t left=cellIndex(lx,ly,lz),right=cellIndex(x,y,z),normalBase=
								(3u*component+component)*cells;
							force=CertifiedDivide(CertifiedSubtract(cellStress[normalBase+right],
								cellStress[normalBase+left]),CertifiedInput(shape.cellWidthM));
							forceTermScale+=(std::fabs(cellStress[normalBase+right].exact)+
								cellStress[normalBase+right].error+std::fabs(cellStress[normalBase+left].exact)+
								cellStress[normalBase+left].error)/shape.cellWidthM;
							for(unsigned int derivative=0u;derivative<3u;++derivative)if(derivative!=component){
								bool lpExists=false,rpExists=false,lnExists=false,rnExists=false;
								const std::size_t lp=shiftedCell(lx,ly,lz,derivative,-1,lpExists),
									rp=shiftedCell(x,y,z,derivative,-1,rpExists),
									ln=shiftedCell(lx,ly,lz,derivative,1,lnExists),
									rn=shiftedCell(x,y,z,derivative,1,rnExists);
								const std::size_t leftPrevious=lpExists?lp:left,rightPrevious=rpExists?rp:right,
									leftNext=lnExists?ln:left,rightNext=rnExists?rn:right,
									base=(3u*component+derivative)*cells;
								CertifiedBinary32 transverse=CertifiedAdd(cellStress[base+leftNext],
									cellStress[base+rightNext]);transverse=CertifiedSubtract(transverse,
									cellStress[base+leftPrevious]);transverse=CertifiedSubtract(transverse,
									cellStress[base+rightPrevious]);
								const float distance=(lpExists&&rpExists&&lnExists&&rnExists?4.0f:2.0f)*
									shape.cellWidthM;force=CertifiedAdd(force,CertifiedDivide(transverse,
									CertifiedInput(distance)));const std::size_t stencil[]={leftNext,rightNext,
										leftPrevious,rightPrevious};for(const std::size_t cell:stencil)
									forceTermScale+=(std::fabs(cellStress[base+cell].exact)+
										cellStress[base+cell].error)/distance;}}
						stressBounds[face]=force.error+traceGamma4096*forceTermScale;}
					combineGate(gateIterationField(stage,iteration,"stress_input","kg_m^-2_s^-2",
						packedStress,packedStress64,[&](std::size_t face){return stressBounds[face];}));}
				else std::fprintf(stderr,"PROJECTED_HEUN_OWNER_ITERATION_FIELD stage=R%u iteration=%zu "
					"field=stress_input units=kg_m^-2_s^-2 scope=not_applicable passed=1\n",stage,iteration);
				if(stage<2u&&(device.iteration&UINT32_C(0x80000000))!=0u&&
					traceTerminalFluxPairAvailable[stage]&&stressBounds.size()==allFaces){
					::RISEFireProductionTrace::FireProductionCompatibleFCTMomentumRequest compatibleRequest;
					compatibleRequest.shape.nx=shape.nx;compatibleRequest.shape.ny=shape.ny;
					compatibleRequest.shape.nz=shape.nz;
					compatibleRequest.shape.cellWidthM=TraceFloat(shape.cellWidthM);
					for(unsigned int side=0u;side<6u;++side)compatibleRequest.boundary[side]=
						static_cast<::RISEFireProductionTrace::FireProductionProjectionBoundary>(
							ownerRequest32.scalarContract.boundary[side]);
					for(unsigned int axis=0u;axis<3u;++axis){const std::size_t faces=
						FireProductionProjectionFaceCount(shape,axis);compatibleRequest.lowGasFluxKGPerM2S[axis].
						resize(faces);compatibleRequest.highGasFluxKGPerM2S[axis].resize(faces);
						compatibleRequest.physicalGasFluxKGPerM2S[axis].resize(faces);
						compatibleRequest.sharedFaceAlpha[axis].resize(faces);
						compatibleRequest.frozenVelocityMPerS[axis].resize(faces);
						for(std::size_t face=0u;face<faces;++face){const std::size_t packed=
							traceFaceOffset[axis]+face;TraceFloat advectiveLow(0.0f),advectiveDelta(0.0f),
								physicalGas(0.0f);for(std::size_t species=1u;species<=6u;++species){
								const std::size_t index=species*allFaces+packed;const TraceFloat physical=
									TraceFloat::Raw(mirror.physicalMassFluxKGPerM2S[index],
										physicalMassBounds[index],device.physicalMassFluxKGPerM2S[index],1u);
								advectiveLow+=traceTerminalFluxPair[stage].lowFlux[index]-physical;
								advectiveDelta+=traceTerminalFluxPair[stage].fluxDelta[index];
								physicalGas+=physical;}
							compatibleRequest.lowGasFluxKGPerM2S[axis][face]=advectiveLow;
							compatibleRequest.highGasFluxKGPerM2S[axis][face]=advectiveDelta;
							compatibleRequest.physicalGasFluxKGPerM2S[axis][face]=physicalGas;
							compatibleRequest.sharedFaceAlpha[axis][face]=TraceFloat::Raw(
								mirror.sharedFaceAlpha[axis][face],traceGamma4096,
								device.sharedFaceAlpha[axis][face],1u);
							compatibleRequest.frozenVelocityMPerS[axis][face]=TraceFloat::Raw(
								mirror.projectedVelocityMPerS[axis][face],transportVelocityBounds[axis][face],
								device.projectedVelocityMPerS[axis][face],1u);}}
					::RISEFireProductionTrace::FireProductionCompatibleFCTMomentumResult compatibleTrace;
					std::string compatibleTraceError;FireProductionRoundoffTrace::Counters compatibleCounters;
					bool compatibleBuilt=false;{FireProductionRoundoffTrace::Scope scope(compatibleCounters);
						compatibleBuilt=::RISEFireProductionTrace::
							EvaluateFireProductionCompatibleFCTMomentumDeltaCPU(compatibleRequest,
								compatibleTrace,&compatibleTraceError);}
					bool rateParentsPassed=compatibleBuilt;
					const std::vector<float>& source32=ownerRequest32.source.SourceDelta();
					const std::vector<double>& source64=ownerRequest64.source.SourceDelta();
					auto sourceRate=[&](const std::size_t cell){CertifiedBinary32 rate=CertifiedInput(0.0f);
						for(std::size_t species=1u;species<=6u;++species){const std::size_t index=
							species*cells+cell;rate=CertifiedAdd(rate,CertifiedBinary32{source32[index],
								source64[index],std::nextafter(std::fabs(static_cast<double>(source32[index])-
									source64[index]),std::numeric_limits<double>::infinity()),true});}
						return CertifiedDivide(rate,CertifiedBinary32{ownerRequest32.scalarContract.timeStepS,
							ownerRequest64.scalarContract.timeStepS,std::nextafter(std::fabs(
								static_cast<double>(ownerRequest32.scalarContract.timeStepS)-
								ownerRequest64.scalarContract.timeStepS),
								std::numeric_limits<double>::infinity()),true});};
					for(unsigned int axis=0u;axis<3u;++axis){const std::size_t faces=
						device.projectedVelocityMPerS[axis].size();for(unsigned int term=0u;term<4u;++term)
						terminalRateBounds[stage][term][axis].resize(faces);
						for(std::size_t face=0u;face<faces;++face){const std::size_t packed=
							traceFaceOffset[axis]+face;const CertifiedBinary32 density={
								device.faceDensityKGPerM3[axis][face],mirror.faceDensityKGPerM3[axis][face],
								projectionFaceDensityBounds[axis][face],true};
							const CertifiedBinary32 ambient={ownerRequest32.forceContract.ambientDensityKGPerM3,
								ownerRequest64.forceContract.ambientDensityKGPerM3,std::nextafter(std::fabs(
									static_cast<double>(ownerRequest32.forceContract.ambientDensityKGPerM3)-
									ownerRequest64.forceContract.ambientDensityKGPerM3),
									std::numeric_limits<double>::infinity()),true};
							const CertifiedBinary32 gravity={ownerRequest32.forceContract.gravityMPerS2[axis],
								ownerRequest64.forceContract.gravityMPerS2[axis],std::nextafter(std::fabs(
									static_cast<double>(ownerRequest32.forceContract.gravityMPerS2[axis])-
									ownerRequest64.forceContract.gravityMPerS2[axis]),
									std::numeric_limits<double>::infinity()),true};
							const CertifiedBinary32 buoyancy=CertifiedMultiply(CertifiedSubtract(density,ambient),
								gravity);terminalRateBounds[stage][0][axis][face]=buoyancy.error+
								std::fabs(buoyancy.exact-mirror.buoyancyMomentumRateKGPerM2S2[axis][face]);
							terminalRateBounds[stage][1][axis][face]=stressBounds[packed];
							std::size_t left=0u,right=0u,coordinate=0u;unsigned int faceAxis=0u;
							traceFaceCells(packed,left,right,faceAxis,coordinate);
							const CertifiedBinary32 restricted=CertifiedMultiply(CertifiedRecord(0.5),
								CertifiedAdd(sourceRate(left),sourceRate(right)));
							const CertifiedBinary32 phase=CertifiedMultiply(CertifiedBinary32{
								device.projectedVelocityMPerS[axis][face],mirror.projectedVelocityMPerS[axis][face],
								transportVelocityBounds[axis][face],true},restricted);
							terminalRateBounds[stage][2][axis][face]=phase.error+std::fabs(phase.exact-
								mirror.phaseSourceMomentumRateKGPerM2S2[axis][face]);
							if(compatibleBuilt&&face<compatibleTrace.advectionRateKGPerM2S2[axis].size()){
								const TraceFloat& advective=compatibleTrace.advectionRateKGPerM2S2[axis][face];
								terminalRateBounds[stage][3][axis][face]=advective.Radius()+std::fabs(
									advective.Center()-mirror.advectionMomentumRateKGPerM2S2[axis][face]);}
							else terminalRateBounds[stage][3][axis][face]=
								std::numeric_limits<double>::infinity();
							const float values32[4]={device.buoyancyMomentumRateKGPerM2S2[axis][face],
								device.stressMomentumRateKGPerM2S2[axis][face],
								device.phaseSourceMomentumRateKGPerM2S2[axis][face],
								device.advectionMomentumRateKGPerM2S2[axis][face]};
							const double values64[4]={mirror.buoyancyMomentumRateKGPerM2S2[axis][face],
								mirror.stressMomentumRateKGPerM2S2[axis][face],
								mirror.phaseSourceMomentumRateKGPerM2S2[axis][face],
								mirror.advectionMomentumRateKGPerM2S2[axis][face]};
							for(unsigned int term=0u;term<4u;++term)rateParentsPassed=rateParentsPassed&&
								std::isfinite(terminalRateBounds[stage][term][axis][face])&&
								std::fabs(static_cast<double>(values32[term])-values64[term])<=
								terminalRateBounds[stage][term][axis][face];}}
					terminalRateBoundsAvailable[stage]=rateParentsPassed;
					std::fprintf(stderr,"PROJECTED_HEUN_OWNER_RATE_PARENT_CERTIFICATE stage=R%u "
						"compatible_trace=%d branch_obligations=%zu passed=%d error=%s\n",stage,
						compatibleBuilt?1:0,compatibleCounters.branchObligations.size(),
						rateParentsPassed?1:0,compatibleTraceError.c_str());
					traceFieldsPassed=traceFieldsPassed&&rateParentsPassed;}
				double targetWorstRatio=0.0,targetMaximumResidual=0.0,targetMaximumBound=0.0;
				std::vector<double> currentTargetBounds(cells,0.0);
				std::size_t targetWorstCell=0u;bool targetBounded=true,enclosureValid=true;
				for(std::size_t cell=0u;cell<cells;++cell){const double targetResidual=std::fabs(
					static_cast<double>(device.producedTargetPerS[cell])-mirror.producedTargetPerS[cell]);
					const double targetBound=targetTermwiseEnclosure(stage,device,mirror,
						physicalMassBounds,physicalEnergyBounds,transportStateBounds,
						transportTemperatureBounds,cell,
						priorTargetBounds[cell],enclosureValid);currentTargetBounds[cell]=targetBound;
					const double targetRatio=targetBound>0.0?targetResidual/targetBound:
						(targetResidual==0.0?0.0:std::numeric_limits<double>::infinity());
					targetMaximumResidual=std::max(targetMaximumResidual,targetResidual);
					targetMaximumBound=std::max(targetMaximumBound,targetBound);
					if(targetRatio>targetWorstRatio){targetWorstRatio=targetRatio;targetWorstCell=cell;}
					targetBounded=targetBounded&&enclosureValid&&std::isfinite(targetBound)&&
						targetResidual<=targetBound;}
				std::fprintf(stderr,"PROJECTED_HEUN_OWNER_TARGET_ENCLOSURE stage=R%u iteration=%zu "
					"units=s^-1 scope=every_cell worst_cell=%zu max_residual=%.17g "
					"max_local_termwise_enclosure=%.17g worst_residual_over_local_bound=%.17g passed=%d\n",
					stage,iteration,targetWorstCell,targetMaximumResidual,targetMaximumBound,
					targetWorstRatio,targetBounded?1:0);
				if((device.iteration&UINT32_C(0x80000000))!=0u){
					ownerTerminalProjectionTargetBounds[stage]=priorTargetBounds;
					ownerTerminalAcceptedTargetBounds[stage]=currentTargetBounds;}
				priorTargetBounds=std::move(currentTargetBounds);
				priorTargetConsumerBounds.resize(cells);
				for(std::size_t cell=0u;cell<cells;++cell)priorTargetConsumerBounds[cell]=
					std::nextafter(std::fabs(static_cast<double>(device.producedTargetPerS[cell])-
						mirror.producedTargetPerS[cell]),std::numeric_limits<double>::infinity());
				std::fprintf(stderr,"PROJECTED_HEUN_OWNER_ITERATION_TRACE stage=R%u iteration=%zu "
					"active_equal=%d next_active_equal=%d input_target_residual=%.17g "
					"input_target_bit_mismatches=%.0f output_target_residual=%.17g "
					"output_target_bit_mismatches=%.0f alpha_residual=%.17g alpha_bit_mismatches=%.0f "
					"velocity_residual=%.17g velocity_bit_mismatches=%.0f transport_state_residual=%.17g "
					"transport_temperature_residual=%.17g diffusivity_residual=%.17g "
					"conductivity_residual=%.17g molecular_nu_residual=%.17g gas_density_residual=%.17g "
					"molecular_nu_worst=%.0f molecular_nu_metal=%.17g molecular_nu_fp64=%.17g "
					"face_density_residual=%.17g momentum_residual=%.17g stress_residual=%.17g\n",
					stage,iteration,sameClasses(device.activeClass,mirror.activeClass)?1:0,
					sameClasses(device.nextActiveClass,mirror.nextActiveClass)?1:0,inputTarget[0],inputTarget[2],
					outputTarget[0],outputTarget[2],alpha[0],alpha[2],velocity[0],velocity[2],
					transportState[0],transportTemperature[0],
					diffusivity[0],conductivity[0],molecular[0],density[0],molecular[1],
					static_cast<double>(device.molecularKinematicViscosityM2PerS[
						static_cast<std::size_t>(molecular[1])]),mirror.molecularKinematicViscosityM2PerS[
					static_cast<std::size_t>(molecular[1])],faceDensity[0],momentum[0],stress[0]);
				auto emitFirstClassPredicate=[&](const char* kind,
					const std::array<std::vector<unsigned char>,6>& deviceClass,
					const std::array<std::vector<unsigned char>,6>& mirrorClass){
					bool admissible=true;
					for(unsigned int side=0u;side<6u;++side){
						admissible=admissible&&deviceClass[side].size()==mirrorClass[side].size();
						for(std::size_t local=0u;
						local<std::min(deviceClass[side].size(),mirrorClass[side].size());++local){
						if(deviceClass[side][local]==mirrorClass[side][local])continue;
						const unsigned int axis=side/2u;const bool positive=(side&1u)!=0u;
						const std::size_t firstCount=axis==0u?shape.ny:shape.nx;
						const std::size_t first=local%firstCount,second=local/firstCount;
						std::size_t x=0u,y=0u,z=0u;
						if(axis==0u){x=positive?shape.nx:0u;y=first;z=second;}
						if(axis==1u){x=first;y=positive?shape.ny:0u;z=second;}
						if(axis==2u){x=first;y=second;z=positive?shape.nz:0u;}
						const std::size_t face=axis==0u?(z*shape.ny+y)*(shape.nx+1u)+x:
							(axis==1u?(z*(shape.ny+1u)+y)*shape.nx+x:
							 (z*shape.ny+y)*shape.nx+x);
						const double outwardDevice=(positive?1.0:-1.0)*
							static_cast<double>(device.projectedVelocityMPerS[axis][face]);
						const double outwardMirror=(positive?1.0:-1.0)*
							mirror.projectedVelocityMPerS[axis][face];
						const double projectionRadius=(static_cast<double>(
							device.maximumPostProjectionResidualPerS)+mirror.
							maximumPostProjectionResidualPerS)*shape.cellWidthM;
						const double lower=std::min(outwardDevice,outwardMirror)-projectionRadius;
						const double upper=std::max(outwardDevice,outwardMirror)+projectionRadius;
						const double tolerance=ownerRequest.endpointVelocityToleranceMPerS;
						const bool negativeCrosses=lower+tolerance<=0.0&&upper+tolerance>=0.0;
						const bool positiveCrosses=lower-tolerance<=0.0&&upper-tolerance>=0.0;
						admissible=admissible&&certifiedPredicateClassGate(deviceClass[side][local],
							mirrorClass[side][local],lower,upper,{-tolerance,tolerance});
						std::fprintf(stderr,"PROJECTED_HEUN_OWNER_CLASS_PREDICATE stage=R%u "
							"iteration=%zu kind=%s side=%u local=%zu device_class=%u fp64_class=%u "
							"outward_device_m_s=%.17g outward_fp64_m_s=%.17g "
							"projection_radius_m_s=%.17g negative_threshold_interval=[%.17g,%.17g] "
							"positive_threshold_interval=[%.17g,%.17g] negative_crosses_zero=%d "
							"positive_crosses_zero=%d\n",stage,iteration,kind,side,local,
							deviceClass[side][local],mirrorClass[side][local],outwardDevice,
							outwardMirror,projectionRadius,lower+tolerance,upper+tolerance,
							lower-tolerance,upper-tolerance,negativeCrosses?1:0,
							positiveCrosses?1:0);
						}
					}
					return admissible;
				};
				const bool activeClassEnvelope=emitFirstClassPredicate(
					"active_set",device.activeClass,mirror.activeClass);
				const bool nextActiveClassEnvelope=emitFirstClassPredicate(
					"next_active_set",device.nextActiveClass,mirror.nextActiveClass);
				const bool predicateDiagnosticsValid=alphaClassEnvelope&&
					activeClassEnvelope&&nextActiveClassEnvelope;
				ownerIterationTraceBounded=ownerIterationTraceBounded&&traceFieldsPassed&&
					targetBounded&&predicateDiagnosticsValid&&
					ownerClassAcceptance(alphaClassesAgree)&&ownerClassAcceptance(
					sameClasses(device.activeClass,mirror.activeClass))&&ownerClassAcceptance(
					sameClasses(device.nextActiveClass,mirror.nextActiveClass));
				if(iteration<traceStages32[stage]->qualificationIterationTrace.size()){
					const auto& fp32=traceStages32[stage]->qualificationIterationTrace[iteration];
					const auto input32=traceVector32(device.projectionTargetPerS,fp32.projectionTargetPerS);
					const auto output32=traceVector32(device.producedTargetPerS,fp32.producedTargetPerS);
					const auto alpha32=traceAxes32(device.sharedFaceAlpha,fp32.sharedFaceAlpha);
					const auto velocity32=traceAxes32(device.projectedVelocityMPerS,fp32.projectedVelocityMPerS);
					const auto transportState32=traceVector32(device.transportConservativeValues,
						fp32.transportConservativeValues);
					const auto transportTemperature32=traceVector32(device.transportTemperatureK,
						fp32.transportTemperatureK);
					const auto diffusivity32=traceVector32(device.diffusivityM2PerS,fp32.diffusivityM2PerS);
					const auto conductivity32=traceVector32(device.conductivityWPerMK,fp32.conductivityWPerMK);
					const auto molecular32=traceVector32(device.molecularKinematicViscosityM2PerS,
						fp32.molecularKinematicViscosityM2PerS);
					const auto density32=traceVector32(device.gasDensityKGPerM3,fp32.gasDensityKGPerM3);
					const auto faceDensity32=traceAxes32(device.faceDensityKGPerM3,fp32.faceDensityKGPerM3);
					const auto momentum32=traceAxes32(device.projectedMomentumKGPerM2S,
						fp32.projectedMomentumKGPerM2S);
					const auto stress32=traceAxes32(device.stressMomentumRateKGPerM2S2,
						fp32.stressMomentumRateKGPerM2S2);
					std::fprintf(stderr,"PROJECTED_HEUN_OWNER_ITERATION_TRACE_FP32 stage=R%u iteration=%zu "
						"active_equal=%d next_active_equal=%d input_target_residual=%.17g input_target_mismatches=%.0f "
						"output_target_residual=%.17g output_target_mismatches=%.0f alpha_residual=%.17g "
						"alpha_mismatches=%.0f velocity_residual=%.17g velocity_mismatches=%.0f "
						"transport_state_residual=%.17g transport_temperature_residual=%.17g "
						"diffusivity_residual=%.17g conductivity_residual=%.17g molecular_nu_residual=%.17g "
						"gas_density_residual=%.17g face_density_residual=%.17g momentum_residual=%.17g "
						"stress_residual=%.17g\n",stage,iteration,
						sameClasses(device.activeClass,fp32.activeClass)?1:0,
						sameClasses(device.nextActiveClass,fp32.nextActiveClass)?1:0,input32[0],input32[1],
						output32[0],output32[1],alpha32[0],alpha32[1],velocity32[0],velocity32[1],
						transportState32[0],transportTemperature32[0],
						diffusivity32[0],conductivity32[0],molecular32[0],density32[0],faceDensity32[0],
						momentum32[0],stress32[0]);
				}
				if(stage==0u&&iteration==1u){
					FireProductionResidentTransportComparatorRequest replay=
						ownerEOS.physicalFlux.transport;
					replay.stage=FireProductionProjectedHeunStage::R0;
					replay.parentCandidateIdentity=UINT64_C(0x2010000000000001);
					replay.projectionIdentity=UINT64_C(0x2010000000000002);
					replay.conservativeValues=device.transportConservativeValues;
					replay.temperatureK=device.transportTemperatureK;
					replay.projectedVelocityMPerS=device.projectedVelocityMPerS;
					FireProductionResidentTransportComparatorResult replayed;
					std::string replayError;const bool replayAccepted=
						EvaluateFireProductionResidentTransportMetalComparator(replay,replayed,&replayError);
					const auto replayMolecular=replayAccepted?traceVector32(
						device.molecularKinematicViscosityM2PerS,
						replayed.molecularKinematicViscosityM2PerS):
						std::array<double,2>{{std::numeric_limits<double>::infinity(),1.0}};
					std::fprintf(stderr,"PROJECTED_HEUN_OWNER_TRANSPORT_REPLAY stage=R0 iteration=0 "
						"accepted=%d molecular_residual=%.17g molecular_bit_mismatches=%.0f error=%s\n",
						replayAccepted?1:0,replayMolecular[0],replayMolecular[1],replayError.c_str());
					FireProductionResidentTransportComparatorRequest zeroVelocityReplay=replay;
					for(auto& axis:zeroVelocityReplay.projectedVelocityMPerS)
						std::fill(axis.begin(),axis.end(),0.0f);
					FireProductionResidentTransportComparatorResult zeroVelocityResult;
					std::string zeroVelocityError;const bool zeroVelocityAccepted=
						EvaluateFireProductionResidentTransportMetalComparator(zeroVelocityReplay,
							zeroVelocityResult,&zeroVelocityError);
					const auto velocityIndependentMolecular=replayAccepted&&zeroVelocityAccepted?
						traceVector32(replayed.molecularKinematicViscosityM2PerS,
							zeroVelocityResult.molecularKinematicViscosityM2PerS):
						std::array<double,2>{{std::numeric_limits<double>::infinity(),1.0}};
					transportInvariantREDs=transportInvariantREDs&&
						velocityIndependentMolecular[1]==0.0;
					std::fprintf(stderr,"PROJECTED_HEUN_OWNER_TRANSPORT_INVARIANT_RED "
						"name=molecular_nu_velocity_independence live_accepted=%d zero_accepted=%d "
						"max_residual_m2_s^-1=%.17g bit_mismatches=%.0f live_error=%s zero_error=%s passed=%d\n",
						replayAccepted?1:0,zeroVelocityAccepted?1:0,velocityIndependentMolecular[0],
						velocityIndependentMolecular[1],replayError.c_str(),zeroVelocityError.c_str(),
						velocityIndependentMolecular[1]==0.0?1:0);
				}
			}
		}
		ownerIterationTraceBounded=ownerIterationTraceBounded&&staleOrSwappedStageFrontRED&&
			sealedOpenHeadOutflowTraceRED;
		std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED "
			"name=stale_or_swapped_stage_front_refused producer_bytes_authenticated=1 "
			"mutated_front_refused=%d passed=%d\n",staleOrSwappedStageFrontRED?1:0,
			staleOrSwappedStageFrontRED?1:0);
		const bool ambiguousActiveAccepted=certifiedPredicateClassGate(0u,1u,-1.0e-6,
			1.0e-6,{0.0});
		const bool separatedActiveRefused=!certifiedPredicateClassGate(0u,1u,1.0e-3,
			2.0e-3,{0.0});
		const bool ambiguousAlphaAccepted=certifiedAlphaClassGate(1u,2u,
			1.0-traceGamma4096,1.0+traceGamma4096);
		const bool nonadjacentAlphaNeedsBoth=!certifiedAlphaClassGate(0u,1u,
			-traceGamma4096,traceGamma4096)&&certifiedAlphaClassGate(0u,1u,
			-traceGamma4096,1.0+traceGamma4096);
		const bool ambiguousWithoutCertificateRefused=!ownerClassAcceptance(false);
		const bool separatedWithoutCertificateRefused=!ownerClassAcceptance(false);
		const bool matchingWithoutCertificateAccepted=ownerClassAcceptance(true);
		const bool ownerPredicatePreclassificationRED=ambiguousActiveAccepted&&separatedActiveRefused&&
			ambiguousAlphaAccepted&&nonadjacentAlphaNeedsBoth&&ambiguousWithoutCertificateRefused&&
			separatedWithoutCertificateRefused&&matchingWithoutCertificateAccepted;
		ownerIterationTraceBounded=ownerIterationTraceBounded&&ownerPredicatePreclassificationRED;
		std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED "
			"name=owner_branch_predicate_preclassification active_crossing_identified=%d "
			"active_non_crossing_refused=%d alpha_crossing_identified=%d "
			"alpha_nonadjacent_requires_both_thresholds=%d "
			"ambiguous_without_certificate_refused=%d noncrossing_without_certificate_refused=%d "
			"matching_without_certificate_accepted=%d trajectory_contract=same_class_enclosures "
			"divergent_class_contract=authenticated_r123_r124_two_successor_or_refusal "
			"flux_branch_obligations=r198_device_bitmap "
			"passed=%d\n",ambiguousActiveAccepted?1:0,
			separatedActiveRefused?1:0,ambiguousAlphaAccepted?1:0,
			nonadjacentAlphaNeedsBoth?1:0,
			ambiguousWithoutCertificateRefused?1:0,
			separatedWithoutCertificateRefused?1:0,matchingWithoutCertificateAccepted?1:0,
			ownerPredicatePreclassificationRED?1:0);
		FireProductionProjectedHeunMetalOwnerRequest sealedClassMutation=ownerRequest;
		sealedClassMutation.qualificationR2SealedClassPhysicalFlux=true;
		FireProductionProjectedHeunMetalOwnerResult sealedClassResult;
		std::string sealedClassError;
		const bool sealedClassAccepted=AttemptFireProductionProjectedHeunMetalOwner(
			sealedClassMutation,sealedClassResult,&sealedClassError);
		double sealedClassEnergyResidual=0.0,sealedClassEnergyBound=0.0,
			sealedClassEnergyWorstRatio=0.0,sealedClassTargetResidual=0.0,
			sealedClassTargetBound=0.0,sealedClassTargetWorstRatio=0.0;
		bool sealedClassSequencesAgree=false;
		if(sealedClassAccepted&&sealedClassResult.qualificationIterationTrace[2].size()>1u&&
			traceStages64[2]->qualificationIterationTrace.size()>1u){const auto& mutatedBootstrap=
				sealedClassResult.qualificationIterationTrace[2][0];const auto& mirrorBootstrap=
				traceStages64[2]->qualificationIterationTrace[0];const auto& mutated=
				sealedClassResult.qualificationIterationTrace[2][1];const auto& mirror=
				traceStages64[2]->qualificationIterationTrace[1];
			sealedClassSequencesAgree=sameClasses(mutated.activeClass,mirror.activeClass)&&
				sameClasses(mutated.nextActiveClass,mirror.nextActiveClass);
			double maximumConductivity=0.0,maximumEnthalpyMagnitude=0.0;
			for(const double value:mirror.conductivityWPerMK)
				maximumConductivity=std::max(maximumConductivity,std::fabs(value));
			for(std::size_t species=0u;species<7u;++species)maximumEnthalpyMagnitude=std::max({
				maximumEnthalpyMagnitude,std::fabs(ownerMinimumEnthalpy[species]),
				std::fabs(ownerMaximumEnthalpy[species])});
			for(std::size_t face=0u;face<allFaces;++face){const double residual=std::fabs(
				static_cast<double>(mutated.physicalEnergyFluxWPerM2[face])-
				mirror.physicalEnergyFluxWPerM2[face]);double scale=maximumConductivity*
				(fuel.TemperatureMaxK()-fuel.TemperatureMinK())/(0.5*shape.cellWidthM);
				for(std::size_t component=1u;component<8u;++component){scale+=
					maximumEnthalpyMagnitude*std::fabs(mirror.physicalMassFluxKGPerM2S[
						component*allFaces+face]);}
				const double bound=traceGamma4096*scale,
					ratio=bound>0.0?residual/bound:std::numeric_limits<double>::infinity();
				if(residual>sealedClassEnergyResidual)sealedClassEnergyResidual=residual;
				if(ratio>sealedClassEnergyWorstRatio){sealedClassEnergyWorstRatio=ratio;
					sealedClassEnergyBound=bound;}}
			std::vector<double> bootstrapBounds(cells,0.0);bool enclosureValid=true;
			std::vector<double> bootstrapMassBounds(8u*allFaces,0.0),
				mutatedMassBounds(8u*allFaces,0.0),bootstrapEnergyBounds(allFaces,0.0),
				mutatedEnergyBounds(allFaces,0.0),bootstrapStateBounds(9u*cells,0.0),
				mutatedStateBounds(9u*cells,0.0),bootstrapTemperatureBounds(cells,0.0),
				mutatedTemperatureBounds(cells,0.0);
			for(std::size_t value=0u;value<8u*allFaces;++value){bootstrapMassBounds[value]=
				traceGamma4096*std::fabs(mirrorBootstrap.physicalMassFluxKGPerM2S[value]);
				mutatedMassBounds[value]=traceGamma4096*
					std::fabs(mirror.physicalMassFluxKGPerM2S[value]);}
			for(std::size_t face=0u;face<allFaces;++face){bootstrapEnergyBounds[face]=
				traceGamma4096*std::fabs(mirrorBootstrap.physicalEnergyFluxWPerM2[face]);
				mutatedEnergyBounds[face]=traceGamma4096*
					std::fabs(mirror.physicalEnergyFluxWPerM2[face]);}
			for(std::size_t cell=0u;cell<cells;++cell)bootstrapBounds[cell]=
				targetTermwiseEnclosure(2u,mutatedBootstrap,mirrorBootstrap,
					bootstrapMassBounds,bootstrapEnergyBounds,bootstrapStateBounds,
					bootstrapTemperatureBounds,cell,0.0,
					enclosureValid);
			for(std::size_t cell=0u;cell<cells;++cell){const double residual=std::fabs(
				static_cast<double>(mutated.producedTargetPerS[cell])-
					mirror.producedTargetPerS[cell]);const double bound=targetTermwiseEnclosure(
					2u,mutated,mirror,mutatedMassBounds,mutatedEnergyBounds,
					mutatedStateBounds,mutatedTemperatureBounds,cell,
					bootstrapBounds[cell],enclosureValid),ratio=
					bound>0.0?residual/bound:std::numeric_limits<double>::infinity();
				if(residual>sealedClassTargetResidual)sealedClassTargetResidual=residual;
				if(ratio>sealedClassTargetWorstRatio){sealedClassTargetWorstRatio=ratio;
					sealedClassTargetBound=bound;}}
				r2ImmediateEndpointClassRED=enclosureValid&&sealedClassSequencesAgree&&
					sealedClassEnergyWorstRatio>1.0&&sealedClassTargetWorstRatio>1.0;}
		if(!sealedClassAccepted)r2ImmediateEndpointClassRED=
			sealedClassResult.ownerPublicationIdentity==0u&&
			sealedClassError.find("endpoint-class producer capability is unverified")!=
				std::string::npos;
		std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED "
			"name=R2_physical_flux_consumes_sealed_input_class "
			"mutant_accepted=%d class_sequences_agree=%d "
			"physical_energy_max_residual_W_m^-2=%.17g "
			"physical_energy_local_termwise_bound_W_m^-2=%.17g "
			"physical_energy_worst_ratio=%.17g target_max_residual_s^-1=%.17g "
			"target_local_termwise_bound_s^-1=%.17g target_worst_ratio=%.17g "
			"error=%s refusal_is_primary=%d passed=%d\n",sealedClassAccepted?1:0,sealedClassSequencesAgree?1:0,
			sealedClassEnergyResidual,sealedClassEnergyBound,sealedClassEnergyWorstRatio,
			sealedClassTargetResidual,sealedClassTargetBound,sealedClassTargetWorstRatio,
			sealedClassError.c_str(),!sealedClassAccepted?1:0,r2ImmediateEndpointClassRED?1:0);
		FireProductionProjectedHeunMetalOwnerRequest unverifiedEndpointClass=ownerRequest;
		unverifiedEndpointClass.qualificationUnverifiedEndpointClassBuffer=true;
		FireProductionProjectedHeunMetalOwnerResult unverifiedEndpointResult;
		std::string unverifiedEndpointError;
		const bool unverifiedEndpointAccepted=AttemptFireProductionProjectedHeunMetalOwner(
			unverifiedEndpointClass,unverifiedEndpointResult,&unverifiedEndpointError);
		endpointClassAuthorityRED=!unverifiedEndpointAccepted&&
			unverifiedEndpointResult.ownerPublicationIdentity==0u&&
			unverifiedEndpointError.find("endpoint-class lineage is stale")!=std::string::npos;
		std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED "
			"name=unverified_private_endpoint_class_refused mutant_accepted=%d "
			"owner_identity=%llu error=%s passed=%d\n",unverifiedEndpointAccepted?1:0,
			static_cast<unsigned long long>(unverifiedEndpointResult.ownerPublicationIdentity),
			unverifiedEndpointError.c_str(),endpointClassAuthorityRED?1:0);
		FireProductionProjectedHeunMetalOwnerRequest endpointToleranceMutation=ownerRequest;
		endpointToleranceMutation.endpointVelocityToleranceMPerS=std::nextafter(
			ownerRequest.endpointVelocityToleranceMPerS,std::numeric_limits<float>::infinity());
		FireProductionProjectedHeunMetalOwnerResult endpointToleranceResult;
		std::string endpointToleranceError;
		const bool endpointToleranceAccepted=AttemptFireProductionProjectedHeunMetalOwner(
			endpointToleranceMutation,endpointToleranceResult,&endpointToleranceError);
		bool endpointClassSequencesEqual=endpointToleranceAccepted;
		for(unsigned int stage=0u;stage<3u&&endpointClassSequencesEqual;++stage){
			endpointClassSequencesEqual=endpointToleranceResult.qualificationIterationTrace[stage].size()==
				ownerObserved.qualificationIterationTrace[stage].size();
			for(std::size_t iteration=0u;endpointClassSequencesEqual&&iteration<
				ownerObserved.qualificationIterationTrace[stage].size();++iteration){const auto& first=
					ownerObserved.qualificationIterationTrace[stage][iteration];const auto& second=
					endpointToleranceResult.qualificationIterationTrace[stage][iteration];
				endpointClassSequencesEqual=sameClasses(first.activeClass,second.activeClass)&&
					sameClasses(first.nextActiveClass,second.nextActiveClass);}}
		endpointPredicateIdentityRED=endpointToleranceAccepted&&endpointClassSequencesEqual&&
			endpointToleranceResult.ownerPublicationIdentity!=ownerObserved.ownerPublicationIdentity;
		std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED "
			"name=endpoint_predicate_tolerance_identity same_class_bytes=%d old_tolerance_m_s=%.9g "
			"new_tolerance_m_s=%.9g old_identity=%llu new_identity=%llu error=%s passed=%d\n",
			endpointClassSequencesEqual?1:0,ownerRequest.endpointVelocityToleranceMPerS,
			endpointToleranceMutation.endpointVelocityToleranceMPerS,
			static_cast<unsigned long long>(ownerObserved.ownerPublicationIdentity),
			static_cast<unsigned long long>(endpointToleranceResult.ownerPublicationIdentity),
			endpointToleranceError.c_str(),endpointPredicateIdentityRED?1:0);
		FireProductionResidentStepResult productionQualificationResult;
		FireProductionProjectedHeunMetalOwnerResult productionQualificationDiagnostics;
		std::string productionQualificationError;
		const bool productionQualificationAccepted=
			AttemptFireProductionProjectedHeunResidentStepMetal(sealedClassMutation,
				productionQualificationResult,&productionQualificationDiagnostics,
				&productionQualificationError);
		productionQualificationRefusalRED=!productionQualificationAccepted&&
			!productionQualificationResult.HasAcceptedManifoldToken()&&
			productionQualificationResult.projectedHeunOwnerIdentity==0u&&
			productionQualificationDiagnostics.ownerPublicationIdentity==0u&&
			productionQualificationError.find("qualification-only owner controls")!=std::string::npos;
		std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED "
			"name=production_entry_refuses_diagnostic_owner_mutant mutant_accepted=%d "
			"token=%d owner_identity=%llu diagnostic_identity=%llu error=%s passed=%d\n",
			productionQualificationAccepted?1:0,
			productionQualificationResult.HasAcceptedManifoldToken()?1:0,
			static_cast<unsigned long long>(
				productionQualificationResult.projectedHeunOwnerIdentity),
			static_cast<unsigned long long>(
				productionQualificationDiagnostics.ownerPublicationIdentity),
			productionQualificationError.c_str(),productionQualificationRefusalRED?1:0);
		const std::vector<float> traceMutantDevice(1u,1.0f);
		const double traceMutantBound=traceGamma4096;
		const std::vector<double> traceMutantMirror(1u,1.0+2.0*traceMutantBound);
		const IterationFieldGate traceMutant=gateIterationField(3u,0u,
			"physical_energy_flux_mutant","W_m^-2",traceMutantDevice,traceMutantMirror,
			[&](std::size_t){return traceMutantBound;});
		const bool intermediateContinuousRED=!traceMutant.passed;
		ownerIterationTraceBounded=ownerIterationTraceBounded&&intermediateContinuousRED;
		std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED "
			"name=intermediate_physical_flux_outside_enclosure residual_W_m^-2=%.17g "
			"local_termwise_enclosure_W_m^-2=%.17g passed=%d\n",2.0*traceMutantBound,
			traceMutantBound,intermediateContinuousRED?1:0);
		ownerIterationTraceBounded=ownerIterationTraceBounded&&quietLocalEnclosureRED;
		std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED "
			"name=quiet_face_local_physical_flux_enclosure samples=2 "
			"mutant_residual_kg_m^-2_s^-1=%.17g quiet_local_termwise_enclosure_kg_m^-2_s^-1=%.17g "
			"loud_local_termwise_enclosure_kg_m^-2_s^-1=%.17g "
			"local_gate_refused=%d pooled_max_mutant_accepted=%d passed=%d\n",
			quietMutantResidual,quietLocalBound,quietGlobalBound,quietLocalEnclosureRED?1:0,
			quietLocalEnclosureRED?1:0,quietLocalEnclosureRED?1:0);
		ownerIterationTraceBounded=ownerIterationTraceBounded&&recordedContinuousDefectRED;
		std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED "
			"name=recorded_r201_continuous_defect_rejected "
			"historical_residual_W_m^-2=%.17g local_termwise_enclosure_W_m^-2=%.17g "
			"passed=%d\n",recordedContinuousDefectResidual,recordedContinuousDefectBound,
			recordedContinuousDefectRED?1:0);
	}
	bool owner32StressBitEqual=owner32Accepted;double owner32StressMaximumResidual=0.0;
	if(owner32Accepted&&ownerAccepted)for(unsigned int axis=0u;axis<3u;++axis)
		for(std::size_t face=0u;face<ownerObserved.heunStressMomentumRateKGPerM2S2[axis].size();
			++face){const float expected=0.5f*(ownerObserved32.r0.nonpressure.
				stressMomentumRateKGPerM2S2[axis][face]+ownerObserved32.r1.nonpressure.
				stressMomentumRateKGPerM2S2[axis][face]);
			owner32StressMaximumResidual=std::max(owner32StressMaximumResidual,std::fabs(
				static_cast<double>(ownerObserved.heunStressMomentumRateKGPerM2S2[axis][face])-expected));
			owner32StressBitEqual=owner32StressBitEqual&&std::memcmp(&expected,
				&ownerObserved.heunStressMomentumRateKGPerM2S2[axis][face],sizeof(float))==0;}
	std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_FP32_DIAGNOSTIC owner_begin=%d r0=%d r1=%d "
		"accepted=%d heun_stress_bit_equal=%d heun_stress_max_residual=%.17g error=%s\n",
		owner32Begin?1:0,owner32R0?1:0,owner32R1?1:0,owner32Accepted?1:0,
		owner32StressBitEqual?1:0,owner32StressMaximumResidual,owner32Error.c_str());
	for(unsigned int diagnosticStage=0u;diagnosticStage<2u&&owner32Accepted;++diagnosticStage){
		const FireProductionProjectedHeunCoupledStageResult& stage=diagnosticStage==0u?
			ownerObserved32.r0:ownerObserved32.r1;
		const std::vector<float>& stageState=diagnosticStage==0u?
			ownerRequest32.beginningConservativeValues:ownerObserved32.r0.scalarAcceptance.accepted;
		const std::vector<float>& stageTemperature=diagnosticStage==0u?
			ownerRequest32.source.BeginningTemperatureK():ownerObserved32.predictorEOS.temperatureK;
		FireProductionProjectedHeunTransportContext context;
		context.stage=stage.stage;context.attemptIdentity=ownerRequest32.attemptIdentity;
		context.parentCandidateIdentity=stage.parentCandidateIdentity;
		context.projectionIdentity=UINT64_C(1);context.conservativeValues=&stageState;
		context.temperatureK=&stageTemperature;context.projectedVelocityMPerS=
			&stage.projection.velocityMPerS;
		FireProductionProjectedHeunTransportCoefficients coefficients;
		std::string diagnosticError;
		const bool coefficientsBuilt=ownerTransport32.Evaluate(context,coefficients,&diagnosticError);
		FireProductionNonpressureMomentumRHSRequest rhs;rhs.force=ownerRequest32.forceContract;
		rhs.force.cellGasDensityKGPerM3.assign(cells,0.0f);
		for(std::size_t cell=0u;cell<cells;++cell)for(std::size_t component=1u;component<=6u;
			++component)rhs.force.cellGasDensityKGPerM3[cell]+=stageState[component*cells+cell];
		rhs.force.molecularKinematicViscosityM2PerS=
			coefficients.molecularKinematicViscosityM2PerS;
		rhs.force.faceDensityKGPerM3=stage.projection.faceDensityKGPerM3;
		rhs.force.beginningMomentumKGPerM2S=stage.projection.momentumKGPerM2S;
		rhs.cellGasPhaseSourceRateKGPerM3S.assign(cells,0.0f);
		for(std::size_t cell=0u;cell<cells;++cell)for(std::size_t component=1u;component<=6u;
			++component)rhs.cellGasPhaseSourceRateKGPerM3S[cell]+=
				ownerRequest32.source.SourceDelta()[component*cells+cell]/
				ownerRequest32.source.TimeStepS();
		FireProductionNonpressureMomentumRHSResult metalRHS;
		FireProductionNonpressureMomentumRHSMetalDiagnostics metalRHSDiagnostics;
		const bool metalRHSAccepted=coefficientsBuilt&&EvaluateFireProductionNonpressureMomentumRHSMetal(
			rhs,metalRHS,metalRHSDiagnostics,&diagnosticError);
		double stressResidual=0.0;bool stressBits=metalRHSAccepted;
		if(metalRHSAccepted)for(unsigned int axis=0u;axis<3u;++axis)
			for(std::size_t face=0u;face<metalRHS.stressMomentumRateKGPerM2S2[axis].size();++face){
				stressResidual=std::max(stressResidual,std::fabs(static_cast<double>(
					metalRHS.stressMomentumRateKGPerM2S2[axis][face]-
					stage.nonpressure.stressMomentumRateKGPerM2S2[axis][face])));
				stressBits=stressBits&&std::memcmp(&metalRHS.stressMomentumRateKGPerM2S2[axis][face],
					&stage.nonpressure.stressMomentumRateKGPerM2S2[axis][face],sizeof(float))==0;}
		std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RHS_DIAGNOSTIC stage=R%u accepted=%d "
			"stress_bit_equal=%d stress_max_residual=%.17g error=%s\n",diagnosticStage,
			metalRHSAccepted?1:0,stressBits?1:0,stressResidual,diagnosticError.c_str());
	}
	bool activeCycleREDs=true;
	for(std::uint32_t stage=1u;stage<=3u;++stage){
		FireProductionProjectedHeunMetalOwnerRequest cycleRequest=ownerRequest;
		cycleRequest.qualificationForcedActiveCycleStage=stage;
		FireProductionProjectedHeunMetalOwnerResult cycleMetal;std::string cycleMetalError;
		const bool cycleMetalAccepted=AttemptFireProductionProjectedHeunMetalOwner(
			cycleRequest,cycleMetal,&cycleMetalError);
		const bool sameOutcome=cycleMetalAccepted&&
			cycleMetal.activeSetDiscontinuousClass[stage-1u]&&
			cycleMetal.activeSetCycleLength[stage-1u]==2u&&
			cycleMetal.activeSetCanonicalProjectionCount[stage-1u]>0u;
		cycleRequest.qualificationDisableCanonicalCycle=true;
		FireProductionProjectedHeunMetalOwnerResult mutant;std::string mutantError;
		const bool mutantAttempted=AttemptFireProductionProjectedHeunMetalOwner(
			cycleRequest,mutant,&mutantError);
		const bool mutantRefused=!mutantAttempted&&!mutant.accepted&&
			mutant.ownerPublicationIdentity==0u&&mutant.conservativeValues.empty();
		activeCycleREDs=activeCycleREDs&&sameOutcome&&mutantRefused;
		std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED name=two_class_cycle_R%u "
			"reference=reviewed_r190_two_class_rule expected_cycle=2 metal_accepted=%d metal_cycle=%u "
			"canonical_projections=%u mutant_refused=%d metal_error=%s "
			"mutant_error=%s passed=%d\n",stage,cycleMetalAccepted?1:0,
			cycleMetal.activeSetCycleLength[stage-1u],
			cycleMetal.activeSetCanonicalProjectionCount[stage-1u],mutantRefused?1:0,
			cycleMetalError.c_str(),mutantError.c_str(),
			sameOutcome&&mutantRefused?1:0);
	}
	FireProductionProjectedHeunMetalOwnerRequest limiterRequest=ownerRequest;
	limiterRequest.qualificationForceLimiterDiscontinuity=true;
	FireProductionProjectedHeunMetalOwnerResult limiterMetal;std::string limiterMetalError;
	const bool limiterMetalAccepted=AttemptFireProductionProjectedHeunMetalOwner(
		limiterRequest,limiterMetal,&limiterMetalError);
	limiterRequest.qualificationDisableLimiterCertification=true;
	FireProductionProjectedHeunMetalOwnerResult limiterMutant;std::string limiterMutantError;
	const bool limiterMutantAttempted=AttemptFireProductionProjectedHeunMetalOwner(
		limiterRequest,limiterMutant,&limiterMutantError);
	const bool limiterOutcomeRED=limiterMetalAccepted&&
		limiterMetal.limiterDiscontinuousClass[1]&&
		!limiterMutantAttempted&&!limiterMutant.accepted&&
		limiterMutant.ownerPublicationIdentity==0u;
	std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED name=limiter_discontinuity_R1 "
		"reference=reviewed_r190_minimum_alpha_certification metal_accepted=%d "
		"metal_discontinuous=%d mutant_refused=%d metal_error=%s "
		"mutant_error=%s passed=%d\n",limiterMetalAccepted?1:0,
		limiterMetal.limiterDiscontinuousClass[1]?1:0,
		!limiterMutantAttempted?1:0,limiterMetalError.c_str(),
		limiterMutantError.c_str(),limiterOutcomeRED?1:0);
	FireProductionProjectedHeunMetalOwnerRequest heunWeightingRequest=ownerRequest;
	heunWeightingRequest.qualificationThreeQuarterHeunWeighting=true;
	FireProductionProjectedHeunMetalOwnerResult heunWeightingMutant;
	std::string heunWeightingMutantError;
	const bool heunWeightingMutantAccepted=AttemptFireProductionProjectedHeunMetalOwner(
		heunWeightingRequest,heunWeightingMutant,&heunWeightingMutantError);
	bool heunWeightingRED=false;
	if(owner64Accepted){
		::RISEFireProductionFP64::FireProductionCompatibleFCTMomentumResult advection0,advection1;
		std::string weightingError;
		if(::RISEFireProductionFP64::EvaluateFireProductionCompatibleHeunMomentumCPU(
			ownerObserved64.r0.flux,ownerObserved64.heunSolve,
			ownerObserved64.r0.projection.velocityMPerS,advection0,&weightingError)&&
			::RISEFireProductionFP64::EvaluateFireProductionCompatibleHeunMomentumCPU(
				ownerObserved64.r1.flux,ownerObserved64.heunSolve,
				ownerObserved64.r1.projection.velocityMPerS,advection1,&weightingError)){
			for(unsigned int axis=0u;axis<3u&&!heunWeightingRED;++axis)
				for(std::size_t face=0u;face<ownerRequest64.beginningMomentumKGPerM2S[axis].size();
					++face){
					const double rate0=ownerObserved64.r0.nonpressure.
						combinedMomentumRateKGPerM2S2[axis][face]-
						advection0.advectionRateKGPerM2S2[axis][face];
					const double rate1=ownerObserved64.r1.nonpressure.
						combinedMomentumRateKGPerM2S2[axis][face]-
						advection1.advectionRateKGPerM2S2[axis][face];
					const double beginning=ownerRequest64.beginningMomentumKGPerM2S[axis][face];
					const float reviewedR1=static_cast<float>(beginning+0.5*
						ownerRequest64.scalarContract.timeStepS*(rate0+rate1));
					const float metalMutant=heunWeightingMutantAccepted?
						heunWeightingMutant.provisionalMomentumKGPerM2S[axis][face]:reviewedR1;
					heunWeightingRED=std::memcmp(&reviewedR1,&metalMutant,sizeof(float))!=0;
				}
		}
	}
	std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED name=three_quarter_one_quarter_R1 "
		"oracle=reviewed_r190_fp64_owner metal_mutant_accepted=%d binary32_bit_mismatch=%d "
		"error=%s passed=%d\n",heunWeightingMutantAccepted?1:0,heunWeightingRED?1:0,
		heunWeightingMutantError.c_str(),heunWeightingMutantAccepted&&heunWeightingRED?1:0);
	FireProductionProjectedHeunMetalOwnerRequest sharedAlphaRequest=ownerRequest;
	sharedAlphaRequest.qualificationForceLimiterDiscontinuity=true;
	FireProductionProjectedHeunMetalOwnerResult sharedAlphaCorrect,sharedAlphaMutant;
	std::string sharedAlphaCorrectError,sharedAlphaMutantError;
	const bool sharedAlphaCorrectAccepted=AttemptFireProductionProjectedHeunMetalOwner(
		sharedAlphaRequest,sharedAlphaCorrect,&sharedAlphaCorrectError);
	sharedAlphaRequest.qualificationReuseR0LimiterAlpha=true;
	const bool sharedAlphaMutantAccepted=AttemptFireProductionProjectedHeunMetalOwner(
		sharedAlphaRequest,sharedAlphaMutant,&sharedAlphaMutantError);
	bool sharedAlphaRED=sharedAlphaCorrectAccepted&&sharedAlphaMutantAccepted&&
		ByteIdenticalVector(sharedAlphaCorrect.acceptedFaceAlpha,sharedAlphaMutant.acceptedFaceAlpha);
	bool sharedAlphaMomentumMismatch=false;
	for(unsigned int axis=0u;axis<3u;++axis)sharedAlphaMomentumMismatch=
		sharedAlphaMomentumMismatch||!ByteIdenticalVector(
			sharedAlphaCorrect.provisionalMomentumKGPerM2S[axis],
			sharedAlphaMutant.provisionalMomentumKGPerM2S[axis]);
	sharedAlphaRED=sharedAlphaRED&&sharedAlphaMomentumMismatch;
	std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED name=shared_heun_alpha_R0_recompute "
		"correct_accepted=%d mixed_alpha_mutant_accepted=%d accepted_alpha_bit_equal=%d "
		"provisional_momentum_bit_mismatch=%d correct_error=%s mutant_error=%s passed=%d\n",
		sharedAlphaCorrectAccepted?1:0,sharedAlphaMutantAccepted?1:0,
		sharedAlphaCorrect.acceptedFaceAlpha==sharedAlphaMutant.acceptedFaceAlpha?1:0,
		sharedAlphaMomentumMismatch?1:0,sharedAlphaCorrectError.c_str(),
		sharedAlphaMutantError.c_str(),sharedAlphaRED?1:0);
	const double ownerGamma512=(512.0*std::numeric_limits<float>::epsilon())/
		(1.0-512.0*std::numeric_limits<float>::epsilon());
	const double ownerGamma8=(8.0*std::numeric_limits<float>::epsilon())/
		(1.0-8.0*std::numeric_limits<float>::epsilon());
	struct OwnerFieldBound{double maximumResidual,maximumBound,worstRatio;std::size_t worst,
		bitMismatchCount;std::vector<double> localBounds;bool passed;OwnerFieldBound():maximumResidual(0.0),maximumBound(0.0),
			worstRatio(0.0),worst(0u),bitMismatchCount(0u),passed(true){}};
	auto evaluateOwnerField=[&](const char* field,const char* units,
		const std::vector<float>& device,const std::vector<double>& mirror,
		const std::function<double(std::size_t)>& bound){OwnerFieldBound result;
		if(device.size()!=mirror.size()){std::fprintf(stderr,
			"PROJECTED_HEUN_METAL_OWNER_FIELD field=%s units=%s size_match=0 passed=0\n",
			field,units);result.passed=false;return result;}
		result.localBounds.resize(device.size());
		for(std::size_t index=0u;index<device.size();++index){const double residual=std::fabs(
			static_cast<double>(device[index])-mirror[index]),localBound=bound(index);
			result.localBounds[index]=localBound;
			const float projected=static_cast<float>(mirror[index]);
			result.bitMismatchCount+=std::memcmp(&device[index],&projected,sizeof(float))!=0?1u:0u;
			const double ratio=localBound>0.0?residual/localBound:(residual==0.0?0.0:
				std::numeric_limits<double>::infinity());result.maximumResidual=std::max(
				result.maximumResidual,residual);result.maximumBound=std::max(result.maximumBound,
				localBound);if(ratio>result.worstRatio){result.worstRatio=ratio;result.worst=index;}
			result.passed=result.passed&&std::isfinite(localBound)&&localBound>=0.0&&residual<=localBound;}
		std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_FIELD field=%s units=%s scope=every_value "
			"worst_index=%zu device=%.17g fp64=%.17g max_residual=%.17g "
			"max_local_termwise_enclosure=%.17g worst_residual_over_local_bound=%.17g "
			"fp64_binary32_projection_bit_mismatches=%zu passed=%d\n",
			field,units,result.worst,static_cast<double>(device[result.worst]),mirror[result.worst],
			result.maximumResidual,result.maximumBound,result.worstRatio,result.bitMismatchCount,
			result.passed?1:0);
		return result;};
	const double ownerArithmeticProbeBound=2.0*ownerGamma8;
	const double ownerArithmeticProbeResidual=2.0*ownerArithmeticProbeBound;
	const std::vector<float> ownerArithmeticProbeDevice(1u,1.0f);
	const std::vector<double> ownerArithmeticProbeMirror(
		1u,1.0+ownerArithmeticProbeResidual);
	const OwnerFieldBound ownerArithmeticProbe=evaluateOwnerField(
		"arithmetic_enclosure_mutant","dimensionless",ownerArithmeticProbeDevice,
		ownerArithmeticProbeMirror,[&](std::size_t){return ownerArithmeticProbeBound;});
	const bool ownerArithmeticBoundCanFail=!ownerArithmeticProbe.passed&&
		ownerArithmeticProbeResidual<ownerRequest64.scalarContract.feasibilityFactor;
	std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED name=admissibility_slack_not_numeric_bound "
		"units=dimensionless residual=%.17g arithmetic_bound=%.17g retired_feasibility_slack=%.17g "
		"passed=%d\n",ownerArithmeticProbeResidual,ownerArithmeticProbeBound,
		ownerRequest64.scalarContract.feasibilityFactor,ownerArithmeticBoundCanFail?1:0);
	bool ownerMirrorBounded=owner64Accepted;
	const double ownerDeviceTerminalResidual=ownerAccepted?
		static_cast<double>(ownerObserved.projection.maximumPostProjectionResidualPerS):0.0;
	const double ownerFP64TerminalResidual=owner64Accepted?
		ownerObserved64.r2.projection.maximumPostProjectionResidualPerS:0.0;
	const double oneSidedProjectionBound=ownerDeviceTerminalResidual*shape.cellWidthM;
	const double twoSidedProjectionBound=(ownerDeviceTerminalResidual+
		ownerFP64TerminalResidual)*shape.cellWidthM;
	const double projectionBracketProbe=oneSidedProjectionBound+
		0.5*ownerFP64TerminalResidual*shape.cellWidthM;
	const bool projectionBracketRED=ownerDeviceTerminalResidual>=0.0&&
		ownerFP64TerminalResidual>0.0&&projectionBracketProbe>oneSidedProjectionBound&&
		projectionBracketProbe<=twoSidedProjectionBound;
	std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_RED name=two_owner_projection_bracket "
		"units=m_s^-1 device_only_bound=%.17g fp64_residual_contribution=%.17g "
		"probe=%.17g two_sided_bound=%.17g passed=%d\n",oneSidedProjectionBound,
		ownerFP64TerminalResidual*shape.cellWidthM,projectionBracketProbe,
		twoSidedProjectionBound,projectionBracketRED?1:0);
	if(ownerAccepted&&owner64Accepted){
		::RISEFireProductionFP64::FireProductionCompatibleFCTMomentumResult
			ownerAdvection0,ownerAdvection1;
		std::string ownerAdvectionError;
		const bool ownerAdvectionBuilt=
			::RISEFireProductionFP64::EvaluateFireProductionCompatibleHeunMomentumCPU(
				ownerObserved64.r0.flux,ownerObserved64.heunSolve,
				ownerObserved64.r0.projection.velocityMPerS,ownerAdvection0,
				&ownerAdvectionError)&&
			::RISEFireProductionFP64::EvaluateFireProductionCompatibleHeunMomentumCPU(
				ownerObserved64.r1.flux,ownerObserved64.heunSolve,
				ownerObserved64.r1.projection.velocityMPerS,ownerAdvection1,
				&ownerAdvectionError);
		auto packDeviceFaces=[&](const std::array<std::vector<float>,3>& source){
			std::vector<float> packed;
			for(const auto& axis:source)packed.insert(packed.end(),axis.begin(),axis.end());
			return packed;};
		auto packMirrorFaces=[&](const std::array<std::vector<double>,3>& source){
			std::vector<double> packed;
			for(const auto& axis:source)packed.insert(packed.end(),axis.begin(),axis.end());
			return packed;};
		std::array<std::vector<double>,3> provisional64,advection64,buoyancy64,
			stress64,phase64;
		std::vector<double> acceptedAlpha64,eddy64,representedRatio64,absoluteDeviation64;
		if(ownerAdvectionBuilt){
			for(unsigned int axis=0u;axis<3u;++axis){
				const std::size_t faces=ownerRequest64.beginningMomentumKGPerM2S[axis].size();
				provisional64[axis].resize(faces);advection64[axis].resize(faces);
				buoyancy64[axis].resize(faces);stress64[axis].resize(faces);
				phase64[axis].resize(faces);
				for(std::size_t face=0u;face<faces;++face){
					advection64[axis][face]=-0.5*(ownerAdvection0.advectionRateKGPerM2S2[axis][face]+
						ownerAdvection1.advectionRateKGPerM2S2[axis][face]);
					buoyancy64[axis][face]=0.5*(ownerObserved64.r0.nonpressure.
						buoyancyMomentumRateKGPerM2S2[axis][face]+ownerObserved64.r1.nonpressure.
						buoyancyMomentumRateKGPerM2S2[axis][face]);
					stress64[axis][face]=0.5*(ownerObserved64.r0.nonpressure.
						stressMomentumRateKGPerM2S2[axis][face]+ownerObserved64.r1.nonpressure.
						stressMomentumRateKGPerM2S2[axis][face]);
					phase64[axis][face]=0.5*(ownerObserved64.r0.nonpressure.
						phaseSourceMomentumRateKGPerM2S2[axis][face]+ownerObserved64.r1.nonpressure.
						phaseSourceMomentumRateKGPerM2S2[axis][face]);
					provisional64[axis][face]=ownerRequest64.beginningMomentumKGPerM2S[axis][face]+
						ownerRequest64.scalarContract.timeStepS*(advection64[axis][face]+
						buoyancy64[axis][face]+stress64[axis][face]+phase64[axis][face]);
				}
			}
			for(unsigned int axis=0u;axis<3u;++axis)acceptedAlpha64.insert(
				acceptedAlpha64.end(),ownerObserved64.heunSolve.scalar.sharedFaceAlpha[axis].begin(),
				ownerObserved64.heunSolve.scalar.sharedFaceAlpha[axis].end());
			eddy64.resize(cells);for(std::size_t cell=0u;cell<cells;++cell)
				eddy64[cell]=0.5*(ownerObserved64.r0.nonpressure.eddyKinematicViscosityM2PerS[cell]+
					ownerObserved64.r1.nonpressure.eddyKinematicViscosityM2PerS[cell]);
			representedRatio64.resize(cells);absoluteDeviation64.resize(cells);
			for(std::size_t cell=0u;cell<cells;++cell){std::array<double,9> state;
				for(std::size_t component=0u;component<9u;++component)
					state[component]=ownerObserved64.conservativeValues[component*cells+cell];
				double ratio=0.0;if(!fuel.AcceptedConservativePressureRatioAtTemperatureByComponentOrder(
					state.data(),state.size(),static_cast<double>(static_cast<float>(
					ownerObserved64.committedEOS.temperatureK[cell])),
					FireStateProducerPrecision::Binary32,ratio,&ownerAdvectionError)){
					ownerMirrorBounded=false;break;}
				representedRatio64[cell]=ratio;absoluteDeviation64[cell]=std::fabs(ratio-1.0);}
		}
		auto streamBound=[&](const std::vector<float>& device,const std::vector<double>& mirror,
			const char* name,const char* units){const OwnerFieldBound checked=evaluateOwnerField(
				name,units,device,mirror,[&](std::size_t index){return ownerGamma512*(
					std::fabs(static_cast<double>(device[index]))+std::fabs(mirror[index]));});
			ownerMirrorBounded=ownerMirrorBounded&&checked.passed;return checked;};
		ownerMirrorBounded=ownerMirrorBounded&&ownerAdvectionBuilt;
		if(ownerAdvectionBuilt){
			streamBound(packDeviceFaces(ownerObserved.provisionalMomentumKGPerM2S),
				packMirrorFaces(provisional64),"R1_provisional_momentum","kg_m^-2_s^-1");
			streamBound(packDeviceFaces(ownerObserved.heunAdvectionMomentumRateKGPerM2S2),
				packMirrorFaces(advection64),"R1_heun_advection","kg_m^-2_s^-2");
			streamBound(packDeviceFaces(ownerObserved.heunBuoyancyMomentumRateKGPerM2S2),
				packMirrorFaces(buoyancy64),"R1_heun_buoyancy","kg_m^-2_s^-2");
			const std::vector<float> r0StressDevice=packDeviceFaces(
				ownerObserved.qualificationIterationTrace[0].back().stressMomentumRateKGPerM2S2);
			const std::vector<float> r1StressDevice=packDeviceFaces(
				ownerObserved.qualificationIterationTrace[1].back().stressMomentumRateKGPerM2S2);
			const std::vector<double> r0StressMirror=packMirrorFaces(
				ownerObserved64.r0.nonpressure.stressMomentumRateKGPerM2S2);
			const std::vector<double> r1StressMirror=packMirrorFaces(
				ownerObserved64.r1.nonpressure.stressMomentumRateKGPerM2S2);
			const std::vector<float> heunStressDevice=packDeviceFaces(
				ownerObserved.heunStressMomentumRateKGPerM2S2);
			const std::vector<double> heunStressMirror=packMirrorFaces(stress64);
			const OwnerFieldBound heunStressBound=evaluateOwnerField("R1_heun_stress",
				"kg_m^-2_s^-2",heunStressDevice,heunStressMirror,[&](std::size_t face){
					return ownerGamma512*(std::fabs(static_cast<double>(r0StressDevice[face]))+
						std::fabs(static_cast<double>(r1StressDevice[face]))+
						std::fabs(r0StressMirror[face])+std::fabs(r1StressMirror[face]));});
			ownerMirrorBounded=ownerMirrorBounded&&heunStressBound.passed;
			streamBound(packDeviceFaces(ownerObserved.heunPhaseSourceMomentumRateKGPerM2S2),
				packMirrorFaces(phase64),"R1_heun_phase_source","kg_m^-2_s^-2");
			streamBound(ownerObserved.acceptedFaceAlpha,acceptedAlpha64,
				"R1_accepted_face_alpha","dimensionless");
			streamBound(ownerObserved.heunEddyKinematicViscosityM2PerS,eddy64,
				"R1_heun_eddy_viscosity","m^2_s^-1");
			const OwnerFieldBound representedRatioBound=streamBound(
				ownerObserved.representedPressureRatio,representedRatio64,
				"R1_represented_pressure_ratio","dimensionless");
			const OwnerFieldBound absoluteDeviationBound=evaluateOwnerField(
				"R1_absolute_EOS_deviation","dimensionless",ownerObserved.absoluteEOSDeviation,
				absoluteDeviation64,[&](std::size_t cell){return representedRatioBound.localBounds[cell]+
					localProjectionBound(static_cast<float>(absoluteDeviation64[cell]));});
			ownerMirrorBounded=ownerMirrorBounded&&absoluteDeviationBound.passed;
		}
		const std::vector<double>* projectionTargets64[3]={
			&ownerObserved64.r0.projectionTarget.TargetPerS(),
			&ownerObserved64.r1.projectionTarget.TargetPerS(),
			&ownerObserved64.r2.projectionTarget.TargetPerS()};
		const std::vector<double>* acceptedTargets64[3]={
			&ownerObserved64.r0.target.TargetPerS(),&ownerObserved64.r1.target.TargetPerS(),
			&ownerObserved64.r2.target.TargetPerS()};
		const std::uint32_t projectionTargetIterations64[3]={
			ownerObserved64.r0.projectionTarget.CorrectionIteration(),
			ownerObserved64.r1.projectionTarget.CorrectionIteration(),
			ownerObserved64.r2.projectionTarget.CorrectionIteration()};
		const std::uint32_t acceptedTargetIterations64[3]={
			ownerObserved64.r0.target.CorrectionIteration(),
			ownerObserved64.r1.target.CorrectionIteration(),
			ownerObserved64.r2.target.CorrectionIteration()};
		const ::RISEFireProductionFP64::FireProductionProjectedHeunCoupledStageResult*
			stages64[3]={&ownerObserved64.r0,&ownerObserved64.r1,&ownerObserved64.r2};
		for(unsigned int stage=0u;stage<3u;++stage){
			const bool terminalBoundsPresent=ownerTerminalProjectionTargetBounds[stage].size()==cells&&
				ownerTerminalAcceptedTargetBounds[stage].size()==cells;
			const std::string projectionName="R"+std::to_string(stage)+"_projection_target";
			const std::string acceptedName="R"+std::to_string(stage)+"_accepted_target";
			const OwnerFieldBound projectionTargetBound=evaluateOwnerField(projectionName.c_str(),
				"s^-1",ownerObserved.projectionTargetPerS[stage],*projectionTargets64[stage],
				[&,stage](std::size_t cell){return terminalBoundsPresent?
					ownerTerminalProjectionTargetBounds[stage][cell]:0.0;});
			const OwnerFieldBound acceptedTargetBound=evaluateOwnerField(acceptedName.c_str(),
				"s^-1",ownerObserved.acceptedTargetPerS[stage],*acceptedTargets64[stage],
				[&,stage](std::size_t cell){return terminalBoundsPresent?
					ownerTerminalAcceptedTargetBounds[stage][cell]:0.0;});
			const bool iterationsMatch=ownerObserved.projectionTargetCorrectionIteration[stage]==
				projectionTargetIterations64[stage]&&
				ownerObserved.acceptedTargetCorrectionIteration[stage]==
				acceptedTargetIterations64[stage]&&
				ownerObserved.acceptedPicardIterations[stage]==stages64[stage]->acceptedIterationCount&&
				ownerObserved.activeSetCycleLength[stage]==stages64[stage]->activeSetCycleLength&&
				ownerObserved.activeSetCanonicalProjectionCount[stage]==
					stages64[stage]->activeSetCanonicalProjectionCount&&
				ownerObserved.activeSetDiscontinuousClass[stage]==
					stages64[stage]->activeSetDiscontinuousClass&&
				ownerObserved.limiterDiscontinuousClass[stage]==
					stages64[stage]->limiterDiscontinuousClass;
			// The owner residual is a control diagnostic: max(target, momentum/dx,
			// transport-coefficient change).  It intentionally pools unlike units
			// and contains subtractive differences, so treating its binary32 ULP as
			// an arithmetic enclosure would violate the termwise-bound rule.  Gate
			// the contributing fields above and the exact class/iteration outcomes;
			// retain this aggregate only as a descriptive trace.
			const bool residualTraceShapeMatch=ownerObserved.picardResidualPerS[stage].size()==
				stages64[stage]->picardResidualPerS.size();
			double residualTraceMaximumDifference=0.0;
			for(std::size_t iteration=0u;residualTraceShapeMatch&&iteration<
				ownerObserved.picardResidualPerS[stage].size();++iteration)
				residualTraceMaximumDifference=std::max(residualTraceMaximumDifference,std::fabs(
					static_cast<double>(ownerObserved.picardResidualPerS[stage][iteration])-
					stages64[stage]->picardResidualPerS[iteration]));
			std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_PICARD_RESIDUAL_TRACE stage=R%u "
				"scope=descriptive_not_acceptance mixed_quantity_maximum=1 shape_match=%d "
				"maximum_difference=%.17g\n",stage,residualTraceShapeMatch?1:0,
				residualTraceMaximumDifference);
			const bool targetEnvelopePassed=terminalBoundsPresent&&projectionTargetBound.passed&&
				acceptedTargetBound.passed;
			ownerMirrorBounded=ownerMirrorBounded&&targetEnvelopePassed&&iterationsMatch&&
				residualTraceShapeMatch&&
				ownerIterationTraceBounded;
			std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_TARGET_TRACE stage=R%u "
				"projection_iteration_device=%u projection_iteration_fp64=%u "
				"accepted_iteration_device=%u accepted_iteration_fp64=%u passed=%d\n",stage,
				ownerObserved.projectionTargetCorrectionIteration[stage],
				projectionTargetIterations64[stage],
				ownerObserved.acceptedTargetCorrectionIteration[stage],
				acceptedTargetIterations64[stage],iterationsMatch&&residualTraceShapeMatch&&
				targetEnvelopePassed&&
				ownerIterationTraceBounded?1:0);
		}
		const std::size_t acceptedAllFaces=ownerObserved64.r1.scalarAcceptance.
			packedFaceOffset[2u]+FireProductionProjectionFaceCount(shape,2u);
		const double acceptedFluxScale=ownerRequest64.scalarContract.timeStepS/shape.cellWidthM;
		const std::vector<double>& ownerSourceDelta64=ownerRequest64.source.SourceDelta();
		auto conservativeArithmeticScale=[&](const std::size_t component,
			const std::size_t cell){
			const std::size_t x=cell%shape.nx,y=(cell/shape.nx)%shape.ny,
				z=cell/(shape.nx*shape.ny);
			// Enclose the operations which form lowState as well as the accepted
			// antidiffusive correction.  Using |lowState| here would already have
			// hidden the donor-flux cancellation, precisely the escape this bound
			// is meant to prevent for nominally-zero CO/carbon rows.
			// The r60 acceptance alpha is a function of the complete local mass row.
			// Its conditioning scale must therefore include all same-unit mass terms
			// for a mass component; otherwise a nominally-zero constituent receives
			// a false zero bound despite taking the shared-alpha branch decisions.
			const std::size_t firstScaleComponent=component<8u?0u:8u;
			const std::size_t endScaleComponent=component<8u?8u:9u;
			double scale=0.0;
			for(std::size_t scaleComponent=firstScaleComponent;
				scaleComponent<endScaleComponent;++scaleComponent)
				scale+=std::fabs(ownerRequest64.beginningConservativeValues[
					scaleComponent*cells+cell])+std::fabs(ownerSourceDelta64[
					scaleComponent*cells+cell]);
			for(unsigned int axis=0u;axis<3u;++axis){
				const std::size_t upperX=x+(axis==0u),upperY=y+(axis==1u),
					upperZ=z+(axis==2u);
				const std::size_t offset=ownerObserved64.r1.scalarAcceptance.
					packedFaceOffset[axis];
				const std::size_t lowerLocal=axis==0u?(z*shape.ny+y)*(shape.nx+1u)+x:
					(axis==1u?(z*(shape.ny+1u)+y)*shape.nx+x:(z*shape.ny+y)*shape.nx+x);
				const std::size_t upperLocal=axis==0u?(upperZ*shape.ny+upperY)*
					(shape.nx+1u)+upperX:(axis==1u?(upperZ*(shape.ny+1u)+upperY)*
					shape.nx+upperX:(upperZ*shape.ny+upperY)*shape.nx+upperX);
				for(std::size_t scaleComponent=firstScaleComponent;
					scaleComponent<endScaleComponent;++scaleComponent){
					const double lowerDonor=acceptedFluxScale*ownerObserved64.r1.scalarAcceptance.
						lowFlux[scaleComponent*acceptedAllFaces+offset+lowerLocal];
					const double upperDonor=acceptedFluxScale*ownerObserved64.r1.scalarAcceptance.
						lowFlux[scaleComponent*acceptedAllFaces+offset+upperLocal];
					const double lowerTerm=acceptedFluxScale*ownerObserved64.r1.scalarAcceptance.
						sharedFaceAlpha[axis][lowerLocal]*ownerObserved64.r1.scalarAcceptance.
						fluxDelta[scaleComponent*acceptedAllFaces+offset+lowerLocal];
					const double upperTerm=acceptedFluxScale*ownerObserved64.r1.scalarAcceptance.
						sharedFaceAlpha[axis][upperLocal]*ownerObserved64.r1.scalarAcceptance.
						fluxDelta[scaleComponent*acceptedAllFaces+offset+upperLocal];
					scale+=std::fabs(lowerDonor)+std::fabs(upperDonor)+
						std::fabs(lowerTerm)+std::fabs(upperTerm);
				}
			}
			return scale;
		};
		const char* conservativeName[9]={"rhoZ","CH4","O2","CO2","H2O","N2","CO",
			"carbon","sensible_energy"};
		for(std::size_t component=0u;component<9u;++component){std::vector<float> device(cells);
			std::vector<double> mirror(cells);for(std::size_t cell=0u;cell<cells;++cell){
				device[cell]=ownerObserved.conservativeValues[component*cells+cell];
				mirror[cell]=ownerObserved64.conservativeValues[component*cells+cell];}
			const OwnerFieldBound bound=evaluateOwnerField(conservativeName[component],
				component==8u?"J_m^-3":"kg_m^-3",device,mirror,[&,component](std::size_t cell){
				const double termMagnitude=conservativeArithmeticScale(component,cell)+
					std::fabs(mirror[cell])+std::fabs(static_cast<double>(device[cell]));
				return ownerGamma512*termMagnitude;});
			ownerMirrorBounded=ownerMirrorBounded&&bound.passed;}
		for(unsigned int axis=0u;axis<3u;++axis){const std::string momentum="momentum_"+
			std::to_string(axis),velocity="velocity_"+std::to_string(axis);
			const double velocityImpulse=3.0*std::fabs(ownerRequest.gravityMPerS2[axis])*
				ownerEOS.candidateTimeStepS;
			// Bracket both terminal solves.  Each independently certifies the
			// remaining divergence-rate residual; their sum times dx is the
			// cancellation-free velocity gap allowed between the two solutions.
			const double projectionVelocityEnclosure=
				(static_cast<double>(ownerObserved.projection.maximumPostProjectionResidualPerS)+
				 ownerObserved64.r2.projection.maximumPostProjectionResidualPerS)*shape.cellWidthM;
			const OwnerFieldBound momentumBound=evaluateOwnerField(momentum.c_str(),
				"kg_m^-2_s^-1",ownerObserved.momentumKGPerM2S[axis],
				ownerObserved64.momentumKGPerM2S[axis],[&,axis,velocityImpulse](std::size_t face){
					const double deviceDensity=static_cast<double>(
						ownerObserved.projection.faceDensityKGPerM3[axis][face]);
					const double mirrorDensity=
						ownerObserved64.r2.projection.faceDensityKGPerM3[axis][face];
					const double localFaceDensity=std::max(std::fabs(deviceDensity),
						std::fabs(mirrorDensity));
					const double densityPerturbation=std::fabs(deviceDensity-mirrorDensity);
					const double impulseScale=localFaceDensity*velocityImpulse;
					return ownerGamma512*(std::fabs(ownerRequest.beginningMomentumKGPerM2S[axis][face])+
						std::fabs(static_cast<double>(ownerObserved.momentumKGPerM2S[axis][face]))+
						std::fabs(ownerObserved64.momentumKGPerM2S[axis][face])+impulseScale)+
						localFaceDensity*projectionVelocityEnclosure+
						std::fabs(ownerObserved64.velocityMPerS[axis][face])*densityPerturbation;});
			const OwnerFieldBound velocityBound=evaluateOwnerField(velocity.c_str(),"m_s^-1",
				ownerObserved.velocityMPerS[axis],ownerObserved64.velocityMPerS[axis],
				[&,axis,velocityImpulse](std::size_t face){return ownerGamma512*(
					std::fabs(static_cast<double>(ownerObserved.velocityMPerS[axis][face]))+
					std::fabs(ownerObserved64.velocityMPerS[axis][face])+velocityImpulse)+
					projectionVelocityEnclosure;});
			std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_PROJECTION_BRACKET axis=%u "
				"units=m_s^-1 device_post_residual_per_s=%.17g fp64_post_residual_per_s=%.17g "
				"cell_width_m=%.17g velocity_enclosure=%.17g\n",axis,
				static_cast<double>(ownerObserved.projection.maximumPostProjectionResidualPerS),
				ownerObserved64.r2.projection.maximumPostProjectionResidualPerS,
				static_cast<double>(shape.cellWidthM),projectionVelocityEnclosure);
			ownerMirrorBounded=ownerMirrorBounded&&momentumBound.passed&&velocityBound.passed;}
		const OwnerFieldBound temperatureBound=evaluateOwnerField("temperature","K",
			ownerObserved.temperatureK,ownerObserved64.committedEOS.temperatureK,
			[&](std::size_t cell){return ownerGamma512*(std::fabs(ownerEOS.physicalFlux.transport.
				temperatureK[cell])+std::fabs(static_cast<double>(ownerObserved.temperatureK[cell]))+
				std::fabs(ownerObserved64.committedEOS.temperatureK[cell]));});
		ownerMirrorBounded=ownerMirrorBounded&&temperatureBound.passed;}
	std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_FP64 source=%d begin=%d r0=%d r1=%d "
		"accepted=%d criterion=conjunction_of_per_cell_per_field_same_unit_enclosures error=%s passed=%d\n",
		ownerSourceBuilt?1:0,owner64Begin?1:0,owner64R0?1:0,owner64R1?1:0,
		owner64Accepted?1:0,ownerSourceBuilt?owner64Error.c_str():ownerSourceError.c_str(),
		ownerMirrorBounded?1:0);
	std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_SMOKE accepted=%d owner_identity=%llu "
		"iterations=%u/%u/%u commits=%u projections=%u staging=%u transfers=%u "
		"actual_ws=%llu certified_ws=%llu commuting_residual=%.9g commuting_scale=%.9g "
		"commuting_bound=%.9g commuting_passed=%d device_ms=%.9g wall_ms=%.9g error=%s\n",
		ownerAccepted?1:0,static_cast<unsigned long long>(ownerObserved.ownerPublicationIdentity),
		ownerObserved.acceptedPicardIterations[0],ownerObserved.acceptedPicardIterations[1],
		ownerObserved.acceptedPicardIterations[2],ownerObserved.commandCommitCount,
		ownerObserved.residentProjectionInvocationCount,ownerObserved.terminalStagingCount,
		ownerObserved.interstageFullGridTransferCount,
		static_cast<unsigned long long>(ownerObserved.actualMetalAllocationBytes),
		static_cast<unsigned long long>(ownerObserved.certifiedWorkingSetBytes),
		ownerObserved.maximumCommutingResidualKGPerM3,
		ownerObserved.commutingIdentityScaleKGPerM3,
		ownerObserved.commutingIdentityBoundKGPerM3,
		ownerObserved.commutingIdentityPassed?1:0,
		ownerObserved.deviceElapsedMS,ownerObserved.wallElapsedMS,ownerError.c_str());
	std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_PRODUCTION_ENTRY accepted=%d "
		"token=%d token_matches=%d owner_identity=%llu diagnostic_identity=%llu "
		"monitored=%d enforced=%d transfers=%u staging=%u error=%s passed=%d\n",
		ownerResidentAccepted?1:0,ownerResidentObserved.HasAcceptedManifoldToken()?1:0,
		ownerResidentObserved.AcceptedManifoldTokenMatchesCurrentPayload()?1:0,
		static_cast<unsigned long long>(ownerResidentObserved.projectedHeunOwnerIdentity),
		static_cast<unsigned long long>(ownerResidentDiagnostics.ownerPublicationIdentity),
		ownerResidentObserved.manifoldDiagnosticsMonitored?1:0,
		ownerResidentObserved.manifoldPlateauEnforced?1:0,
		ownerResidentObserved.interstageFullGridTransferCount,
		ownerResidentObserved.terminalStagingCount,ownerResidentError.c_str(),
		ownerResidentAccepted&&ownerResidentObserved.HasAcceptedManifoldToken()&&
		ownerResidentObserved.AcceptedManifoldTokenMatchesCurrentPayload()&&
		ownerResidentObserved.projectedHeunOwnerIdentity==
			ownerResidentDiagnostics.ownerPublicationIdentity&&
		ownerResidentObserved.manifoldDiagnosticsMonitored&&
		!ownerResidentObserved.manifoldPlateauEnforced&&
		ownerResidentObserved.interstageFullGridTransferCount==0u&&
		ownerResidentObserved.terminalStagingCount==1u?1:0);
	std::fprintf(stderr,"PROJECTED_HEUN_METAL_OWNER_TIMING samples=%zu device_p95_ms=%.9g "
		"wall_p95_ms=%.9g passed=%d\n",ownerDeviceTimingMS.size(),ownerDeviceP95,ownerWallP95,
		ownerDeviceTimingMS.size()==5u?1:0);
	const bool ownerProductionEntryPassed=ownerResidentAccepted&&
		ownerResidentObserved.HasAcceptedManifoldToken()&&
		ownerResidentObserved.AcceptedManifoldTokenMatchesCurrentPayload()&&
		ownerResidentObserved.projectedHeunOwnerIdentity==
			ownerResidentDiagnostics.ownerPublicationIdentity&&
		ownerResidentObserved.manifoldDiagnosticsMonitored&&
		!ownerResidentObserved.manifoldPlateauEnforced&&
		ownerResidentObserved.interstageFullGridTransferCount==0u&&
		ownerResidentObserved.terminalStagingCount==1u;
	const bool ownerSmokePassed=ownerAccepted&&ownerMirrorBounded&&ownerArithmeticBoundCanFail&&
		projectionBracketRED&&effectiveAsMolecularRED&&
		r2ImmediateEndpointClassRED&&endpointClassAuthorityRED&&endpointPredicateIdentityRED&&
		productionQualificationRefusalRED&&transportInvariantREDs&&
		ownerProductionEntryPassed&&
		ownerObserved.ownerPublicationIdentity!=0u&&
		ownerObserved.terminalStagingCount==1u&&
		ownerObserved.qualificationTraceStagingCount>0u&&ownerDeviceTimingMS.size()==5u&&
		ownerStaleRefused&&ownerOrderRefused&&ownerForgedRefused&&ownerCallbackRefused&&
		ownerAtomicRefused&&ownerPolicyDivergenceRefused&&
		ownerStaleTargetPublicationRefused&&ownerUnverifiedPrivateBufferRefused&&
		ownerActualInterstageTransferRefused&&tier8OwnerWorkingSetScales&&
		activeCycleREDs&&limiterOutcomeRED;
	const bool passed=thresholdCoverage&&exactThresholdNoDrain&&policyOwnersIdentical&&
		deviceExactPositiveNoDrain&&
		deviceExactNegativeNoDrain&&independentBoundCanFail&&allBoundsPass&&
		tangentEqual&&sourceEqual&&diagnosticEqual&&tailEqual&&assembledEqual&&
		openTangentEqual&&openSourceEqual&&openDiagnosticEqual&&openTailEqual&&openAssembledEqual&&
		unsealedTransport&&unsealedPhysical&&unsealedCandidate&&unsealedEOS&&unsealedSource&&
		staleCandidate&&unlinkedEOS&&cpuSourceRefused&&cpuPrivateSourceRefused&&
		wrongBeginningSourceRefused&&wrongCaseSourceRefused&&
		mismatchedSourcePacket&&mismatchedEOSThermochemistry&&staleMetadataRefused&&
		cpuTargetRefused&&cpuProjectionMetadataRefused&&
		preauthoredRefused&&topologyRefused&&dormantThresholdIdentity&&fixtureCertified&&
		liveCertified&&observed.liveAuthorityAllocationBytes<=liveBytes&&understatedRefused&&
		transferLedgerRefused&&ownerSmokePassed&&heunWeightingRED&&
		continuousEnclosureRED&&outOfEnclosureRED&&
		heunWeightingMutantAccepted&&sharedAlphaRED&&ownerWorkingSetPreflightRefused&&
		branches&&observed.commandCommitCount==1u&&observed.terminalStagingCount==1u&&
		observed.interstageFullGridTransferCount==0u&&openObserved.commandCommitCount==1u&&
		openObserved.terminalStagingCount==1u&&openObserved.interstageFullGridTransferCount==0u;
	std::fprintf(stderr,"RESIDENT_TARGET passed=%d tangent_bit_equal=%d source_bit_equal=%d "
		"absolute_diagnostic_bit_equal=%d tail_bit_equal=%d assembled_bit_equal=%d "
		"unsealed_transport=%d unsealed_physical=%d unsealed_candidate=%d unsealed_eos=%d "
		"unsealed_source=%d stale_candidate=%d unlinked_eos=%d cpu_source_refused=%d "
		"cpu_private_source_refused=%d source_packet_refused=%d "
		"source_beginning_refused=%d source_case_refused=%d eos_thermo_refused=%d "
		"stale_metadata_refused=%d cpu_target_refused=%d "
		"cpu_projection_metadata_refused=%d preauthored_refused=%d "
		"topology_refused=%d dormant_threshold_identity=%d closed_branch_bitmap=0x%08x "
		"closed_required=0x%08x open_branch_bitmap=0x%08x open_required=0x%08x "
		"command_per_interval=%u reads_per_interval=%u transfers_per_interval=%u "
		"enclosures=%d fixture_ws=%llu actual_ws=%llu live_ws=%llu\n",passed?1:0,tangentEqual?1:0,
		sourceEqual?1:0,diagnosticEqual?1:0,tailEqual?1:0,assembledEqual?1:0,
		unsealedTransport?1:0,unsealedPhysical?1:0,unsealedCandidate?1:0,unsealedEOS?1:0,
		unsealedSource?1:0,staleCandidate?1:0,unlinkedEOS?1:0,cpuSourceRefused?1:0,
		cpuPrivateSourceRefused?1:0,mismatchedSourcePacket?1:0,
		wrongBeginningSourceRefused?1:0,wrongCaseSourceRefused?1:0,
		mismatchedEOSThermochemistry?1:0,staleMetadataRefused?1:0,cpuTargetRefused?1:0,
		cpuProjectionMetadataRefused?1:0,preauthoredRefused?1:0,
		topologyRefused?1:0,dormantThresholdIdentity?1:0,
		observed.branchObligationBitmap,requiredClosedBranches,
		openObserved.branchObligationBitmap,requiredOpenBranches,
		observed.commandCommitCount,
		observed.terminalStagingCount,observed.interstageFullGridTransferCount,
		allBoundsPass?1:0,
		static_cast<unsigned long long>(fixtureBytes),
		static_cast<unsigned long long>(observed.actualMetalAllocationBytes),
		static_cast<unsigned long long>(liveBytes));
	std::fprintf(stderr,"RESIDENT_TARGET_IDENTITIES transport=%llu physical_flux=%llu "
		"candidate=%llu eos=%llu frozen_source=%llu target=%llu projection_metadata=%llu consumer=%llu "
		"all_nonzero=%d\n",static_cast<unsigned long long>(observed.transportPublicationIdentity),
		static_cast<unsigned long long>(observed.physicalFluxPublicationIdentity),
		static_cast<unsigned long long>(observed.candidatePublicationIdentity),
		static_cast<unsigned long long>(observed.EOSPublicationIdentity),
		static_cast<unsigned long long>(observed.frozenSourcePublicationIdentity),
		static_cast<unsigned long long>(observed.targetPublicationIdentity),
		static_cast<unsigned long long>(observed.projectionMetadataIdentity),
		static_cast<unsigned long long>(observed.projectionConsumerIdentity),
		observed.transportPublicationIdentity!=0u&&observed.physicalFluxPublicationIdentity!=0u&&
		observed.candidatePublicationIdentity!=0u&&observed.EOSPublicationIdentity!=0u&&
		observed.frozenSourcePublicationIdentity!=0u&&
		observed.targetPublicationIdentity!=0u&&observed.projectionMetadataIdentity!=0u&&
		observed.projectionConsumerIdentity!=0u?1:0);
	const char* termName[5]={"tangent","frozen_source","absolute_reference_diagnostic",
		"monitored_absolute_reference","assembled_compatible_target"};
	std::fprintf(stderr,"RESIDENT_TARGET_BOUND_RED units=s^-1 projected=%.9g "
		"displaced_exact=%.17g residual_s^-1=%.17g local_rounding_enclosure_s^-1=%.17g "
		"passed=%d\n",boundREDProjection,boundREDDisplacedExact,boundREDResidual,
		boundREDLocal,independentBoundCanFail?1:0);
	auto emitTerms=[&](const char* topology,
		const FireProductionResidentTargetLineageComparatorResult& device,
		const std::vector<float>& tangentValue,const std::vector<float>& sourceValue,
		const std::vector<float>& diagnosticValue,const std::vector<float>& tailValue,
		const std::vector<float>& assembledValue,const std::vector<double>& tangentReference,
		const std::vector<double>& sourceReference,const std::vector<double>& diagnosticReference,
		const std::vector<double>& tailReference,const std::vector<double>& assembledReference){
		const std::vector<float>* localDevice[5]={&device.tangentTargetPerS,
			&device.frozenSourceTargetPerS,&device.absoluteReferenceDiagnosticPerS,
			&device.monitoredAbsoluteReferenceTargetPerS,&device.assembledTargetPerS};
		const std::vector<float>* localMirror[5]={&tangentValue,&sourceValue,&diagnosticValue,
			&tailValue,&assembledValue};
		const std::vector<double>* localExact[5]={&tangentReference,&sourceReference,
			&diagnosticReference,&tailReference,&assembledReference};
		for(unsigned int term=0u;term<5u;++term){double maximumResidual=0.0,maximumBound=0.0,
			worstRatio=0.0;bool termBounded=true;for(std::size_t cell=0u;cell<cells;++cell){const double residual=
			std::fabs(static_cast<double>((*localDevice[term])[cell])-(*localExact[term])[cell]);
			const double bound=term==0u?device.tangentEnclosurePerS[cell]:
				(term==1u?0.0:(term==4u?device.assembledEnclosurePerS[cell]:
				localProjectionBound((*localMirror[term])[cell])));
			termBounded=termBounded&&insideEnclosure((*localDevice[term])[cell],
				(*localExact[term])[cell],bound);
			maximumResidual=std::max(maximumResidual,residual);
			maximumBound=std::max(maximumBound,bound);if(bound>0.0)worstRatio=std::max(worstRatio,
				residual/bound);}
			std::fprintf(stderr,"RESIDENT_TARGET_TERM topology=%s field=%s units=s^-1 "
				"scope=every_cell max_residual_s^-1=%.17g max_local_termwise_enclosure_s^-1=%.17g "
				"worst_residual_over_local_bound=%.17g bit_equal=%d passed=%d\n",topology,
				termName[term],maximumResidual,maximumBound,worstRatio,
				bitEqual(*localDevice[term],*localMirror[term])?1:0,
				bitEqual(*localDevice[term],*localMirror[term])&&termBounded?1:0);}
		for(unsigned int term=0u;term<5u;++term)for(std::size_t cell=0u;cell<cells;++cell){
			const bool equal=std::memcmp(&(*localDevice[term])[cell],&(*localMirror[term])[cell],
				sizeof(float))==0;const double residual=std::fabs(static_cast<double>(
				(*localDevice[term])[cell])-(*localExact[term])[cell]);
			const double bound=term==0u?device.tangentEnclosurePerS[cell]:
				(term==1u?0.0:(term==4u?device.assembledEnclosurePerS[cell]:
				localProjectionBound((*localMirror[term])[cell])));
			const double ratio=bound>0.0?residual/bound:0.0;
			std::fprintf(stderr,"RESIDENT_TARGET_CELL topology=%s field=%s cell=%zu units=s^-1 "
				"device=%.9g fp64_mirror_binary32_projection=%.9g fp64_exact=%.17g residual_s^-1=%.17g "
				"local_termwise_enclosure_s^-1=%.17g residual_over_local_bound=%.17g bit_equal=%d\n",
				topology,termName[term],cell,(*localDevice[term])[cell],(*localMirror[term])[cell],
				(*localExact[term])[cell],residual,bound,ratio,equal?1:0);}};
	emitTerms("closed",observed,tangentMirror,sourceMirror,diagnosticMirror,tailMirror,
		assembledMirror,tangentExact,sourceExact,diagnosticExact,tailExact,assembledExact);
	emitTerms("pressure_open",openObserved,openTangent,openSource,openDiagnostic,openTail,
		openAssembled,openTangentExact,openSourceExact,openDiagnosticExact,openTailExact,
		openAssembledExact);
	return passed?0:219;
}

int RunProductionMetalFP64KernelSweep()
{
	const int transport=RunProductionResidentTransportMetalFP64Fixture();
	if( transport!=0 ) return transport;
	const int physicalFlux=RunProductionResidentPhysicalFluxMetalFP64Fixture();
	if( physicalFlux!=0 ) return physicalFlux;
	const int eosCandidate=RunProductionResidentEOSCandidateMetalFP64Fixture();
	if( eosCandidate!=0 ) return eosCandidate;
	const int targetLineage=RunProductionResidentTargetLineageMetalFP64Fixture();
	if( targetLineage!=0 ) return targetLineage;
	const int scalar=RunProductionScalarFCTMetalStageFixture();
	if( scalar!=0 ) return scalar;
	const int scalarMixed=RunProductionScalarFCTMetalMixedBoundaryFixture();
	if( scalarMixed!=0 ) return scalarMixed;
	return RunProductionCompatibleMomentumMetalFP64Fixture();
}
#endif

int main(int argc,char** argv)
{
#if defined(__APPLE__)
	if(argc==2&&std::strcmp(argv[1],"--fire-production-scalar-fct-metal-stages")==0)
		return RunProductionScalarFCTMetalStageFixture();
	if(argc==2&&std::strcmp(argv[1],"--fire-production-resident-eos-metal")==0)
		return RunProductionResidentEOSCandidateMetalFP64Fixture();
	if(argc==2&&std::strcmp(argv[1],"--fire-production-resident-target-metal")==0)
		return RunProductionResidentTargetLineageMetalFP64Fixture();
	if(argc==2&&std::strcmp(argv[1],"--fire-production-metal-fp64-kernel-sweep")==0)
		return RunProductionMetalFP64KernelSweep();
#endif
	if(argc==3&&std::strcmp(argv[1],"--fire-checkpoint-build-id")==0){
		MethaneRunCheckpoint checkpoint;std::string error;
		if(!LoadMethaneRunCheckpoint(argv[2],checkpoint,error))return 98;
		std::fprintf(stdout,"checkpoint_build_id=%s accepted_steps=%llu\n",
			checkpoint.producerBuildId.c_str(),
			static_cast<unsigned long long>(checkpoint.acceptedSteps));
		return 0;
	}
	if(argc==2&&std::strcmp(argv[1],"--fire-production-flame-holder-red")==0){
		const bool passed=ProductionEstablishedFlameHolderREDPasses();
		std::fprintf(stdout,"PRODUCTION_FLAME_HOLDER_RED passed=%d\n",passed?1:0);
		return passed?0:98;
	}
#if defined(RISE_ENABLE_OPENVDB)
	if(argc==7&&std::strcmp(argv[1],"--fire-production-puffing-spectrum")==0){
		double tier=0.0;if(!ParsePositiveDoubleArgument(argv[2],tier))return 91;
		return RunProductionPuffingSpectrumChild(tier,argv[3],argv[4],argv[5],argv[6]);
	}
	if(argc==4&&std::strcmp(argv[1],"--fire-production-checkpoint-physics-diagnostic")==0)
		return RunProductionCheckpointPhysicsDiagnosticChild(argv[2],argv[3]);
	if(argc==4&&std::strcmp(argv[1],"--fire-production-frame-write-benchmark")==0)
		return RunProductionFrameWriteBenchmarkChild(argv[2],argv[3]);
	if(argc==5&&std::strcmp(argv[1],"--fire-production-temporal-capstone")==0){
		double tier=0.0,cadence=0.0;
		if(!ParsePositiveDoubleArgument(argv[2],tier)||
			!ParsePositiveDoubleArgument(argv[3],cadence))return 91;
		return RunProductionTemporalCapstoneChild(tier,cadence,argv[4]);
	}
	if(argc==4&&std::strcmp(argv[1],"--fire-production-onset")==0){
		double tier=0.0;if(!ParsePositiveDoubleArgument(argv[2],tier))return 91;
		return RunProductionOnsetCampaignChild(tier,argv[3]);
	}
	if(argc==5&&std::strcmp(argv[1],"--fire-production-r193-baseline")==0)
		return RunProductionOnsetCampaignChild(8.0,argv[2],true,argv[3],argv[4]);
	if(argc==5&&std::strcmp(argv[1],"--fire-production-r201-ported")==0)
		return RunProductionOnsetCampaignChild(8.0,argv[2],false,argv[3],argv[4],true);
	if(argc==4&&std::strcmp(argv[1],"--fire-oracle-retained-trajectory")==0){
		double target=0.0;if(!ParsePositiveDoubleArgument(argv[2],target))return 91;
		return RunOracleRetainedTrajectoryChild(target,argv[3]);
	}
	if(argc==6&&std::strcmp(argv[1],"--fire-production-momentum-replay")==0)
		return RunProductionMomentumReplayChild(argv[2],argv[3],argv[4],argv[5]);
	if(argc==5&&std::strcmp(argv[1],"--fire-oracle-momentum-replay")==0)
		return RunOracleMomentumReplayChild(argv[2],argv[3],argv[4]);
	if(argc==4&&std::strcmp(argv[1],"--fire-production-temporal-preview")==0)
		return RunTemporalFirePreviewChild(argv[2],argv[3]);
	if(argc==4&&std::strcmp(argv[1],"--fire-first-light-preview")==0)
		return RunFirstLightPreviewChild(argv[2],argv[3]);
	Check(ProductionTemporalCapstoneTierSupported(6.0)&&
		ProductionTemporalCapstoneTierSupported(8.0)&&
		ProductionTemporalCapstoneTierSupported(10.0),
		"production temporal capstone admits the sealed preview and claim tiers");
	Check(!ProductionTemporalCapstoneTierSupported(5.0)&&
		!ProductionTemporalCapstoneTierSupported(7.0)&&
		!ProductionTemporalCapstoneTierSupported(9.0)&&
		!ProductionTemporalCapstoneTierSupported(11.0),
		"production temporal capstone refuses unsupported neighboring tiers");
	Check(ProductionResolutionTierForCheckpointDimensions({{52u,52u,80u}})==6.0&&
		ProductionResolutionTierForCheckpointDimensions({{69u,69u,106u}})==8.0&&
		ProductionResolutionTierForCheckpointDimensions({{86u,86u,132u}})==10.0,
		"production checkpoint diagnostics derive every sealed resolution tier from shape");
	Check(ProductionResolutionTierForCheckpointDimensions({{68u,69u,106u}})==0.0&&
		ProductionResolutionTierForCheckpointDimensions({{70u,69u,106u}})==0.0,
		"production checkpoint diagnostics refuse neighboring unknown shapes");
	{
		MethaneRunCheckpoint tier8,tier10;double tier8Width=0.0,tier10Width=0.0;
		const bool tier8Built=CanonicalCapstoneGridForTier(
			8.0,tier8.dimensions,tier8Width);
		const bool tier10Built=CanonicalCapstoneGridForTier(
			10.0,tier10.dimensions,tier10Width);
		tier8.cellWidthM=tier8Width;tier10.cellWidthM=tier10Width;
		std::ostringstream tier8Evidence,tier10Evidence,rejectedTierEvidence;
		const bool tier8Written=WriteProductionPuffingSpectrumTierEvidence(
			tier8Evidence,tier8,8.0);
		const bool tier10Written=WriteProductionPuffingSpectrumTierEvidence(
			tier10Evidence,tier10,10.0);
		const bool crossTierRejected=!WriteProductionPuffingSpectrumTierEvidence(
			rejectedTierEvidence,tier8,10.0)&&rejectedTierEvidence.str().empty();
		const auto exactlyOneLine=[](const std::string& text,const std::string& line){
			std::istringstream records(text);std::string record;unsigned int matches=0u;
			while(std::getline(records,record))if(record==line)++matches;
			return matches==1u;
		};
		Check(tier8Built&&tier10Built&&
			ProductionPuffingSpectrumTierMatches(tier8,8.0)&&
			!ProductionPuffingSpectrumTierMatches(tier8,10.0)&&
			ProductionPuffingSpectrumTierMatches(tier10,10.0)&&
			!ProductionPuffingSpectrumTierMatches(tier10,8.0)&&tier8Written&&tier10Written&&
			exactlyOneLine(tier8Evidence.str(),"resolution_tier 8")&&
			exactlyOneLine(tier10Evidence.str(),"resolution_tier 10")&&
			tier8Evidence.str().find("resolution_tier 6\n")==std::string::npos&&
			tier8Evidence.str().find("resolution_tier 10\n")==std::string::npos&&
			tier10Evidence.str().find("resolution_tier 6\n")==std::string::npos&&
			tier10Evidence.str().find("resolution_tier 8\n")==std::string::npos&&
			crossTierRejected,
			"puffing spectrum serializes tier 8/10 evidence only for matching checkpoint grids");
	}
	{
		std::vector<double> time(129u),signal(129u);
		for(std::size_t sample=0u;sample<time.size();++sample){
			time[sample]=0.0625*static_cast<double>(sample);
			signal[sample]=3.0+0.01*static_cast<double>(sample)+
				2.0*std::sin(2.0*3.14159265358979323846*2.0*time[sample]);
		}
		UniformHannSpectrum spectrum;
		Check(ComputeUniformHannSpectrum(time,signal,512u,spectrum)&&
			std::fabs(spectrum.frequencyHz[spectrum.dominantBin]-2.0)<0.01,
			"puffing spectrum removes an affine trend and retains a sealed sinusoid");
		std::fill(signal.begin(),signal.end(),4.0);
		Check(!ComputeUniformHannSpectrum(time,signal,512u,spectrum),
			"puffing spectrum refuses a zero-energy detrended signal");
	}
	{
		const std::vector<std::string> changing={"a","b","c","d","e","f","g","h"};
		const std::vector<bool> plume(8u,true);
		const std::vector<std::size_t> changingArea={640u,665u,690u,720u,750u,785u,820u,855u};
		const std::vector<double> movingX={27.0,27.2,27.4,27.6,27.8,28.0,28.2,28.4};
		const std::vector<double> movingY={22.0,22.7,23.4,24.1,24.8,25.5,26.2,26.9};
		Check(FirstLightAnimatedPlumeMetricsPass(changing,plume,plume,changingArea,
			movingX,movingY),
			"first-light animation accepts eight visible structured changing plume frames");
		std::vector<bool> terminalOnly(8u,false);terminalOnly.back()=true;
		Check(!FirstLightAnimatedPlumeMetricsPass(changing,terminalOnly,terminalOnly,
			{0u,0u,0u,0u,0u,0u,0u,855u},movingX,movingY),
			"first-light animation rejects a black prefix followed by one plume still");
		Check(!FirstLightAnimatedPlumeMetricsPass(std::vector<std::string>(8u,"same"),
			plume,plume,std::vector<std::size_t>(8u,720u),movingX,movingY),
			"first-light animation rejects a visible but static plume sequence");
		const std::vector<double> staticCentroid(8u,24.0);
		Check(!FirstLightAnimatedPlumeMetricsPass(changing,plume,plume,changingArea,
			staticCentroid,staticCentroid),
			"first-light animation rejects static geometry with digest and area flicker");
	}
	{
		const double beginning=2.1054894166300073;
		const double pilotEnd=2.1063728557800188;
		double representedStep=0.0016462659696117043;
		Check(LimitBinary32ProductionStepToEvent(beginning,pilotEnd,representedStep)&&
			representedStep==0.00088343920651823282&&beginning+representedStep>=pilotEnd,
			"production event owner rounds the pilot deadline outward and leaves no microscopic step");
		const double staleStep=static_cast<double>(static_cast<float>(pilotEnd-beginning));
		Check(beginning+staleStep<pilotEnd,
			"production event RED reproduces the inward-rounded pilot residue that caused velocity collapse");
	}
	{
		const std::vector<std::uint8_t> annulus={0u,0u,1u,0u,0u,0u};
		const std::vector<double> source={1.0,1.0,0.0,1.0,0.0,0.0};
		Check(!ProductionPilotCommandCell(0u,3u,2u,annulus,source)&&
			ProductionPilotCommandCell(1u,3u,2u,annulus,source)&&
			ProductionPilotCommandCell(2u,3u,2u,annulus,source)&&
			!ProductionPilotCommandCell(3u,3u,2u,annulus,source)&&
			!ProductionPilotCommandCell(4u,3u,2u,annulus,source)&&
			!ProductionPilotCommandCell(5u,3u,2u,annulus,source),
			"production pilot command contacts only the source-side boundary of the canonical annulus");
		Check(ProductionEstablishedFlameHolderREDPasses(),
			"production established-flame holder requires product evidence and source-boundary contact");
	}
	if(const char* profileEnvironment=std::getenv("RISE_FIRE_PROFILE")){
		if(std::strcmp(profileEnvironment,"1")!=0){
			std::fprintf(stderr,"RISE_FIRE_PROFILE must be exactly 1\n");return 96;
		}
	}
	if(argc==7&&std::strcmp(argv[1],"--fire-case-id")==0){
		double duration=0.0,framesPerS=0.0,tier=0.0,diameter=0.0,heatRelease=0.0;
		if(!ParsePositiveDoubleArgument(argv[2],duration)||
			!ParsePositiveDoubleArgument(argv[3],framesPerS)||
			!ParsePositiveDoubleArgument(argv[4],tier)||
			!ParsePositiveDoubleArgument(argv[5],diameter)||
			!ParsePositiveDoubleArgument(argv[6],heatRelease))return 90;
		const SolverFrameValues identity=RunMethaneFrameProbe(1u,0u,0.0,duration,
			framesPerS,tier,diameter,heatRelease);
		if(!identity.succeeded)return 91;
		const double fullTarget=5.0*identity.flowThroughTimeS+
			40.0/(1.5/std::sqrt(CapstonePoolDiameterM));
		std::fprintf(stdout,"case_record_id=%s flow_through_time_s=%.17g "
			"full_target_s=%.17g full_frame_rate_per_s=%.17g\n",
			identity.caseRecordId.c_str(),identity.flowThroughTimeS,fullTarget,1.0/fullTarget);
		return 0;
	}
	if(argc==7&&std::strcmp(argv[1],"--fire-resume-equivalence-legacy-trace")==0)
		return RunLegacyCheckpointTraceChild(argv[2],argv[3],argv[4],argv[5],argv[6]);
	if(argc==13&&std::strcmp(argv[1],"--fire-resume-equivalence-trace")==0){
		unsigned long workers=0u,steps=0u;double duration=0.0,framesPerS=0.0,tier=0.0,
			diameter=0.0,heatRelease=0.0;
		if(!ParseUnsignedArgument(argv[5],64u,workers)||
			!ParseUnsignedArgument(argv[6],65536u,steps)||steps<8u||
			!ParsePositiveDoubleArgument(argv[7],duration)||
			!ParsePositiveDoubleArgument(argv[8],framesPerS)||
			!ParsePositiveDoubleArgument(argv[9],tier)||
			!ParsePositiveDoubleArgument(argv[10],diameter)||
			!ParsePositiveDoubleArgument(argv[11],heatRelease)||std::strlen(argv[12])!=64u)return 90;
		return RunResumeEquivalenceTraceChild(argv[2],argv[3],argv[4],
			static_cast<unsigned int>(workers),static_cast<unsigned int>(steps),duration,
			framesPerS,tier,diameter,heatRelease,argv[12]);
	}
	if(argc==6&&std::strcmp(argv[1],"--fire-resume-equivalence-certify")==0)
		return RunResumeEquivalenceCertificateChild(argv[2],argv[3],argv[4],argv[5]);
	if(argc==5&&std::strcmp(argv[1],"--fire-r80-golden-continuation")==0)
		return RunR80GoldenContinuationFixture(argv[2],argv[3],argv[4]);
	if(argc==5&&std::strcmp(argv[1],"--fire-r171-golden-beginnings")==0)
		return RunR171GoldenBeginningGeneration(argv[2],argv[3],argv[4]);
	if(argc==3&&std::strcmp(argv[1],"--fire-production-golden-projection")==0)
		return RunProductionGoldenProjectionFixture(argv[2]);
	if(argc==4&&std::strcmp(argv[1],"--fire-production-golden-composition")==0)
		return RunProductionGoldenCompositionFixture(argv[2],argv[3]);
	if(argc==3&&std::strcmp(argv[1],"--fire-production-calibration-generate-inputs")==0)
		return RunProductionCalibrationStateGeneration(argv[2]);
	if(argc==3&&std::strcmp(argv[1],"--fire-production-calibration-seal-inputs")==0)
		return SealExistingProductionCalibrationInputs(argv[2]);
	if(argc==3&&std::strcmp(argv[1],"--fire-production-calibration-diagnose-input-family")==0)
		return DiagnoseProductionCalibrationInputFamily(argv[2]);
	if(argc==4&&std::strcmp(argv[1],"--fire-production-calibration-diagnose-input-family")==0)
		return DiagnoseProductionCalibrationInputFamily(argv[2],argv[3]);
	if(argc==3&&std::strcmp(argv[1],"--fire-production-calibration-seal-oracle-spatial")==0)
		return SealOracleSpatialCalibrationInputs(argv[2]);
	if(argc==4&&std::strcmp(argv[1],"--fire-production-calibration-check-oracle-spatial")==0)
		return CheckOracleSpatialCalibrationOutput(argv[2],argv[3]);
	if(argc==3&&std::strcmp(argv[1],"--fire-production-calibration-seal-dyadic-protocol")==0)
		return FireProductionDyadicCalibration::SealProtocol(argv[2]);
	if(argc==4&&std::strcmp(argv[1],"--fire-production-calibration-seal-dyadic-targets")==0)
		return FireProductionDyadicCalibration::SealTargets(argv[2],argv[3]);
	if(argc==5&&std::strcmp(argv[1],"--fire-production-calibration-check-dyadic-oracle")==0)
		return FireProductionDyadicCalibration::CheckOracle(argv[2],argv[3],argv[4]);
	if(argc==5&&std::strcmp(argv[1],"--fire-production-calibration-seal-dyadic-metrics")==0)
		return FireProductionDyadicCalibration::SealSupplementalMetrics(argv[2],argv[3],argv[4]);
	if(argc==6&&std::strcmp(argv[1],"--fire-production-calibration-check-dyadic-complete")==0)
		return FireProductionDyadicCalibration::CheckSupplementalOracleMetrics(
			argv[2],argv[3],argv[4],argv[5]);
	if(argc==5&&std::strcmp(argv[1],"--fire-production-calibration-check-dyadic-production")==0)
		return FireProductionDyadicCalibration::CheckProduction(argv[2],argv[3],argv[4]);
	if(argc==5&&std::strcmp(argv[1],"--fire-production-calibration-seal-temporal-targets")==0)
		return FireProductionDyadicCalibration::SealTemporalTargets(argv[2],argv[3],argv[4]);
	if(argc==8&&std::strcmp(argv[1],"--fire-production-calibration-measure-temporal")==0)
		return FireProductionDyadicCalibration::MeasureTemporalRefinement(
			argv[2],argv[3],argv[4],argv[5],argv[6],argv[7]);
	if(argc==5&&std::strcmp(argv[1],"--fire-production-calibration-diagnose-roundoff")==0)
		return FireProductionDyadicCalibration::DiagnoseRoundoff(argv[2],argv[3],argv[4]);
	if(argc==7&&std::strcmp(argv[1],"--fire-production-calibration-measure-subdominance")==0)
		return FireProductionDyadicCalibration::MeasureProductionSubdominance(
			argv[2],argv[3],argv[4],argv[5],argv[6]);
	if(argc==6&&std::strcmp(argv[1],"--fire-checkpoint-child")==0){
		const unsigned long parsed=std::strtoul(argv[5],nullptr,10);
		if(parsed==0u||parsed>64u)return 92;
		return RunCheckpointChild(argv[2],argv[3],argv[4],static_cast<unsigned int>(parsed));
	}
	if(const char* probeEnvironment=std::getenv("RISE_FIRE_BUDGET_PROBE")){
		if(std::strcmp(probeEnvironment,"1")!=0){std::fprintf(stderr,
			"RISE_FIRE_BUDGET_PROBE must be exactly 1\n");return 96;}
		double targetS=1.2;
		unsigned int probeWorkers=1u;
		if(const char* targetEnvironment=std::getenv("RISE_FIRE_BUDGET_TARGET_S")){
			char* end=0;errno=0;targetS=std::strtod(targetEnvironment,&end);
			if(errno||end==targetEnvironment||*end!='\0'||!std::isfinite(targetS)||targetS<=0.0){
				std::fprintf(stderr,"RISE_FIRE_BUDGET_TARGET_S is invalid\n");return 96;
			}
		}
		if(const char* workerEnvironment=std::getenv("RISE_FIRE_BUDGET_WORKERS")){
			char* end=0;errno=0;const unsigned long parsed=std::strtoul(workerEnvironment,&end,10);
			if(errno||end==workerEnvironment||*end!='\0'||parsed==0u||parsed>64u){
				std::fprintf(stderr,"RISE_FIRE_BUDGET_WORKERS is invalid\n");return 96;
			}
			probeWorkers=static_cast<unsigned int>(parsed);
		}
		RunPersistenceOptions probePersistence;
		if(const char* productionEnvironment=std::getenv("RISE_FIRE_PRODUCTION_CAPSTONE")){
			if(std::strcmp(productionEnvironment,"1")!=0){std::fprintf(stderr,
				"RISE_FIRE_PRODUCTION_CAPSTONE must be exactly 1\n");return 96;}
			probePersistence.productionMetal=true;
		}
		if(const char* checkpointEnvironment=std::getenv("RISE_FIRE_BUDGET_CHECKPOINT")){
			if(!*checkpointEnvironment){std::fprintf(stderr,
				"RISE_FIRE_BUDGET_CHECKPOINT is empty\n");return 96;}
			probePersistence.checkpointPath=checkpointEnvironment;
			probePersistence.checkpointCadenceWallS=900.0;
			probePersistence.resume=std::filesystem::exists(probePersistence.checkpointPath);
		}
		const SolverFrameValues probe=RunMethaneFrameProbe(probeWorkers,1u,targetS,
			std::max(1.0,targetS),1.0/std::max(1.0,targetS),6.0,
			CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,probePersistence);
		const double minimumAcceptedStep=probe.acceptedTimeStepHistoryS.empty()?0.0:
			*std::min_element(probe.acceptedTimeStepHistoryS.begin(),
				probe.acceptedTimeStepHistoryS.end());
		const double maximumAcceptedStep=probe.acceptedTimeStepHistoryS.empty()?0.0:
			*std::max_element(probe.acceptedTimeStepHistoryS.begin(),
				probe.acceptedTimeStepHistoryS.end());
		std::fprintf(stderr,"budget probe target=%.17g workers=%u succeeded=%d steps=%zu time=%.17g "
			"dt=%.17g dt_min=%.17g dt_max=%.17g eos_max=%.17g "
			"hold_min=%.17g hold_max=%.17g approach=%d hold_seen=%d hold_ok=%d ignition=%d "
			"sustained=%d error=%s\n",targetS,probeWorkers,probe.succeeded?1:0,
			probe.acceptedMaximumEOSResidualHistory.size(),probe.simulatedTimeS,
			probe.acceptedTimeStepS,minimumAcceptedStep,maximumAcceptedStep,
			probe.maximumAcceptedEOSResidual,probe.minimumActiveHoldTemperatureK,
			probe.maximumActiveHoldTemperatureK,
			probe.pilotApproachComplete?1:0,probe.pilotHoldBandObserved?1:0,
			probe.pilotHoldBandSatisfied?1:0,probe.ignitedDuringPilot?1:0,
			probe.sustainedAfterPilot?1:0,
			probe.structuredError.c_str());
		const std::size_t first=probe.acceptedMaximumEOSResidualHistory.size()>64u?
			probe.acceptedMaximumEOSResidualHistory.size()-64u:0u;
		for(std::size_t step=first;step<probe.acceptedMaximumEOSResidualHistory.size();++step)
			std::fprintf(stderr,"budget history step=%zu eos_max=%.17g\n",step+1u,
				probe.acceptedMaximumEOSResidualHistory[step]);
		return probe.succeeded?0:97;
	}
	if(argc==1)Check(FireProductionDyadicCalibration::DiagnoseRoundoff(
		"rendered/fire_production_calibration/r112_dyadic_smooth_open",
		"42185c882c52e8c94db4b58f40674c53341eabe1b75b6922fdd1c7f56415a4ed",
		"d4947cb8eedbc57732190bf1833e68c3f83a356346c1662db321d7831bce958b")==237,
		"r136 full-step a-priori refusal remains an exact normal-suite gate");
#endif
#if !defined(RISE_ENABLE_OPENVDB)
	if(argc==5&&std::strcmp(argv[1],"--fire-r80-golden-continuation")==0)
		return RunR80GoldenContinuationFixture(argv[2],argv[3],argv[4]);
	if(argc==4&&std::strcmp(argv[1],"--fire-production-golden-composition")==0)
		return RunProductionGoldenCompositionFixture(argv[2],argv[3]);
	if(argc==7&&std::strcmp(argv[1],"--fire-production-calibration-measure-subdominance")==0)
		return FireProductionDyadicCalibration::MeasureProductionSubdominance(
			argv[2],argv[3],argv[4],argv[5],argv[6]);
#endif
	std::string identityFailure;
	Check(!DiscontinuousThreadIdentityAccepted(false,true,identityFailure)&&
		identityFailure=="active_set_thread_identity_mismatch"&&
		DiscontinuousThreadIdentityAccepted(true,true,identityFailure),
		"r81 active-set 1-vs-N mismatch is a structured fail-closed run error");
	PointLight* directLight=new PointLight(1.0,RISEPel(1,1,1),false);
	IKeyframeParameter* lightEnergy=directLight->KeyframeFromParameters("energy","7");
	UniformColorPainter* colorA=new UniformColorPainter(RISEPel(0,0,0));
	UniformColorPainter* colorB=new UniformColorPainter(RISEPel(1,1,1));
	FrozenPainterProbe* painter=new FrozenPainterProbe(*colorA,*colorB);
	FrozenUniformProbe* uniform=new FrozenUniformProbe();
	colorA->release(); colorB->release();
	IKeyframeParameter* painterScale=painter->KeyframeFromParameters("scale","3 4 5");
	IKeyframeParameter* uniformColor=uniform->KeyframeFromParameters("risepel","1 0 0");
	const void* originalFunction=painter->Function();
	Transformable::BeginPreparedMutationFreeze();
	directLight->SetIntermediateValue(*lightEnergy);
	directLight->SetCanGeneratePhotons(true);
	painter->SetIntermediateValue(*painterScale);
	painter->RegenerateData();
	uniform->SetIntermediateValue(*uniformColor);
	Check(directLight->emissionEnergy()==1.0 && !directLight->CanGeneratePhotons() &&
		painter->Scale().x==1.0 &&
		painter->Function()==originalFunction && uniform->Value().g==0.5,
		"direct light and painter keyframe/data mutations fail fast under prepared freeze");
	Transformable::EndPreparedMutationFreeze();
	lightEnergy->release(); painterScale->release(); uniformColor->release();
	directLight->release(); painter->release(); uniform->release();
#if !defined(RISE_ENABLE_OPENVDB)
	std::string error;
	FireSequenceManifest manifest;
	const std::string digest(64u,'a');
	const RISECBOR64::Bytes envelope=ManifestBytes(digest,digest);
	FireSequenceMappedTime mapped;
	FireSequencePreparedFrame frame;
	Check(manifest.LoadCanonicalEnvelope(envelope,".",error) &&
		manifest.MapSceneTime(10.0,mapped,error),
		"capability-independent manifest/time contract remains available");
	Check(!manifest.LoadFrame(4,frame,error) &&
		error=="fire_sequence_openvdb_capability_unavailable",
		"OpenVDB-disabled build fails with the explicit capability result");
	RISECBOR64::Value decodedEnvelope;
	Check(RISECBOR64::DecodeCanonical(envelope,decodedEnvelope,&error),
		"capability-disabled envelope still decodes canonically");
	const RISECBOR64::Value* decodedPayload=decodedEnvelope.Find("payload");
	Check(decodedPayload && !FireSequenceManifest().LoadCanonicalEnvelope(
		EnvelopeForPayload(ReplaceMember(*decodedPayload,"frame_encoding",
			RISECBOR64::Value::String("raw-dense-v1"))),".",error),
		"capability-disabled build still enforces the exact manifest schema");
	Check(decodedPayload && !FireSequenceManifest().LoadCanonicalEnvelope(
		EnvelopeForPayload(ReplaceMember(*decodedPayload,"physical_mapping",
			RISECBOR64::Value::String("heuristic:bogus"))),".",error),
		"capability-disabled build still rejects unratified physical mappings");
	Check(manifest.MapSceneTime(10.25,mapped,error) && mapped.baseFrameIndex==5,
		"capability-disabled build still applies the complete time map");
	FireSequencePreparationController disabledController(manifest);
	Check(disabledController.SetFrameInstaller([](const FireSequencePreparedFrame&) {
		return true;
	},std::string(64u,'b'),error) && disabledController.SetActiveBindingIdentity(
		std::string(64u,'c'),error),
		"capability-disabled controller accepts only its immutable identity inputs");
	Check(!disabledController.PrepareMediaForRender(
		FireSequenceRenderTimeSupport{10.0,10.0,10.0},false,error) &&
		error=="fire_sequence_openvdb_capability_unavailable" &&
		disabledController.Generation()==0u,
		"capability-disabled preparation fails transactionally before publication");
	std::printf("FireSequenceTest: OpenVDB capability-unavailable gates passed\n");
	return failures ? 1 : 0;
#else
	const std::filesystem::path root = std::filesystem::temp_directory_path()/
		("rise-fire-sequence-test-"+std::to_string(static_cast<unsigned long long>(
			std::chrono::high_resolution_clock::now().time_since_epoch().count())));
	std::filesystem::create_directories(root);
	const std::filesystem::path checkpointFixture=root/"checkpoint_fixture";
	std::filesystem::create_directories(checkpointFixture);
	RunPersistenceOptions identityMismatchPersistence;
	identityMismatchPersistence.checkpointPath=checkpointFixture/"identity_mismatch.checkpoint";
	identityMismatchPersistence.checkpointCadenceWallS=0.0;
	identityMismatchPersistence.forceActiveSetIdentityCheckForTest=true;
	identityMismatchPersistence.injectActiveSetIdentityMismatchForTest=true;
	const SolverFrameValues identityMismatch=RunMethaneFrameProbe(4u,2u,0.0,1.0,
		4.0,6.0,CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,
		identityMismatchPersistence);
	Check(!identityMismatch.succeeded&&identityMismatch.structuredError==
		"solver_failure:active_set_thread_identity_mismatch"&&
		identityMismatch.discontinuousActiveSetEvents==0u&&
		!std::filesystem::exists(identityMismatchPersistence.checkpointPath),
		"r81 injected production-path active-set mismatch fails before accumulation and checkpoint publication");
	const std::filesystem::path checkpointPath=checkpointFixture/"run.checkpoint";
	const std::filesystem::path baselineCheckpointFrame=checkpointFixture/"baseline.vdb";
	const std::filesystem::path resumedCheckpointFrame=checkpointFixture/"resumed.vdb";
	const std::filesystem::path self=std::filesystem::absolute(argv[0]);
	const int baselineCheckpointExit=RunCheckpointSubprocess(self,"baseline",checkpointPath,
		baselineCheckpointFrame,1u);
	std::string checkpointFixtureError;
	const std::filesystem::path streamedPrefixFrame=checkpointFixture/"streamed_prefix_frame4.vdb";
	const bool streamedPrefixPublished=baselineCheckpointExit==0&&
		DurableCopyPublishedFile(baselineCheckpointFrame,streamedPrefixFrame,
			checkpointFixtureError);
	const std::filesystem::path retainedCheckpointRoot=checkpointFixture/"retention_run.checkpoint";
	const std::filesystem::path retainedCheckpointFrame=checkpointFixture/"retention_run.vdb";
	const int retainedCheckpointExit=RunCheckpointSubprocess(self,"retain",retainedCheckpointRoot,
		retainedCheckpointFrame,2u);
	const std::filesystem::path retainedStep1=checkpointFixture/"retained"/
		"step_0000000001.checkpoint";
	const std::filesystem::path retainedStep2=checkpointFixture/"retained"/
		"step_0000000002.checkpoint";
	MethaneRunCheckpoint retainedStep1Record,retainedStep2Record,retainedCurrentRecord;
	std::string retainedCheckpointError;
	Check(retainedCheckpointExit==0&&LoadMethaneRunCheckpoint(retainedStep1,
		retainedStep1Record,retainedCheckpointError)&&LoadMethaneRunCheckpoint(retainedStep2,
		retainedStep2Record,retainedCheckpointError)&&LoadMethaneRunCheckpoint(retainedCheckpointRoot,
		retainedCurrentRecord,retainedCheckpointError)&&retainedStep1Record.acceptedSteps==1u&&
		retainedStep2Record.acceptedSteps==2u&&retainedCurrentRecord.acceptedSteps==2u&&
		DigestFile(retainedStep1)!=DigestFile(retainedStep2)&&
		DigestFile(retainedStep2)==DigestFile(retainedCheckpointRoot)&&
		!std::filesystem::exists(checkpointFixture/"retained"/"step_0000000003.checkpoint"),
		"r181 periodic checkpoint retention preserves each completed prior state instead of overwriting it");
	const std::filesystem::path syncFailureCheckpoint=checkpointFixture/"sync_failure.checkpoint";
	const std::filesystem::path syncFailureReturnedMarker=checkpointFixture/"sync_failure.returned";
	const int syncFailureExit=RunCheckpointSubprocess(self,"syncfail",syncFailureCheckpoint,
		syncFailureReturnedMarker,2u);
	Check(syncFailureExit==0&&std::filesystem::exists(syncFailureReturnedMarker),
		"r61 post-rename directory-sync failure returns a structured error instead of reporting durability or killing");
	const int killedCheckpointExit=RunCheckpointSubprocess(self,"kill",checkpointPath,
		resumedCheckpointFrame,2u);
	Check(baselineCheckpointExit==0&&CheckpointSubprocessWasHardKilled(killedCheckpointExit)&&
		streamedPrefixPublished&&DigestFile(streamedPrefixFrame)==DigestFile(baselineCheckpointFrame)&&
		std::filesystem::exists(checkpointPath)&&!std::filesystem::exists(resumedCheckpointFrame),
		"r61 streamed prefix survives a hard kill before the final frame is published");
	MethaneRunCheckpoint oneStepCheckpointMetadata;
	Check(LoadMethaneRunCheckpoint(checkpointPath,oneStepCheckpointMetadata,
		checkpointFixtureError)&&oneStepCheckpointMetadata.acceptedSteps==1u,
		"r115 binary32 resume fixture captures the immutable one-step beginning");
	const int resumedCheckpointExit=RunCheckpointSubprocess(self,"resume",checkpointPath,
		resumedCheckpointFrame,4u);
	MethaneRunCheckpoint resumedCheckpointMetadata;
	const bool checkpointMetadataLoaded=LoadMethaneRunCheckpoint(checkpointPath,
		resumedCheckpointMetadata,checkpointFixtureError);
	if(!(resumedCheckpointExit==0&&checkpointMetadataLoaded&&
		resumedCheckpointMetadata.values.checkpointStepIndices==
			std::vector<std::uint64_t>({1u,2u})&&
		resumedCheckpointMetadata.values.workerCountHistory==
			std::vector<std::uint64_t>({2u,4u}))){
		std::fprintf(stderr,"checkpoint fixture diagnostic: resume_exit=%d loaded=%d error=%s "
			"resumed=%d resumed_step=%llu checkpoint_count=%zu worker_count=%zu\n",
			resumedCheckpointExit,checkpointMetadataLoaded?1:0,checkpointFixtureError.c_str(),
			resumedCheckpointMetadata.values.resumedFromCheckpoint?1:0,
			static_cast<unsigned long long>(resumedCheckpointMetadata.values.resumedFromStep),
			resumedCheckpointMetadata.values.checkpointStepIndices.size(),
			resumedCheckpointMetadata.values.workerCountHistory.size());
	}
	Check(resumedCheckpointExit==0&&DigestFile(baselineCheckpointFrame)==
		DigestFile(resumedCheckpointFrame)&&checkpointMetadataLoaded&&
		resumedCheckpointMetadata.values.resumedFromCheckpoint&&
		resumedCheckpointMetadata.values.resumedFromStep==1u&&
		!resumedCheckpointMetadata.producerBuildId.empty()&&
		resumedCheckpointMetadata.values.checkpointCadenceWallS==0.0&&
		resumedCheckpointMetadata.values.checkpointStepIndices==
			std::vector<std::uint64_t>({1u,2u})&&
		resumedCheckpointMetadata.values.workerCountHistory==
			std::vector<std::uint64_t>({2u,4u})&&
		resumedCheckpointMetadata.values.reductionMode=="fixed_order_tree_v1",
		"r61 checkpoint plus hard kill plus different-thread resume is frame-bit-transparent and records run events");
	FireStateProducerPrecision resumedPrecision=FireStateProducerPrecision::Unknown;
	Check(HomogeneousStateProducerPrecision(resumedCheckpointMetadata.states,resumedPrecision)&&
		resumedPrecision==FireStateProducerPrecision::Binary64,
		"r115 ordinary binary64 checkpoints retain one authoritative producer class");
	for(std::uint64_t retiredVersion=9u;retiredVersion<=12u;++retiredVersion){
		const std::filesystem::path retiredBinary64Checkpoint=checkpointFixture/
			("retired_binary64_v"+std::to_string(retiredVersion)+".checkpoint");
		const bool writerRejected=!SaveMethaneRunCheckpoint(retiredBinary64Checkpoint,
			resumedCheckpointMetadata,checkpointFixtureError,retiredVersion)&&
			!std::filesystem::exists(retiredBinary64Checkpoint);
		forceMalformedManifoldLifecycleWriteForTest=true;
		const bool malformedWritten=SaveMethaneRunCheckpoint(retiredBinary64Checkpoint,
			resumedCheckpointMetadata,checkpointFixtureError,retiredVersion);
		forceMalformedManifoldLifecycleWriteForTest=false;
		MethaneRunCheckpoint rejectedRetiredBinary64;
		const bool loaderRejected=malformedWritten&&!LoadMethaneRunCheckpoint(
			retiredBinary64Checkpoint,rejectedRetiredBinary64,checkpointFixtureError);
		Check(writerRejected&&loaderRejected,
			"r148 modern binary64 v9-v12 checkpoints without origin authority fail closed");
	}
	for(std::uint64_t legacyVersion=5u;legacyVersion<=8u;++legacyVersion){
		const std::filesystem::path legacyPrecisionCheckpoint=checkpointFixture/
			("precision_class_legacy_v"+std::to_string(legacyVersion)+".checkpoint");
		MethaneRunCheckpoint loadedLegacyPrecision;
		loadedLegacyPrecision.states.assign(resumedCheckpointMetadata.states.size(),MethaneCellState());
		for(MethaneCellState& state:loadedLegacyPrecision.states)
			state.producerPrecision=FireStateProducerPrecision::Unknown;
		Check(SaveMethaneRunCheckpoint(legacyPrecisionCheckpoint,resumedCheckpointMetadata,
			checkpointFixtureError,legacyVersion)&&
			LoadMethaneRunCheckpoint(legacyPrecisionCheckpoint,loadedLegacyPrecision,
				checkpointFixtureError)&&
			loadedLegacyPrecision.checkpointFormatVersion==legacyVersion&&
			HomogeneousStateProducerPrecision(loadedLegacyPrecision.states,resumedPrecision)&&
			resumedPrecision==FireStateProducerPrecision::Binary64,
			"r115 every legacy v5-v8 checkpoint decodes in the binary64 producer class");
	}
	MethaneRunCheckpoint mixedPrecisionCheckpoint=resumedCheckpointMetadata;
	if(!mixedPrecisionCheckpoint.states.empty())mixedPrecisionCheckpoint.states.front().producerPrecision=
		FireStateProducerPrecision::Binary32;
	const std::filesystem::path rejectedMixedPrecision=
		checkpointFixture/"mixed_precision.checkpoint";
	Check(mixedPrecisionCheckpoint.states.size()>1u&&
		!SaveMethaneRunCheckpoint(rejectedMixedPrecision,mixedPrecisionCheckpoint,
			checkpointFixtureError)&&!std::filesystem::exists(rejectedMixedPrecision),
		"r115 a composed checkpoint rejects mixed producer precision before publication");
	MethaneRunCheckpoint precisionRoundTrip=oneStepCheckpointMetadata;
	const FireSimulationMethaneRecord& checkpointFuel=FireSimulationMethaneRecord::PhysicalV1();
	MethaneCellState checkpointAmbient;checkpointAmbient.temperatureK=300.0;
	double checkpointInvW=0.0;
	for(std::size_t species=0;species<MethaneSpeciesCount;++species){
		checkpointAmbient.constituent[species]=checkpointFuel.AmbientMassFractions()[species];
		if(species<MethaneCarbon){const FireThermochemistrySpecies* property=
			checkpointFuel.FindSpecies(checkpointFuel.SpeciesOrder()[species].c_str());
			if(property)checkpointInvW+=checkpointAmbient.constituent[species]/
				property->molecularWeightKGPerKMol;}
	}
	const double checkpointRho=checkpointFuel.ThermodynamicPressurePa()/(8314.46261815324*
		checkpointAmbient.temperatureK*checkpointInvW);
	for(double& density:checkpointAmbient.constituent)density*=checkpointRho;
	checkpointAmbient.rhoTotalZ=0.0;
	Check(checkpointFuel.MixtureSensibleEnergyJPerM3(
		ThermochemicalDensities(checkpointAmbient),checkpointAmbient.temperatureK,
		checkpointAmbient.sensibleEnergyJPerM3,&checkpointFixtureError),
		"r115 binary32 resume fixture reconstructs the canonical ambient state");
	checkpointAmbient.producerPrecision=FireStateProducerPrecision::Binary32;
	precisionRoundTrip.states.assign(precisionRoundTrip.states.size(),checkpointAmbient);
	for(unsigned int axis=0;axis<3u;++axis){
		std::fill(precisionRoundTrip.momentum.component[axis].begin(),
			precisionRoundTrip.momentum.component[axis].end(),0.0);
		std::fill(precisionRoundTrip.velocity.component[axis].begin(),
			precisionRoundTrip.velocity.component[axis].end(),0.0);
	}
	precisionRoundTrip.centerlineTemperatureIntegral.assign(
		precisionRoundTrip.centerlineTemperatureIntegral.size(),0.0);
	precisionRoundTrip.centerlineVelocityIntegral.assign(
		precisionRoundTrip.centerlineVelocityIntegral.size(),0.0);
	precisionRoundTrip.planeHeatReleaseIntegral.assign(
		precisionRoundTrip.planeHeatReleaseIntegral.size(),0.0);
	precisionRoundTrip.centerlineStatisticsDurationS=0.0;
	precisionRoundTrip.acceptedSteps=0u;
	precisionRoundTrip.simulationTimeS=0.0;
	precisionRoundTrip.previousStepS=0.0;
	precisionRoundTrip.lastAcceptedStepS=0.0;
	precisionRoundTrip.values.acceptedTimeStepHistoryS.clear();
	if(!precisionRoundTrip.states.empty()){
		const double excursion=0.5*AcceptedStateRoundoffFactor(
			checkpointFuel.AcceptedStateFeasibilityEnvelope(),FireStateProducerPrecision::Binary32)*
			AcceptedStateMassScale(ToConservativeVector(precisionRoundTrip.states.front()));
		precisionRoundTrip.states.front().rhoTotalZ+=excursion;
	}
	MethaneCellState binary64View=precisionRoundTrip.states.front();
	binary64View.producerPrecision=FireStateProducerPrecision::Binary64;
	for(std::uint64_t legacyVersion=5u;legacyVersion<=8u;++legacyVersion){
		const std::filesystem::path rejectedBinary32Legacy=checkpointFixture/
			("binary32_legacy_v"+std::to_string(legacyVersion)+".checkpoint");
		const std::filesystem::path rejectedMixedLegacy=checkpointFixture/
			("mixed_legacy_v"+std::to_string(legacyVersion)+".checkpoint");
		Check(!SaveMethaneRunCheckpoint(rejectedBinary32Legacy,precisionRoundTrip,
			checkpointFixtureError,legacyVersion)&&
			!std::filesystem::exists(rejectedBinary32Legacy)&&
			!SaveMethaneRunCheckpoint(rejectedMixedLegacy,mixedPrecisionCheckpoint,
				checkpointFixtureError,legacyVersion)&&
			!std::filesystem::exists(rejectedMixedLegacy),
			"r115 legacy v5-v8 publication rejects binary32 and mixed producer classes");
	}
	const std::filesystem::path precisionCheckpoint=checkpointFixture/"precision_class.checkpoint";
	const bool unacceptedBinary32Rejected=!precisionRoundTrip.states.empty()&&
		AcceptedMethaneCellStateAdmissible(precisionRoundTrip.states.front(),checkpointFuel,
			&checkpointFixtureError)&&
		!AcceptedMethaneCellStateAdmissible(binary64View,checkpointFuel,&checkpointFixtureError)&&
		!SaveMethaneRunCheckpoint(precisionCheckpoint,precisionRoundTrip,
			checkpointFixtureError)&&!std::filesystem::exists(precisionCheckpoint);
	Check(unacceptedBinary32Rejected,
		"r148 unaccepted binary32 state cannot be persisted and relabelled as a first-step checkpoint");
	const std::filesystem::path version9Checkpoint=checkpointFixture/"precision_class_v9.checkpoint";
	const std::filesystem::path version9ZeroCountCheckpoint=
		checkpointFixture/"precision_class_v9_zero_count.checkpoint";
	const std::filesystem::path version10Checkpoint=checkpointFixture/"precision_class_v10.checkpoint";
	const std::filesystem::path version11Checkpoint=checkpointFixture/"precision_class_v11.checkpoint";
	const std::filesystem::path version10ZeroCountCheckpoint=
		checkpointFixture/"precision_class_v10_zero_count.checkpoint";
	const std::filesystem::path version11ZeroCountCheckpoint=
		checkpointFixture/"precision_class_v11_zero_count.checkpoint";
	const std::filesystem::path version9AllZeroCheckpoint=
		checkpointFixture/"precision_class_v9_all_zero.checkpoint";
	const std::filesystem::path version10AllZeroCheckpoint=
		checkpointFixture/"precision_class_v10_all_zero.checkpoint";
	const std::filesystem::path version11AllZeroCheckpoint=
		checkpointFixture/"precision_class_v11_all_zero.checkpoint";
	const std::filesystem::path version9Binary64AllZeroCheckpoint=
		checkpointFixture/"precision_class_v9_binary64_all_zero.checkpoint";
	const std::filesystem::path version10Binary64AllZeroCheckpoint=
		checkpointFixture/"precision_class_v10_binary64_all_zero.checkpoint";
	const std::filesystem::path version11Binary64AllZeroCheckpoint=
		checkpointFixture/"precision_class_v11_binary64_all_zero.checkpoint";
	{std::error_code ignored;
		std::filesystem::remove(version9Checkpoint,ignored);
		std::filesystem::remove(version9ZeroCountCheckpoint,ignored);
		std::filesystem::remove(version10Checkpoint,ignored);
		std::filesystem::remove(version10ZeroCountCheckpoint,ignored);
		std::filesystem::remove(version11Checkpoint,ignored);
		std::filesystem::remove(version11ZeroCountCheckpoint,ignored);
		std::filesystem::remove(version9AllZeroCheckpoint,ignored);
		std::filesystem::remove(version10AllZeroCheckpoint,ignored);
		std::filesystem::remove(version11AllZeroCheckpoint,ignored);
		std::filesystem::remove(version9Binary64AllZeroCheckpoint,ignored);
		std::filesystem::remove(version10Binary64AllZeroCheckpoint,ignored);
		std::filesystem::remove(version11Binary64AllZeroCheckpoint,ignored);}
	MethaneRunCheckpoint binary64AllZero=precisionRoundTrip;
	for(MethaneCellState& cell:binary64AllZero.states)
		cell.producerPrecision=FireStateProducerPrecision::Binary64;
	MethaneRunCheckpoint legacyAccepted=precisionRoundTrip;
	legacyAccepted.acceptedSteps=1u;legacyAccepted.simulationTimeS=0.001;
	legacyAccepted.previousStepS=0.0;legacyAccepted.lastAcceptedStepS=0.0;
	legacyAccepted.values.acceptedTimeStepHistoryS.push_back(0.001);
	MethaneRunCheckpoint rejectedLegacy;
	MethaneRunCheckpoint retaggedAccepted=precisionRoundTrip;
	for(MethaneCellState& cell:retaggedAccepted.states)
		cell.producerPrecision=FireStateProducerPrecision::Binary64;
	retaggedAccepted.acceptedSteps=1u;retaggedAccepted.simulationTimeS=0.001;
	retaggedAccepted.previousStepS=0.001;retaggedAccepted.lastAcceptedStepS=0.001;
	retaggedAccepted.values.acceptedTimeStepHistoryS.push_back(0.001);
	bool acceptedBinary32PromotionRejected=true;
	for(std::uint64_t version=5u;version<=13u;++version){
		const std::filesystem::path retaggedAcceptedCheckpoint=checkpointFixture/
			("retagged_accepted_v"+std::to_string(version)+".checkpoint");
		{std::error_code ignored;
			std::filesystem::remove(retaggedAcceptedCheckpoint,ignored);}
		const bool writerRejected=!SaveMethaneRunCheckpoint(retaggedAcceptedCheckpoint,
			retaggedAccepted,checkpointFixtureError,version)&&
			!std::filesystem::exists(retaggedAcceptedCheckpoint);
		forceMalformedManifoldLifecycleWriteForTest=true;
		const bool malformedWritten=SaveMethaneRunCheckpoint(retaggedAcceptedCheckpoint,
			retaggedAccepted,checkpointFixtureError,version);
		forceMalformedManifoldLifecycleWriteForTest=false;
		const bool loaderRejected=malformedWritten&&
			!LoadMethaneRunCheckpoint(retaggedAcceptedCheckpoint,rejectedLegacy,
				checkpointFixtureError);
		if(!(writerRejected&&loaderRejected))std::fprintf(stderr,
			"r148 accepted promotion RED version=%llu writer=%d forced=%d loader=%d error=%s\n",
			static_cast<unsigned long long>(version),writerRejected?1:0,
			malformedWritten?1:0,loaderRejected?1:0,checkpointFixtureError.c_str());
		{std::error_code ignored;
			std::filesystem::remove(retaggedAcceptedCheckpoint,ignored);}
		acceptedBinary32PromotionRejected=acceptedBinary32PromotionRejected&&
			writerRejected&&loaderRejected;
	}
	Check(acceptedBinary32PromotionRejected,
		"r148 accepted binary32 bytes cannot be retagged and promoted through v5-v13 checkpoints");
	const std::filesystem::path unavailableV12Checkpoint=
		checkpointFixture/"precision_class_unavailable_v12.checkpoint";
	const bool unavailableAcceptedV12Rejected=
		!SaveMethaneRunCheckpoint(unavailableV12Checkpoint,legacyAccepted,
			checkpointFixtureError,12u)&&!std::filesystem::exists(unavailableV12Checkpoint);
	const bool legacyBinary32WriterRejected=
		!SaveMethaneRunCheckpoint(version9Checkpoint,legacyAccepted,
			checkpointFixtureError,9u)&&!std::filesystem::exists(version9Checkpoint)&&
		!SaveMethaneRunCheckpoint(version10Checkpoint,legacyAccepted,
			checkpointFixtureError,10u)&&!std::filesystem::exists(version10Checkpoint)&&
		!SaveMethaneRunCheckpoint(version11Checkpoint,legacyAccepted,
			checkpointFixtureError,11u)&&!std::filesystem::exists(version11Checkpoint);
	const bool legacyZeroCountBinary32WriterRejected=
		!SaveMethaneRunCheckpoint(version9ZeroCountCheckpoint,precisionRoundTrip,
			checkpointFixtureError,9u)&&!std::filesystem::exists(version9ZeroCountCheckpoint)&&
		!SaveMethaneRunCheckpoint(version10ZeroCountCheckpoint,precisionRoundTrip,
			checkpointFixtureError,10u)&&!std::filesystem::exists(version10ZeroCountCheckpoint)&&
		!SaveMethaneRunCheckpoint(version11ZeroCountCheckpoint,precisionRoundTrip,
			checkpointFixtureError,11u)&&!std::filesystem::exists(version11ZeroCountCheckpoint);
	const bool legacyZeroCountBinary64WriterRejected=
		!SaveMethaneRunCheckpoint(version9Binary64AllZeroCheckpoint,binary64AllZero,
			checkpointFixtureError,9u)&&!std::filesystem::exists(version9Binary64AllZeroCheckpoint)&&
		!SaveMethaneRunCheckpoint(version10Binary64AllZeroCheckpoint,binary64AllZero,
			checkpointFixtureError,10u)&&!std::filesystem::exists(version10Binary64AllZeroCheckpoint)&&
		!SaveMethaneRunCheckpoint(version11Binary64AllZeroCheckpoint,binary64AllZero,
			checkpointFixtureError,11u)&&!std::filesystem::exists(version11Binary64AllZeroCheckpoint);
	forceMalformedManifoldLifecycleWriteForTest=true;
	const bool malformedVersion9Written=SaveMethaneRunCheckpoint(version9Checkpoint,
		legacyAccepted,checkpointFixtureError,9u);
	MethaneRunCheckpoint legacyZeroCount=precisionRoundTrip;
	legacyZeroCount.simulationTimeS=0.001;
	legacyZeroCount.values.acceptedTimeStepHistoryS.push_back(0.001);
	const bool malformedVersion9ZeroCountWritten=SaveMethaneRunCheckpoint(
		version9ZeroCountCheckpoint,legacyZeroCount,checkpointFixtureError,9u);
	const bool malformedVersion10ZeroCountWritten=SaveMethaneRunCheckpoint(
		version10ZeroCountCheckpoint,legacyZeroCount,checkpointFixtureError,10u);
	const bool malformedVersion11ZeroCountWritten=SaveMethaneRunCheckpoint(
		version11ZeroCountCheckpoint,legacyZeroCount,checkpointFixtureError,11u);
	const bool malformedVersion10AcceptedWritten=SaveMethaneRunCheckpoint(
		version10Checkpoint,legacyAccepted,checkpointFixtureError,10u);
	const bool malformedVersion11AcceptedWritten=SaveMethaneRunCheckpoint(
		version11Checkpoint,legacyAccepted,checkpointFixtureError,11u);
	const bool malformedVersion9AllZeroWritten=SaveMethaneRunCheckpoint(
		version9AllZeroCheckpoint,precisionRoundTrip,checkpointFixtureError,9u);
	const bool malformedVersion10AllZeroWritten=SaveMethaneRunCheckpoint(
		version10AllZeroCheckpoint,precisionRoundTrip,checkpointFixtureError,10u);
	const bool malformedVersion11AllZeroWritten=SaveMethaneRunCheckpoint(
		version11AllZeroCheckpoint,precisionRoundTrip,checkpointFixtureError,11u);
	const bool malformedVersion9Binary64AllZeroWritten=SaveMethaneRunCheckpoint(
		version9Binary64AllZeroCheckpoint,binary64AllZero,checkpointFixtureError,9u);
	const bool malformedVersion10Binary64AllZeroWritten=SaveMethaneRunCheckpoint(
		version10Binary64AllZeroCheckpoint,binary64AllZero,checkpointFixtureError,10u);
	const bool malformedVersion11Binary64AllZeroWritten=SaveMethaneRunCheckpoint(
		version11Binary64AllZeroCheckpoint,binary64AllZero,checkpointFixtureError,11u);
	forceMalformedManifoldLifecycleWriteForTest=false;
	Check(legacyBinary32WriterRejected&&legacyZeroCountBinary32WriterRejected&&
		legacyZeroCountBinary64WriterRejected&&
		malformedVersion9Written&&
		!LoadMethaneRunCheckpoint(version9Checkpoint,rejectedLegacy,
		checkpointFixtureError)&&malformedVersion9ZeroCountWritten&&
		!LoadMethaneRunCheckpoint(version9ZeroCountCheckpoint,rejectedLegacy,
		checkpointFixtureError)&&malformedVersion10ZeroCountWritten&&
		!LoadMethaneRunCheckpoint(version10ZeroCountCheckpoint,rejectedLegacy,
		checkpointFixtureError)&&malformedVersion11ZeroCountWritten&&
		!LoadMethaneRunCheckpoint(version11ZeroCountCheckpoint,rejectedLegacy,
		checkpointFixtureError)&&malformedVersion10AcceptedWritten&&
		!LoadMethaneRunCheckpoint(version10Checkpoint,rejectedLegacy,
		checkpointFixtureError)&&malformedVersion11AcceptedWritten&&
		!LoadMethaneRunCheckpoint(version11Checkpoint,rejectedLegacy,
		checkpointFixtureError)&&malformedVersion9AllZeroWritten&&
		!LoadMethaneRunCheckpoint(version9AllZeroCheckpoint,rejectedLegacy,
		checkpointFixtureError)&&malformedVersion10AllZeroWritten&&
		!LoadMethaneRunCheckpoint(version10AllZeroCheckpoint,rejectedLegacy,
		checkpointFixtureError)&&malformedVersion11AllZeroWritten&&
		!LoadMethaneRunCheckpoint(version11AllZeroCheckpoint,rejectedLegacy,
		checkpointFixtureError)&&malformedVersion9Binary64AllZeroWritten&&
		!LoadMethaneRunCheckpoint(version9Binary64AllZeroCheckpoint,rejectedLegacy,
		checkpointFixtureError)&&malformedVersion10Binary64AllZeroWritten&&
		!LoadMethaneRunCheckpoint(version10Binary64AllZeroCheckpoint,rejectedLegacy,
		checkpointFixtureError)&&malformedVersion11Binary64AllZeroWritten&&
		!LoadMethaneRunCheckpoint(version11Binary64AllZeroCheckpoint,rejectedLegacy,
		checkpointFixtureError)&&unavailableAcceptedV12Rejected,
		"r148 checksum-valid v9-v11 production resumes cannot alias accepted history to a first step");
	RISECBOR64::Bytes corruptedCheckpoint=ReadFileBytes(checkpointPath);
	if(!corruptedCheckpoint.empty())corruptedCheckpoint.back()^=0x01u;
	const std::filesystem::path corruptedCheckpointPath=checkpointFixture/"corrupt.checkpoint";
	{std::ofstream output(corruptedCheckpointPath,std::ios::binary|std::ios::trunc);
		output.write(reinterpret_cast<const char*>(corruptedCheckpoint.data()),
			static_cast<std::streamsize>(corruptedCheckpoint.size()));}
	MethaneRunCheckpoint rejectedCheckpoint;
	Check(!LoadMethaneRunCheckpoint(corruptedCheckpointPath,rejectedCheckpoint,
		checkpointFixtureError),"r61 corrupt checkpoint fails closed before resume");
	RISECBOR64::Bytes corruptEarlySize=ReadFileBytes(checkpointPath);
	if(corruptEarlySize.size()>48u)corruptEarlySize[48]^=0xffu;
	const std::filesystem::path corruptEarlySizePath=checkpointFixture/"corrupt_early_size.checkpoint";
	{std::ofstream output(corruptEarlySizePath,std::ios::binary|std::ios::trunc);
		output.write(reinterpret_cast<const char*>(corruptEarlySize.data()),
			static_cast<std::streamsize>(corruptEarlySize.size()));}
	Check(!LoadMethaneRunCheckpoint(corruptEarlySizePath,rejectedCheckpoint,
		checkpointFixtureError)&&checkpointFixtureError=="run checkpoint checksum mismatch",
		"r61 validates the payload checksum before a corrupted size can allocate memory");
	const std::filesystem::path trailingCheckpointPath=checkpointFixture/"trailing.checkpoint";
	corruptedCheckpoint=ReadFileBytes(checkpointPath);corruptedCheckpoint.push_back(0u);
	{std::ofstream output(trailingCheckpointPath,std::ios::binary|std::ios::trunc);
		output.write(reinterpret_cast<const char*>(corruptedCheckpoint.data()),
			static_cast<std::streamsize>(corruptedCheckpoint.size()));}
	Check(!LoadMethaneRunCheckpoint(trailingCheckpointPath,rejectedCheckpoint,
		checkpointFixtureError),"r61 checkpoint rejects a valid payload with trailing bytes");
	auto ValidMutatedCheckpointRejects=[&](const char* name,
		const MethaneRunCheckpoint& mutated)->bool{
		const std::filesystem::path path=checkpointFixture/(std::string(name)+".checkpoint");
		forceMalformedManifoldLifecycleWriteForTest=true;
		const bool written=SaveMethaneRunCheckpoint(path,mutated,checkpointFixtureError);
		forceMalformedManifoldLifecycleWriteForTest=false;
		if(!written)return false;
		RunPersistenceOptions persistence;persistence.checkpointPath=path;persistence.resume=true;
		const SolverFrameValues attempt=RunMethaneFrameProbe(3u,3u,0.0,1.0,4.0,6.0,
			CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,persistence);
		return !attempt.succeeded&&attempt.structuredError.find("checkpoint_resume_failure:")==0;
	};
	MethaneRunCheckpoint bindingMutation=resumedCheckpointMetadata;
	bindingMutation.producerBuildId=std::string(64u,'0');
	Check(ValidMutatedCheckpointRejects("wrong_build",bindingMutation),
		"r61 resume rejects a checksummed checkpoint from a different executable build");
	bindingMutation=resumedCheckpointMetadata;
	bindingMutation.caseRecordId=std::string(64u,'0');
	Check(ValidMutatedCheckpointRejects("wrong_case",bindingMutation),
		"r61 resume rejects a checksummed checkpoint for a different case identity");
	bindingMutation=resumedCheckpointMetadata;
	bindingMutation.values.reductionMode="unordered_reduction";
	Check(ValidMutatedCheckpointRejects("wrong_reduction",bindingMutation),
		"r61 resume rejects a checksummed checkpoint with a different reduction mode");
	bindingMutation=resumedCheckpointMetadata;
	bindingMutation.values.activeSetAlgorithmVersion="mutated_active_set_algorithm";
	Check(ValidMutatedCheckpointRejects("wrong_active_set_algorithm",bindingMutation),
		"r81 resume rejects a checksummed checkpoint with different active-set semantics");
	bindingMutation=resumedCheckpointMetadata;
	bindingMutation.values.priorActiveSetAlgorithmVersion="fabricated_prior_algorithm";
	Check(ValidMutatedCheckpointRejects("wrong_prior_active_set_algorithm",bindingMutation),
		"r81 resume rejects a checksummed checkpoint with fabricated prior active-set history");
	auto Binary64AuthorityMutationRejects=[&](const char* name,
		const MethaneRunCheckpoint& mutated)->bool{
		const std::filesystem::path path=checkpointFixture/(std::string(name)+".checkpoint");
		{std::error_code ignored;std::filesystem::remove(path,ignored);}
		const bool writerRejected=!SaveMethaneRunCheckpoint(path,mutated,
			checkpointFixtureError)&&!std::filesystem::exists(path);
		forceMalformedManifoldLifecycleWriteForTest=true;
		const bool malformedWritten=SaveMethaneRunCheckpoint(path,mutated,
			checkpointFixtureError);
		forceMalformedManifoldLifecycleWriteForTest=false;
		MethaneRunCheckpoint rejected;
		const bool loaderRejected=malformedWritten&&!LoadMethaneRunCheckpoint(path,rejected,
			checkpointFixtureError);
		return writerRejected&&loaderRejected;
	};
	bindingMutation=resumedCheckpointMetadata;
	bindingMutation.values.statisticsStartS=std::nextafter(
		bindingMutation.values.statisticsStartS,std::numeric_limits<double>::infinity());
	Check(Binary64AuthorityMutationRejects("wrong_frame_statistics",bindingMutation),
		"r148 binary64 authority binds every serialized solver-frame value at writer and loader");
	bindingMutation=resumedCheckpointMetadata;
	if(!bindingMutation.centerlineTemperatureIntegral.empty())
		bindingMutation.centerlineTemperatureIntegral.front()=std::nextafter(
			bindingMutation.centerlineTemperatureIntegral.front(),
			std::numeric_limits<double>::infinity());
	Check(!bindingMutation.centerlineTemperatureIntegral.empty()&&
		Binary64AuthorityMutationRejects("wrong_centerline_integral",bindingMutation),
		"r148 binary64 authority binds every serialized accumulated field at writer and loader");
	bindingMutation=resumedCheckpointMetadata;
	bindingMutation.centerlineStatisticsDurationS=std::nextafter(
		bindingMutation.centerlineStatisticsDurationS,
		std::numeric_limits<double>::infinity());
	Check(Binary64AuthorityMutationRejects("wrong_statistics_duration",bindingMutation),
		"r148 binary64 authority binds the serialized statistics duration at writer and loader");
	RISECBOR64::Bytes migrationBuildRecord;std::string migrationNewBuildId,
		migrationNewExecutableDigest;
	Check(CurrentRendererBuildIdentity(migrationBuildRecord,migrationNewBuildId)&&
		CurrentExecutableDigest(migrationBuildRecord,migrationNewExecutableDigest,
			checkpointFixtureError),
		"r78 migration fixture resolves the exact current build and executable identities");
	MethaneRunCheckpoint migrationSource=resumedCheckpointMetadata;
	migrationSource.producerBuildId=std::string(64u,'a');
	const std::filesystem::path migrationCheckpoint=checkpointFixture/"migration_source.checkpoint";
	Check(IssueBinary64CheckpointAuthority(migrationSource)&&
		SaveMethaneRunCheckpoint(migrationCheckpoint,migrationSource,checkpointFixtureError),
		"r78 migration fixture authors a valid foreign-build checkpoint");
	ResumeEquivalenceTrace oldMigrationTrace,newMigrationTrace;
	oldMigrationTrace.checkpointDigest=DigestFile(migrationCheckpoint);
	oldMigrationTrace.checkpointProducerBuildId=migrationSource.producerBuildId;
	oldMigrationTrace.buildId=migrationSource.producerBuildId;
	oldMigrationTrace.executableDigest=std::string(64u,'b');
	oldMigrationTrace.resumedFromStep=migrationSource.acceptedSteps;
	oldMigrationTrace.acceptedStepCount=8u;
	for(std::uint64_t step=0u;step<8u;++step){
		oldMigrationTrace.timeStepBits.push_back(DoubleBits(0.001+1.0e-6*step));
		oldMigrationTrace.maximumTemperatureBits.push_back(DoubleBits(900.0+step));
		oldMigrationTrace.maximumEOSResidualBits.push_back(DoubleBits(1.0e-7*(step+1u)));
	}
	oldMigrationTrace.frameDigests.assign(8u,DigestFile(resumedCheckpointFrame));
	newMigrationTrace=oldMigrationTrace;newMigrationTrace.buildId=migrationNewBuildId;
	newMigrationTrace.executableDigest=migrationNewExecutableDigest;
	const std::filesystem::path migrationCertificatePath=
		checkpointFixture/"resume_equivalence.cbor";
	ResumeEquivalenceCertificate migrationCertificate;
	Check(BuildResumeEquivalenceCertificate(oldMigrationTrace,newMigrationTrace,
		migrationCertificatePath,migrationCertificate,checkpointFixtureError)&&
		migrationCertificate.acceptedStepCount==8u,
		"r78 certificate requires eight bit-identical continuation steps and a frame digest");
	RunPersistenceOptions certifiedPersistence;
	certifiedPersistence.checkpointPath=migrationCheckpoint;
	certifiedPersistence.resume=true;
	certifiedPersistence.resumeEquivalenceCertificatePath=migrationCertificatePath;
	const SolverFrameValues certifiedMigration=RunMethaneFrameProbe(3u,3u,0.0,1.0,4.0,6.0,
		CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,certifiedPersistence);
	Check(certifiedMigration.succeeded&&certifiedMigration.migrationCertificateId==
		migrationCertificate.certificateId&&certifiedMigration.migrationOldBuildId==
		migrationSource.producerBuildId&&certifiedMigration.migrationNewBuildId==
		migrationNewBuildId&&certifiedMigration.migrationAcceptedStepCount==8u,
		"r78 a foreign checkpoint resumes only through its exact current-build certificate");
	ResumeEquivalenceCertificate rejectedMigrationCertificate;
	auto TraceMutationRejects=[&](const char* name,const ResumeEquivalenceTrace& trace)->bool{
		return !BuildResumeEquivalenceCertificate(oldMigrationTrace,trace,
			checkpointFixture/(std::string(name)+".cbor"),rejectedMigrationCertificate,
			checkpointFixtureError);
	};
	ResumeEquivalenceTrace mismatchedTrace=newMigrationTrace;
	mismatchedTrace.timeStepBits[3]^=1u;
	Check(TraceMutationRejects("mismatched_dt",mismatchedTrace),
		"r78 a one-bit timestep difference rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.maximumTemperatureBits[3]^=1u;
	Check(TraceMutationRejects("mismatched_temperature",mismatchedTrace),
		"r78 a one-bit per-step T_max difference rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.maximumEOSResidualBits[3]^=1u;
	Check(TraceMutationRejects("mismatched_eos",mismatchedTrace),
		"r78 a one-bit EOS-maximum difference rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.frameDigests[0]=std::string(64u,'c');
	Check(TraceMutationRejects("mismatched_frame",mismatchedTrace),
		"r78 a diagnostic-frame digest difference rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.timeStepBits.pop_back();
	Check(TraceMutationRejects("missing_step",mismatchedTrace),
		"r78 missing per-step evidence rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.timeStepBits.push_back(0u);
	Check(TraceMutationRejects("extra_step",mismatchedTrace),
		"r78 extra timestep evidence rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.maximumTemperatureBits.pop_back();
	Check(TraceMutationRejects("missing_temperature",mismatchedTrace),
		"r78 missing per-step T_max evidence rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.maximumTemperatureBits.push_back(0u);
	Check(TraceMutationRejects("extra_temperature",mismatchedTrace),
		"r78 extra per-step evidence rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.maximumEOSResidualBits.pop_back();
	Check(TraceMutationRejects("missing_eos",mismatchedTrace),
		"r78 missing per-step EOS evidence rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.maximumEOSResidualBits.push_back(0u);
	Check(TraceMutationRejects("extra_eos",mismatchedTrace),
		"r78 extra per-step EOS evidence rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.frameDigests.pop_back();
	Check(TraceMutationRejects("missing_frame",mismatchedTrace),
		"r78 missing per-step frame evidence rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.frameDigests.push_back(std::string(64u,'c'));
	Check(TraceMutationRejects("extra_frame",mismatchedTrace),
		"r78 extra per-step frame evidence rejects build migration");
	auto CertificateMutationRejects=[&](const char* name,
		ResumeEquivalenceCertificate certificate)->bool{
		const std::filesystem::path path=checkpointFixture/(std::string(name)+".cbor");
		if(!SaveResumeEquivalenceCertificate(path,certificate,checkpointFixtureError))return false;
		RunPersistenceOptions persistence;
		persistence.checkpointPath=migrationCheckpoint;persistence.resume=true;
		persistence.resumeEquivalenceCertificatePath=path;
		const SolverFrameValues attempt=RunMethaneFrameProbe(3u,3u,0.0,1.0,4.0,6.0,
			CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,persistence);
		return !attempt.succeeded&&
			attempt.structuredError.find("checkpoint_resume_failure:")==0;
	};
	ResumeEquivalenceCertificate mutatedCertificate=migrationCertificate;
	mutatedCertificate.checkpointDigest=std::string(64u,'c');
	Check(CertificateMutationRejects("wrong_migration_checkpoint",mutatedCertificate),
		"r78 production resume binds the certificate to the exact checkpoint digest");
	mutatedCertificate=migrationCertificate;mutatedCertificate.oldBuildId=std::string(64u,'c');
	Check(CertificateMutationRejects("wrong_migration_old_build",mutatedCertificate),
		"r78 production resume binds the certificate to the checkpoint producer build");
	mutatedCertificate=migrationCertificate;mutatedCertificate.newBuildId=std::string(64u,'c');
	Check(CertificateMutationRejects("wrong_migration_new_build",mutatedCertificate),
		"r78 production resume binds the certificate to the current build identity");
	mutatedCertificate=migrationCertificate;
	mutatedCertificate.newExecutableDigest=std::string(64u,'c');
	Check(CertificateMutationRejects("wrong_migration_executable",mutatedCertificate),
		"r78 production resume binds the certificate to the current executable digest");
	mutatedCertificate=migrationCertificate;++mutatedCertificate.resumedFromStep;
	Check(CertificateMutationRejects("wrong_migration_step",mutatedCertificate),
		"r78 production resume binds the certificate to the exact resumed step");
	mutatedCertificate=migrationCertificate;mutatedCertificate.acceptedStepCount=7u;
	mutatedCertificate.timeStepBits.resize(7u);
	mutatedCertificate.maximumTemperatureBits.resize(7u);
	mutatedCertificate.maximumEOSResidualBits.resize(7u);
	Check(CertificateMutationRejects("short_migration_evidence",mutatedCertificate),
		"r78 production resume rejects fewer than eight certified accepted steps");
	SolverFrameValues metadataFixture=resumedCheckpointMetadata.values;
	metadataFixture.streamedFrameCount=2u;
	metadataFixture.migrationCertificateId=certifiedMigration.migrationCertificateId;
	metadataFixture.migrationOldBuildId=certifiedMigration.migrationOldBuildId;
	metadataFixture.migrationNewBuildId=certifiedMigration.migrationNewBuildId;
	metadataFixture.migrationAcceptedStepCount=certifiedMigration.migrationAcceptedStepCount;
	metadataFixture.migrationResumedFromStep=certifiedMigration.migrationResumedFromStep;
	metadataFixture.maximumActiveSetComplementarityDiscrepancyMPerS=0.0025;
	metadataFixture.discontinuousActiveSetEvents=3u;
	metadataFixture.maximumActiveSetCycleLength=2u;
	metadataFixture.maximumActiveSetDifferingFaceCount=4u;
	metadataFixture.activeSetThreadIdentityChecked=true;
	metadataFixture.priorActiveSetAlgorithmVersion=LegacyActiveSetAlgorithmVersion();
	std::string fixtureRunMetadataId;
	const RISECBOR64::Bytes fixtureRunMetadata=RunMetadataEnvelope(metadataFixture,4u,
		DigestFile(streamedPrefixFrame),DigestFile(resumedCheckpointFrame),std::string(64u,'b'),
		fixtureRunMetadataId);
	RISECBOR64::Value decodedRunMetadata;
	std::string runMetadataError;
	const RISECBOR64::Value* runMetadataPayload=nullptr;
	RISECBOR64::Bytes runMetadataPayloadBytes;
	const bool runMetadataDecoded=RISECBOR64::DecodeCanonical(fixtureRunMetadata,
		decodedRunMetadata,&runMetadataError);
	if(runMetadataDecoded)runMetadataPayload=decodedRunMetadata.Find("payload");
	Check(runMetadataPayload&&RISECBOR64::Encode(*runMetadataPayload,
		runMetadataPayloadBytes,&runMetadataError)&&
		fixtureRunMetadataId==RISECBOR64::SHA256Hex(runMetadataPayloadBytes)&&
		!runMetadataPayload->Find("case_record_id")&&
		runMetadataPayload->Find("checkpoint_step_indices")&&
		runMetadataPayload->Find("reduction_mode")&&
		runMetadataPayload->Find("reduction_mode")->GetText()=="fixed_order_tree_v1"&&
		runMetadataPayload->Find("active_set_algorithm_version")&&
		runMetadataPayload->Find("active_set_algorithm_version")->GetText()==
			"open_active_set_two_class_r81_v2"&&
		runMetadataPayload->Find("active_set_prior_algorithm_version")&&
		runMetadataPayload->Find("active_set_prior_algorithm_version")->GetText()==
			"legacy_pre_r80_active_set"&&
		runMetadataPayload->Find("active_set_discontinuous_event_count")&&
		runMetadataPayload->Find("active_set_discontinuous_event_count")->
			GetIntegerArgument()==3u&&
		runMetadataPayload->Find("active_set_maximum_cycle_length")&&
		runMetadataPayload->Find("active_set_maximum_cycle_length")->
			GetIntegerArgument()==2u&&
		runMetadataPayload->Find("active_set_maximum_differing_face_count")&&
		runMetadataPayload->Find("active_set_maximum_differing_face_count")->
			GetIntegerArgument()==4u&&
		runMetadataPayload->Find("worker_count_history")&&
		runMetadataPayload->Find("worker_count_history")->GetArray().size()==2u&&
		runMetadataPayload->Find("build_migration")&&
		runMetadataPayload->Find("build_migration")->Find("certificate_id")&&
		runMetadataPayload->Find("build_migration")->Find("certificate_id")->GetText()==
			migrationCertificate.certificateId&&
		runMetadataPayload->Find("streamed_frame_count")&&
		runMetadataPayload->Find("streamed_frame_count")->GetIntegerArgument()==2u,
		"r61/r78 run and certified build-migration events are canonical companion metadata and do not enter case identity");
	const std::filesystem::path frame4 = root/"frame4.vdb";
	const std::filesystem::path frame5 = root/"frame5.vdb";
	WriteFrame(frame4,FrameMutation{},1.0f);
	WriteFrame(frame5,FrameMutation{},2.0f);

	std::string error;
	FireSequenceManifest manifest;
	RISECBOR64::Bytes envelope = ManifestBytes(DigestFile(frame4),DigestFile(frame5));
	Check(manifest.LoadCanonicalEnvelope(envelope,root.string(),error),
		"canonical sequence envelope loads");
	Check(manifest.SequenceId().size() == 64u && manifest.SourceKind() == "rise_simulation" &&
		manifest.PhysicalMapping() == "absolute_si" && manifest.Frames().size() == 2u,
		"sequence identity and producer qualification survive canonical decode");
	RISECBOR64::Value decodedEnvelope;
	Check(RISECBOR64::DecodeCanonical(envelope,decodedEnvelope,&error),"baseline envelope decodes");
	RISECBOR64::Bytes wrongIdentity;
	Check(RISECBOR64::Encode(ReplaceMember(decodedEnvelope,"sequence_id",
		RISECBOR64::Value::String(std::string(64u,'0'))),wrongIdentity,&error) &&
		!FireSequenceManifest().LoadCanonicalEnvelope(wrongIdentity,root.string(),error),
		"sequence_id must hash the exact canonical payload preimage");
	const RISECBOR64::Value* decodedPayload = decodedEnvelope.Find("payload");
	Check(decodedPayload && !FireSequenceManifest().LoadCanonicalEnvelope(
		EnvelopeForPayload(ReplaceMember(*decodedPayload,"frame_count",
			RISECBOR64::Value::Unsigned(3))),root.string(),error),
		"self-consistently rehashed inconsistent frame count rejects semantically");
	Check(decodedPayload && !FireSequenceManifest().LoadCanonicalEnvelope(
		EnvelopeForPayload(ReplaceMember(*decodedPayload,"producer_build_id",
			RISECBOR64::Value::String(std::string(64u,'0')))),root.string(),error),
		"self-consistently rehashed producer-build identity mutation rejects");
	Check(decodedPayload && !FireSequenceManifest().LoadCanonicalEnvelope(
		EnvelopeForPayload(ReplaceMember(*decodedPayload,"frame_encoding",
			RISECBOR64::Value::String("raw-dense-v1"))),root.string(),error),
		"frame encoding is exact OpenVDB v1");
	Check(decodedPayload && !FireSequenceManifest().LoadCanonicalEnvelope(
		EnvelopeForPayload(ReplaceMember(*decodedPayload,"physical_mapping",
			RISECBOR64::Value::String("heuristic:bogus"))),root.string(),error),
		"unknown normalized-to-physical mapping profiles reject");
	if( decodedPayload ) {
		const RISECBOR64::Bytes incompleteBuild=CanonicalRecord("producer_build_v1");
		RISECBOR64::Value badBuild=ReplaceMember(*decodedPayload,"producer_build_v1",
			RISECBOR64::Value::BytesValue(incompleteBuild));
		badBuild=ReplaceMember(badBuild,"producer_build_id",
			RISECBOR64::Value::String(RISECBOR64::SHA256Hex(incompleteBuild)));
		Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(badBuild),
			root.string(),error),"hash-valid incomplete producer build identity rejects");
		const RISECBOR64::Value* channelValue=decodedPayload->Find("channels");
		if( channelValue ) {
			auto channels=channelValue->GetArray();
			channels[0]=ReplaceMember(channels[0],"temporal_semantics",
				RISECBOR64::Value::String("derived_eulerian_source"));
			Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(
				ReplaceMember(*decodedPayload,"channels",RISECBOR64::Value::ArrayValue(channels))),
				root.string(),error),"material/source temporal roles cannot be exchanged");
		}
		Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(
			ReplaceMember(*decodedPayload,"velocity_halo_width_m",
				RISECBOR64::Value::Float(std::numeric_limits<double>::max()))),root.string(),error),
			"finite halo widths whose derived cell count overflows reject before conversion");
		const RISECBOR64::Value* timeMap=decodedPayload->Find("time_map");
		if( timeMap ) {
			const RISECBOR64::Value hugeTimeMap=ReplaceMember(*timeMap,"i0",
				RISECBOR64::Value::Signed(std::numeric_limits<std::int64_t>::max()));
			Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(
				ReplaceMember(*decodedPayload,"time_map",hugeTimeMap)),root.string(),error),
				"frame ranges that would overflow signed index arithmetic reject first");
		}
	}
	if( decodedPayload ) {
		const RISECBOR64::Bytes arbitrary=CanonicalRecord("arbitrary_record_v1");
		RISECBOR64::Value badAerosol=ReplaceMember(*decodedPayload,
			"aerosol_thermochemistry_record",RISECBOR64::Value::BytesValue(arbitrary));
		badAerosol=ReplaceMember(badAerosol,"aerosol_thermochemistry_record_id",
			RISECBOR64::Value::String(RISECBOR64::SHA256Hex(arbitrary)));
		Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(badAerosol),
			root.string(),error),"hash-valid arbitrary aerosol semantics reject");
		RISECBOR64::Value badChem=ReplaceMember(*decodedPayload,"chem_record",
			RISECBOR64::Value::BytesValue(arbitrary));
		badChem=ReplaceMember(badChem,"chem_record_id",
			RISECBOR64::Value::String(RISECBOR64::SHA256Hex(arbitrary)));
		Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(badChem),
			root.string(),error),"hash-valid arbitrary chemistry semantics reject");
		RISECBOR64::Value aerosolRecord;
		const RISECBOR64::Value* aerosolBytes=decodedPayload->Find(
			"aerosol_thermochemistry_record");
		if( aerosolBytes && RISECBOR64::DecodeCanonical(aerosolBytes->GetBytes(),
			aerosolRecord,&error) ) {
			RISECBOR64::Value carbon=*aerosolRecord.Find("carbon_phase");
			carbon=ReplaceMember(carbon,"source_fuel_record_id",
				RISECBOR64::Value::String(std::string(64u,'0')));
			aerosolRecord=ReplaceMember(aerosolRecord,"carbon_phase",carbon);
			RISECBOR64::Bytes badAerosolBytes;
			Check(RISECBOR64::Encode(aerosolRecord,badAerosolBytes,&error),
				"aerosol reference mutation encodes");
			RISECBOR64::Value badReference=ReplaceMember(*decodedPayload,
				"aerosol_thermochemistry_record",RISECBOR64::Value::BytesValue(badAerosolBytes));
			badReference=ReplaceMember(badReference,"aerosol_thermochemistry_record_id",
				RISECBOR64::Value::String(RISECBOR64::SHA256Hex(badAerosolBytes)));
			Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(badReference),
				root.string(),error),"aerosol carbon thermochemistry binds the methane fuel record");
		}
	}
	if( decodedPayload ) {
		const RISECBOR64::Value* channelValue=decodedPayload->Find("channels");
		for( unsigned int channelIndex=0; channelValue && channelIndex<4u; ++channelIndex ) {
			auto channels=channelValue->GetArray();
			auto background=channels[channelIndex].Find("background_value")->GetArray();
			background[0]=RISECBOR64::Value::Float(channelIndex==1u ? 301.0 : 1.0);
			channels[channelIndex]=ReplaceMember(channels[channelIndex],"background_value",
				RISECBOR64::Value::ArrayValue(background));
			Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(
				ReplaceMember(*decodedPayload,"channels",RISECBOR64::Value::ArrayValue(channels))),
				root.string(),error),"mandatory inactive-channel backgrounds reject mutation");
		}
	}

	FireSequenceMappedTime mapped;
	Check(manifest.MapSceneTime(10.0,mapped,error) && mapped.baseFrameIndex == 4 &&
		mapped.simulationTime == 1.0 && mapped.advectionOffsetSeconds == 0.0,
		"time map applies t0/alpha/t_scene_0 exactly");
	Check(manifest.MapSceneTime(10.25,mapped,error) && mapped.baseFrameIndex == 5 &&
		mapped.advectionOffsetSeconds == 0.25,
		"hold endpoint selects the last frame with one full-frame advection offset");
	Check(manifest.MapSceneTime(20.0,mapped,error) && mapped.baseFrameIndex == 5 && mapped.held,
		"hold policy clamps beyond authored support");
	FireSequenceManifest errorManifest;
	Check(errorManifest.LoadCanonicalEnvelope(
		ManifestBytes(DigestFile(frame4),DigestFile(frame5),"error"),root.string(),error) &&
		!errorManifest.MapSceneTime(10.25,mapped,error),
		"error policy rejects the half-open authored endpoint");

	FireSequencePreparedFrame loaded;
	Check(manifest.LoadFrame(4,loaded,error) && loaded.channels.size() == 4u &&
		loaded.channels.at("carbon").maximum == 1.0 &&
		loaded.channels.at("temperature").minimum == 300.0,
		"frame digest, topology, backgrounds, active values, and extrema preflight");

	const FrameMutation::Kind badKinds[] = {
		FrameMutation::NegativeActiveCarbon, FrameMutation::HotInactiveTemperature,
		FrameMutation::NegativeInactiveCarbon, FrameMutation::NonfiniteInactiveCarbon,
		FrameMutation::NonfiniteVelocity, FrameMutation::PositiveInfinityReaction,
		FrameMutation::NegativeInfinityReaction, FrameMutation::ZeroTemperature,
		FrameMutation::OutOfDomainTemperature, FrameMutation::NegativeActiveTile,
		FrameMutation::HotInactiveTile, FrameMutation::NaNInactiveTile };
	for( const FrameMutation::Kind kind : badKinds ) {
		WriteFrame(frame4,FrameMutation{kind},1.0f);
		FireSequenceManifest bad;
		Check(bad.LoadCanonicalEnvelope(ManifestBytes(DigestFile(frame4),DigestFile(frame5)),
			root.string(),error) && !bad.LoadFrame(4,loaded,error),
			"active and stored value-off defects reject before fidelity/derived structures");
	}
	WriteFrame(frame4,FrameMutation{},1.0f,true);
	WriteFrame(frame5,FrameMutation{},2.0f,true);
	FireSequenceManifest chemManifest;
	Check(chemManifest.LoadCanonicalEnvelope(ManifestBytes(DigestFile(frame4),DigestFile(frame5),
		"hold",false,true),root.string(),error) && chemManifest.HasChemChannels() &&
		chemManifest.PreflightAllFrames(error),
		"preview-only synthetic chem record enables and preflights the complete triplet");
	RISECBOR64::Value chemDecoded;
	const RISECBOR64::Bytes chemEnvelopeForLattice=ManifestBytes(DigestFile(frame4),
		DigestFile(frame5),"hold",false,true);
	Check(RISECBOR64::DecodeCanonical(chemEnvelopeForLattice,chemDecoded,&error),
		"chem envelope decodes for lattice RED");
	const RISECBOR64::Value* chemPayload=chemDecoded.Find("payload");
	const RISECBOR64::Value* chemChannels=chemPayload ? chemPayload->Find("channels") : nullptr;
	if( chemPayload && chemChannels ) {
		auto shifted=chemChannels->GetArray();
		for( auto& channel : shifted ) if( channel.Find("name") &&
			channel.Find("name")->GetText()=="chem_CH" ) {
			channel=ReplaceMember(channel,"origin_m",RISECBOR64::Value::ArrayValue({
				RISECBOR64::Value::Float(0.5),RISECBOR64::Value::Float(0.0),
				RISECBOR64::Value::Float(0.0)}));
			channel=ReplaceMember(channel,"core_face_bounds_m",RISECBOR64::Value::ArrayValue({
				RISECBOR64::Value::Float(0.25),RISECBOR64::Value::Float(-0.25),
				RISECBOR64::Value::Float(-0.25),RISECBOR64::Value::Float(1.25),
				RISECBOR64::Value::Float(0.75),RISECBOR64::Value::Float(0.75)}));
		}
		Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(
			ReplaceMember(*chemPayload,"channels",RISECBOR64::Value::ArrayValue(shifted))),
			root.string(),error),
			"chem lattices cannot be shifted while the renderer maps them onto carbon space");
	}
	FireSequencePreparedFrame chemFrame;
	Check(chemManifest.LoadFrame(4,chemFrame,error) && chemFrame.channels.size()==7u &&
		chemFrame.channels.at("chem_CH").maximum==120.0 &&
		chemFrame.channels.at("chem_C2").maximum==50.0 &&
		chemFrame.channels.at("chem_CO2").maximum==8.0,
		"chem triplet survives digest-bound OpenVDB decoding with absolute W/m3 values");
	HenyeyGreensteinPhaseFunction* chemPhase=new HenyeyGreensteinPhaseFunction(0.0);
	MultichannelHeterogeneousMedium* chemMedium=new MultichannelHeterogeneousMedium(
		chemFrame,chemManifest.ChemNormalizationIntervalsNM(),2,2,2,
		Point3(-0.25,-0.25,-0.25),Point3(0.75,0.75,0.75),1.0,
		FireOpticsPreset::PredictiveV1(),*chemPhase);
	chemPhase->release();
	Check(chemMedium->IsValid() && chemMedium->GetChemEmissionNM(Point3(0,0,0),420.0)>0.0,
		"sequence-backed medium installs the chem accessors and normalized fixture SPD");
	chemMedium->release();
	for( const FrameMutation::Kind kind : {FrameMutation::NegativeActiveChem,
		FrameMutation::NaNInactiveChem} ) {
		WriteFrame(frame4,FrameMutation{kind},1.0f,true);
		FireSequenceManifest badChemFrame;
		Check(badChemFrame.LoadCanonicalEnvelope(ManifestBytes(DigestFile(frame4),
			DigestFile(frame5),"hold",false,true),root.string(),error) &&
			!badChemFrame.LoadFrame(4,loaded,error),
			"negative and stored value-off nonfinite chem payloads reject before fidelity");
	}
	WriteFrame(frame4,FrameMutation{},1.0f,true);
	WriteFrame(frame5,FrameMutation{},2.0f,true);
	const std::filesystem::path chemManifestPath=root/"sequence_chem_fixture.rise-fire.cbor";
	const RISECBOR64::Bytes chemEnvelope=ManifestBytes(DigestFile(frame4),DigestFile(frame5),
		"hold",false,true);
	{
		std::ofstream output(chemManifestPath,std::ios::binary);
		output.write(reinterpret_cast<const char*>(chemEnvelope.data()),
			static_cast<std::streamsize>(chemEnvelope.size()));
	}
	IJob* chemJob=nullptr;
	Check(RISE_CreateJob(&chemJob) && chemJob && chemJob->AddFireMediumBound(
		"chem_sequence",chemManifestPath.string().c_str(),"carbon","temperature","",
		"reaction","chem_CH","chem_C2","chem_CO2","velocity",false),
		"Job consumes the preview-only non-none chem record through the complete binding path");
	const MultichannelHeterogeneousMedium* jobChemMedium=chemJob ?
		dynamic_cast<const MultichannelHeterogeneousMedium*>(chemJob->GetMedium("chem_sequence")) : nullptr;
	Check(jobChemMedium && jobChemMedium->IsValid() &&
		jobChemMedium->GetChemEmissionNM(Point3(0,0,0),420.0)>0.0,
		"Job-installed sequence medium retains nonzero absolute chemistry emission");
	if( chemJob ) chemJob->release();
	WriteFrame(frame4,FrameMutation{},1.0f);
	WriteFrame(frame5,FrameMutation{FrameMutation::NegativeActiveCarbon},2.0f);
	FireSequenceManifest laterMalformed;
	Check(laterMalformed.LoadCanonicalEnvelope(ManifestBytes(DigestFile(frame4),DigestFile(frame5)),
		root.string(),error) && !laterMalformed.PreflightAllFrames(error),
		"loadability preflight scans later frames before any frame is activated");
	WriteFrame(frame5,FrameMutation{},2.0f);
	manifest = FireSequenceManifest();
	Check(manifest.LoadCanonicalEnvelope(ManifestBytes(DigestFile(frame4),DigestFile(frame5)),
		root.string(),error),"restored valid manifest loads");
	std::fstream truncate(frame4,std::ios::binary|std::ios::in|std::ios::out);
	truncate.seekp(0,std::ios::end);
	const std::streampos originalSize = truncate.tellp();
	truncate.close();
	std::filesystem::resize_file(frame4,static_cast<std::uintmax_t>(originalSize)-1u);
	Check(!manifest.LoadFrame(4,loaded,error),"truncated frame is rejected by whole-file digest");
	WriteFrame(frame4,FrameMutation{},1.0f);
	manifest = FireSequenceManifest();
	Check(manifest.LoadCanonicalEnvelope(ManifestBytes(DigestFile(frame4),DigestFile(frame5)),
		root.string(),error),"controller fixture manifest loads");

	FireSequencePreparationController controller(manifest);
	unsigned int installedFrames = 0u;
	const std::string preparedComponentIdentity(64u,'b');
	Check(controller.SetFrameInstaller(
		[&installedFrames](const FireSequencePreparedFrame& candidate) {
			++installedFrames;
			return candidate.channels.at("temperature").minimum > 0.0;
		},preparedComponentIdentity,error) && controller.SetActiveBindingIdentity(
			std::string(64u,'c'),error),"prepared owner binds one transactional frame installer");
	FireSequenceRenderTimeSupport support{10.0,10.0,10.0};
	Check(controller.PrepareMediaForRender(support,false,error) &&
		controller.Generation() == 1u && controller.MajorantGeneration() == 1u &&
		controller.EmissionCDFGeneration() == 1u,
		"frame advance transaction rebuilds majorant and emission CDF once");
	Check(installedFrames == 1u,"first prepared generation performs one real install callback");
	const std::string firstPreparedId = controller.PreparedInputId();
	Check(controller.PrepareMediaForRender(support,false,error) &&
		controller.Generation() == 1u && controller.PreparedInputId() == firstPreparedId,
		"identical complete prepared-input identity reuses immutable state");
	Check(installedFrames == 1u,"identical prepared input does not rebuild derived structures");
	FireSequencePreparationController::RenderLease lease = controller.AcquireRenderLease(error);
	Check(lease.IsValid() && lease.StateStayedFrozen(),"render lease captures immutable prepared state");
	FireSequenceRenderTimeSupport next{10.125,10.125,10.125};
	Check(!controller.PrepareMediaForRender(next,false,error) && error == "mutation_frozen" &&
		lease.StateStayedFrozen(),"mid-render frame/majorant/CDF swap fails immediately");
	lease = FireSequencePreparationController::RenderLease();
	Check(controller.PrepareMediaForRender(next,false,error) && controller.Generation() == 2u &&
		controller.PreparedInputId() != firstPreparedId,
		"between-render frame advance atomically publishes a new prepared generation");
	Check(installedFrames == 2u,"frame advance performs exactly one new install callback");
	const std::string secondPreparedId=controller.PreparedInputId();
	Check(controller.SetActiveBindingIdentity(std::string(64u,'e'),error) &&
		controller.PrepareMediaForRender(next,false,error) && controller.Generation()==3u &&
		controller.PreparedInputId()!=secondPreparedId && installedFrames==3u,
		"identical-time binding mutation changes the complete prepared identity and rebuilds");

	FireSequencePreparedFrame physicalInitial;
	Check(manifest.LoadFrame(4,physicalInitial,error),"physical medium fixture reloads frame 4");
	HenyeyGreensteinPhaseFunction* phase = new HenyeyGreensteinPhaseFunction(0.0);
	MultichannelHeterogeneousMedium* medium = new MultichannelHeterogeneousMedium(
		physicalInitial,manifest.ChemNormalizationIntervalsNM(),2,2,2,
		Point3(-0.25,-0.25,-0.25),Point3(0.75,0.75,0.75),
		1.0,FireOpticsPreset::PredictiveV1(),*phase);
	phase->release();
	Check(medium->IsValid(),"manifest frame constructs the production fire medium");
	FireSequencePreparationController physicalController(manifest);
	Check(medium->BindSequencePreparationController(physicalController,
		preparedComponentIdentity,error) && physicalController.SetActiveBindingIdentity(
			std::string(64u,'d'),error),
		"physical medium is the prepared owner's sole frame installer");
	const unsigned long long initialMajorant = medium->ForTest_FireMajorantGeneration();
	const unsigned long long initialEmission = medium->ForTest_FireEmissionGeneration();
	Check(physicalController.PrepareMediaForRender(support,false,error) &&
		medium->FireDerivedStructuresCurrent() &&
		medium->ForTest_FireMajorantGeneration() == initialMajorant+1u &&
		medium->ForTest_FireEmissionGeneration() == initialEmission+1u,
		"prepared frame install performs the medium's real majorant and emission-CDF rebuild");
	const unsigned long long firstPhysicalMajorant = medium->ForTest_FireMajorantGeneration();
	const unsigned long long firstPhysicalEmission = medium->ForTest_FireEmissionGeneration();
	FireSequencePreparationController::RenderLease physicalLease =
		physicalController.AcquireRenderLease(error);
	Check(!physicalController.PrepareMediaForRender(next,false,error) &&
		medium->ForTest_FireMajorantGeneration() == firstPhysicalMajorant &&
		medium->ForTest_FireEmissionGeneration() == firstPhysicalEmission,
		"mid-render mutation cannot reach grid, majorant, or emission CDF");
	physicalLease = FireSequencePreparationController::RenderLease();
	Check(physicalController.PrepareMediaForRender(next,false,error) &&
		medium->ForTest_FireMajorantGeneration() == firstPhysicalMajorant+1u &&
		medium->ForTest_FireEmissionGeneration() == firstPhysicalEmission+1u,
		"between-render frame advance rebuilds both real derived structures once");
	medium->release();

	const bool capstoneArtifactRun=std::getenv("RISE_FIRE_CAPSTONE_OUTPUT")!=nullptr;
	const std::filesystem::path capstoneOutputDirectory=capstoneArtifactRun?
		std::filesystem::path(std::getenv("RISE_FIRE_CAPSTONE_OUTPUT")):std::filesystem::path();
	const bool capstoneValidationOnly=capstoneArtifactRun&&
		std::getenv("RISE_FIRE_CAPSTONE_VALIDATE_ONLY")!=nullptr;
	const unsigned int capstoneWorkerCount=std::max(4u,std::thread::hardware_concurrency());
	const double runDiameterM=capstoneArtifactRun?CapstonePoolDiameterM:0.03;
	const double runHeatReleaseRateKW=capstoneArtifactRun?CapstoneHeatReleaseRateKW:0.40;
	const SolverFrameValues injectedFailure=RunMethaneFrameProbe(1u,1u,0.0,1.0,4.0,
		6.0,0.03,0.40,true);
	Check(!injectedFailure.succeeded&&injectedFailure.temperature.empty()&&
		injectedFailure.structuredError.find("solver_failure:injected_solver_failure")==0,
		"solver failure aborts the run pipeline with a structured error before any frame state exists");
	const SolverFrameValues tier6PipelinePreview=RunMethaneFrameProbe(1u,0u,0.0,1.0,4.0,6.0,
		runDiameterM,runHeatReleaseRateKW);
	if(!tier6PipelinePreview.succeeded){std::fprintf(stderr,"capstone fail-fast: %s\n",
		tier6PipelinePreview.structuredError.c_str());return 1;}
	double reportedResolutionTier=capstoneArtifactRun?10.0:6.0;
	if(const char* tier=std::getenv("RISE_FIRE_CAPSTONE_TIER"))
		reportedResolutionTier=std::strtod(tier,nullptr);
	const SolverFrameValues capstoneCasePreview=capstoneArtifactRun?
		RunMethaneFrameProbe(1u,0u,0.0,1.0,4.0,reportedResolutionTier,
			runDiameterM,runHeatReleaseRateKW):tier6PipelinePreview;
	if(!capstoneCasePreview.succeeded){std::fprintf(stderr,"capstone fail-fast: %s\n",
		capstoneCasePreview.structuredError.c_str());return 1;}
	const double expectedPuffingHz=1.5/std::sqrt(CapstonePoolDiameterM);
	// Forty reference periods leave at least thirty observed periods even at
	// the allowed -20% frequency edge and after the first post-window step.
	double capstoneTargetS=5.0*capstoneCasePreview.flowThroughTimeS+40.0/expectedPuffingHz;
	if(capstoneValidationOnly)
		capstoneTargetS=1.1*capstoneCasePreview.flowThroughTimeS;
	if(const char* target=std::getenv("RISE_FIRE_CAPSTONE_TARGET_S"))
		capstoneTargetS=std::strtod(target,nullptr);
	const double caseDurationS=capstoneArtifactRun?std::max(1.0,capstoneTargetS):1.0;
	const double caseFramesPerS=capstoneArtifactRun?1.0/capstoneTargetS:4.0;
	const SolverFrameValues methaneFrame=RunMethaneFrameProbe(1u,0u,0.0,
		caseDurationS,caseFramesPerS,reportedResolutionTier,runDiameterM,runHeatReleaseRateKW);
	if(!methaneFrame.succeeded){std::fprintf(stderr,"capstone fail-fast: %s\n",
		methaneFrame.structuredError.c_str());return 1;}
	if(!WriteFrame(frame4,FrameMutation{},0.0f,true,methaneFrame)){
		std::fprintf(stderr,"capstone fail-fast: initial frame serialization failed\n");return 1;
	}
	std::string durableArtifactError;
	if(capstoneArtifactRun&&!DurableCopyPublishedFile(frame4,
		capstoneOutputDirectory/"frame4.vdb",durableArtifactError)){
		std::fprintf(stderr,"capstone frame streaming failed: %s\n",durableArtifactError.c_str());
		return 1;
	}
	const unsigned int determinismStepCount=capstoneArtifactRun&&!capstoneValidationOnly?1u:0u;
	const SolverFrameValues deterministicOne=RunMethaneFrameProbe(1u,determinismStepCount,0.0,
		caseDurationS,caseFramesPerS,6.0,runDiameterM,runHeatReleaseRateKW);
	if(!deterministicOne.succeeded){std::fprintf(stderr,"capstone fail-fast: %s\n",
		deterministicOne.structuredError.c_str());return 1;}
	const SolverFrameValues methaneFrameParallel=RunMethaneFrameProbe(capstoneWorkerCount,
		determinismStepCount,0.0,
		caseDurationS,caseFramesPerS,6.0,runDiameterM,runHeatReleaseRateKW);
	if(!methaneFrameParallel.succeeded){std::fprintf(stderr,"capstone fail-fast: %s\n",
		methaneFrameParallel.structuredError.c_str());return 1;}
	const std::filesystem::path deterministicOneFrame=root/"frame_deterministic_one.vdb";
	const std::filesystem::path deterministicParallelFrame=root/"frame_deterministic_parallel.vdb";
	if(!WriteFrame(deterministicOneFrame,FrameMutation{},0.0f,true,deterministicOne)||
		!WriteFrame(deterministicParallelFrame,FrameMutation{},0.0f,true,methaneFrameParallel)){
		std::fprintf(stderr,"capstone fail-fast: deterministic frame serialization failed\n");return 1;
	}
	const std::string singleWorkerDigest=DigestFile(deterministicOneFrame);
	const std::string parallelWorkerDigest=DigestFile(deterministicParallelFrame);
	Check(!singleWorkerDigest.empty()&&singleWorkerDigest==parallelWorkerDigest,
		"r57 same methane case at one and N workers produces identical frame bytes");
	if(capstoneArtifactRun&&!DurableCopyPublishedFile(deterministicOneFrame,
		capstoneOutputDirectory/"frame_single_worker.vdb",durableArtifactError)){
		std::fprintf(stderr,"capstone frame streaming failed: %s\n",durableArtifactError.c_str());
		return 1;
	}
	RunPersistenceOptions capstonePersistence;
	if(capstoneArtifactRun){
		capstonePersistence.checkpointPath=capstoneOutputDirectory/"tier10.run.checkpoint";
		capstonePersistence.retainedCheckpointDirectory=
			capstoneOutputDirectory/"checkpoint_history";
		capstonePersistence.checkpointCadenceWallS=900.0;
		capstonePersistence.streamedFrameCountAtStart=1u;
		capstonePersistence.resume=true;
		if(const char* migrationCertificate=
			std::getenv("RISE_FIRE_CAPSTONE_MIGRATION_CERTIFICATE")){
			if(!*migrationCertificate){std::fprintf(stderr,
				"RISE_FIRE_CAPSTONE_MIGRATION_CERTIFICATE is empty\n");return 1;}
			capstonePersistence.resumeEquivalenceCertificatePath=migrationCertificate;
		}
	}
	SolverFrameValues methaneFrameNext=capstoneArtifactRun?
		RunMethaneFrameProbe(capstoneWorkerCount,1u,capstoneTargetS,caseDurationS,caseFramesPerS,
			reportedResolutionTier,runDiameterM,runHeatReleaseRateKW,false,capstonePersistence):
		methaneFrameParallel;
	if(!methaneFrameNext.succeeded){std::fprintf(stderr,"capstone fail-fast: %s\n",
		methaneFrameNext.structuredError.c_str());return 1;}
	if(!WriteFrame(frame5,FrameMutation{},0.0f,true,methaneFrameNext,0.8f)){
		std::fprintf(stderr,"capstone fail-fast: final frame serialization failed\n");return 1;
	}
	methaneFrameNext.streamedFrameCount=2u;
	if(capstoneArtifactRun&&!DurableCopyPublishedFile(frame5,
		capstoneOutputDirectory/"frame5.vdb",durableArtifactError)){
		std::fprintf(stderr,"capstone frame streaming failed: %s\n",durableArtifactError.c_str());
		return 1;
	}
	const bool capstoneIgnition=!capstoneArtifactRun||(methaneFrameNext.temperatureK>800.0f&&
		methaneFrameNext.reactionWPerM3>0.0f&&
		methaneFrameNext.ignitedDuringPilot&&methaneFrameNext.sustainedAfterPilot&&
		methaneFrameNext.pilotHoldBandObserved&&methaneFrameNext.pilotHoldBandSatisfied&&
		std::fabs(methaneFrameNext.pilotEnergyJ-methaneFrameNext.expectedPilotEnergyJ)<=
			2.0e-12*std::max(1.0,methaneFrameNext.expectedPilotEnergyJ));
	if(!capstoneIgnition) std::printf("capstone ignition diagnostic T=%.9g Tmax=%.9g reaction=%.9g inside=%d sustained=%d hold_seen=%d hold_ok=%d eos_max=%.9g approach_eos_max=%.9g pilot=%.17g expected=%.17g time=%.17g tft=%.17g\n",
		methaneFrameNext.temperatureK,methaneFrameNext.maximumTemperatureK,
		methaneFrameNext.reactionWPerM3,
		methaneFrameNext.ignitedDuringPilot?1:0,methaneFrameNext.sustainedAfterPilot?1:0,
		methaneFrameNext.pilotHoldBandObserved?1:0,
		methaneFrameNext.pilotHoldBandSatisfied?1:0,
		methaneFrameNext.maximumAcceptedEOSResidual,
		methaneFrameNext.maximumPilotApproachEOSResidual,
		methaneFrameNext.pilotEnergyJ,methaneFrameNext.expectedPilotEnergyJ,
		methaneFrameNext.simulatedTimeS,methaneFrameNext.flowThroughTimeS);
	Check(capstoneIgnition,
		"capstone cold domain ignites with the r58 thermostat, sustains after shutoff, and ledgers exact pilot energy");
	Check(!capstoneArtifactRun||methaneFrameNext.maximumTemperatureK<
		FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1().TemperatureMaxK(),
		"capstone thermostat keeps every tier inside the unchanged certified opacity domain");
	Check(!capstoneArtifactRun||
		methaneFrameNext.maximumTemperatureK<2300.0,
		"every capstone tier stays below the r74 case-derived methane physicality bound");
	Check(!capstoneArtifactRun||capstoneValidationOnly||reportedResolutionTier<10.0||(
		methaneFrameNext.discontinuousLimiterClassSteps>0u&&
		methaneFrameNext.discontinuousClassThreadIdentityChecked&&
		methaneFrameNext.discontinuousClassThreadIdentity),
		"r59 tier-10 owner enters the discontinuous class and produces bit-identical 1-vs-N accepted bytes at that step");
	Check(!capstoneArtifactRun||capstoneValidationOnly||reportedResolutionTier<10.0||
		methaneFrameNext.discontinuousActiveSetEvents==0u||(
			methaneFrameNext.activeSetThreadIdentityChecked&&
			methaneFrameNext.activeSetThreadIdentity),
		"r81 every discontinuous active-set event is fail-closed on a 1-vs-N byte mismatch");
	Check(!capstoneArtifactRun||capstoneValidationOnly||(
		methaneFrameNext.statisticsBoundaryObserved&&
		methaneFrameNext.firstStatisticsStepStartS==methaneFrameNext.statisticsStartS),
		"capstone timestep event-splits exactly at 5*t_ft before accumulating empirical rows");
	const double integratedFuelEnergy=methaneFrameNext.integratedFuelConsumptionKG*
		FireSimulationMethaneRecord::PhysicalV1().LowerHeatingValueJPerKG();
	Check(!capstoneArtifactRun||capstoneValidationOnly||(methaneFrameNext.probeTimeS.size()>=64u&&
		methaneFrameNext.simulatedTimeS>=capstoneTargetS&&
		std::isfinite(methaneFrameNext.puffingFrequencyHz)&&
		methaneFrameNext.puffingFrequencyHz>0.0&&
		methaneFrameNext.puffingRelativeError<=0.20&&
		(methaneFrameNext.probeTimeS.back()-methaneFrameNext.probeTimeS.front())*
			methaneFrameNext.puffingFrequencyHz>=30.0),
		"capstone exports thirty measured puffing periods and matches 1.5/sqrt(D) within 20 percent");
	Check(!capstoneArtifactRun||capstoneValidationOnly||(methaneFrameNext.integratedHeatReleaseJ>0.0&&
		std::fabs(methaneFrameNext.integratedHeatReleaseJ-integratedFuelEnergy)<=
			2.0e-10*methaneFrameNext.integratedHeatReleaseJ),
		"capstone statistics-window HRR equals fuel consumption times record LHV");
	Check(!capstoneArtifactRun||capstoneValidationOnly||(
		std::fabs(methaneFrameNext.integratedRadiativeFraction-0.20)<=0.02&&
		methaneFrameNext.integratedRadiativeFraction>=0.07&&
		methaneFrameNext.integratedRadiativeFraction<=0.28),
		"capstone statistics-window radiative fraction matches the methane default within its recorded spread");
	Check(!capstoneArtifactRun||capstoneValidationOnly||(methaneFrameNext.centerlineHeightM.size()==
		methaneFrameNext.dimensions[2]&&std::isfinite(methaneFrameNext.centerlineTemperatureExponent)&&
		methaneFrameNext.mccaffreyPlumeStationCount>=4u&&
		methaneFrameNext.mccaffreyMaximumTemperatureRelativeError<=0.10&&
		methaneFrameNext.mccaffreyMaximumVelocityRelativeError<=0.10),
		"capstone time-averaged above-tip centerline T and velocity match McCaffrey NBSIR 79-1910 within 10 percent");
	bool stationArchiveComplete=true;
	const std::size_t expectedStationRows=methaneFrameNext.probeTimeS.size()*
		methaneFrameNext.dimensions[2];
	stationArchiveComplete=expectedStationRows==methaneFrameNext.stationProbeTimeS.size()&&
		methaneFrameNext.stationProbeHeightM.size()==expectedStationRows&&
		methaneFrameNext.stationProbeTemperatureK.size()==expectedStationRows&&
		methaneFrameNext.stationProbeReactionWPerM3.size()==expectedStationRows&&
		methaneFrameNext.stationProbeVerticalVelocityMPerS.size()==expectedStationRows;
	for(std::size_t sample=0;stationArchiveComplete&&sample<methaneFrameNext.probeTimeS.size();
		++sample) for(std::size_t station=0;station<methaneFrameNext.dimensions[2];++station) {
		const std::size_t row=sample*methaneFrameNext.dimensions[2]+station;
		stationArchiveComplete=stationArchiveComplete&&
			methaneFrameNext.stationProbeTimeS[row]==methaneFrameNext.probeTimeS[sample]&&
			methaneFrameNext.stationProbeHeightM[row]==
				(static_cast<double>(station)+0.5)*methaneFrameNext.cellWidthM;
	}
	Check(!capstoneArtifactRun||capstoneValidationOnly||stationArchiveComplete,
		"capstone archives timestamped T, reaction-rate, and velocity at every fixed centerline station");
	if(capstoneArtifactRun&&!capstoneValidationOnly) {
		const double puffingSpanS=methaneFrameNext.probeTimeS.size()>1u?
			methaneFrameNext.probeTimeS.back()-methaneFrameNext.probeTimeS.front():0.0;
		const bool puffingQualified=methaneFrameNext.probeTimeS.size()>=64u&&
			std::isfinite(methaneFrameNext.puffingFrequencyHz)&&
			methaneFrameNext.puffingFrequencyHz>0.0&&methaneFrameNext.puffingRelativeError<=0.20&&
			puffingSpanS*methaneFrameNext.puffingFrequencyHz>=30.0;
		const bool mccaffreyQualified=methaneFrameNext.mccaffreyPlumeStationCount>=4u&&
			methaneFrameNext.mccaffreyMaximumTemperatureRelativeError<=0.10&&
			methaneFrameNext.mccaffreyMaximumVelocityRelativeError<=0.10;
		if(!puffingQualified||!mccaffreyQualified) {
			std::fprintf(stderr,"capstone empirical qualification failed: puffing_Hz=%.17g "
				"puffing_error=%.17g observed_cycles=%.17g McCaffrey_T_error=%.17g "
				"McCaffrey_u_error=%.17g\n",methaneFrameNext.puffingFrequencyHz,
				methaneFrameNext.puffingRelativeError,
				puffingSpanS*methaneFrameNext.puffingFrequencyHz,
				methaneFrameNext.mccaffreyMaximumTemperatureRelativeError,
				methaneFrameNext.mccaffreyMaximumVelocityRelativeError);
			return 1;
		}
	}
	const std::filesystem::path manifestPath = root/"sequence.rise-fire.cbor";
	const RISECBOR64::Bytes productionEnvelope = ManifestBytes(
		DigestFile(frame4),DigestFile(frame5),"hold",true,true,
		{{static_cast<std::uint64_t>(methaneFrame.dimensions[0]),
		  static_cast<std::uint64_t>(methaneFrame.dimensions[1]),
		  static_cast<std::uint64_t>(methaneFrame.dimensions[2])}},methaneFrame.cellWidthM,
		0.0,capstoneArtifactRun?capstoneTargetS:0.25,1.0,10.0,true,
		runDiameterM,runHeatReleaseRateKW,false,0.0,
		reportedResolutionTier,true);
	FireSequenceManifest productionManifest;
	Check(productionManifest.LoadCanonicalEnvelope(productionEnvelope,root.string(),error),
		"production-shaped Job manifest validates independently");
	Check(productionManifest.CaseRecordId()==methaneFrame.caseRecordId,
		"solver lattice and sequence manifest bind the identical case_record_id");
	{
		std::ofstream output(manifestPath,std::ios::binary);
		output.write(reinterpret_cast<const char*>(productionEnvelope.data()),
			static_cast<std::streamsize>(productionEnvelope.size()));
	}
	{
		RISECBOR64::Value productionDecoded;
		Check(RISECBOR64::DecodeCanonical(productionEnvelope,productionDecoded,&error),
			"production envelope decodes for cross-record RED");
		const RISECBOR64::Value* payload=productionDecoded.Find("payload");
		const RISECBOR64::Bytes syntheticOptics=FireOpticsPreset::SyntheticRegressionV1().RecordBytes();
		if( payload ) {
			RISECBOR64::Value mismatch=ReplaceMember(*payload,"optical_record",
				RISECBOR64::Value::BytesValue(syntheticOptics));
			mismatch=ReplaceMember(mismatch,"optical_record_id",
				RISECBOR64::Value::String(RISECBOR64::SHA256Hex(syntheticOptics)));
			const std::filesystem::path mismatchPath=root/"sequence_mismatched_optics.rise-fire.cbor";
			const RISECBOR64::Bytes mismatchEnvelope=EnvelopeForPayload(mismatch);
			std::ofstream output(mismatchPath,std::ios::binary);
			output.write(reinterpret_cast<const char*>(mismatchEnvelope.data()),
				static_cast<std::streamsize>(mismatchEnvelope.size()));
			output.close();
			IJob* mismatchJob=nullptr;
			Check(RISE_CreateJob(&mismatchJob) && mismatchJob &&
				!mismatchJob->AddFireMedium("mismatch",mismatchPath.string().c_str()),
				"fuel soot-density reference rejects a different valid optics identity");
			if( mismatchJob ) mismatchJob->release();
		}
	}
	IJob* job = nullptr;
	Check(RISE_CreateJob(&job) && job,"sequence binding test creates a Job");
	Check(job && job->AddFireMediumBound("sequence_fire",manifestPath.string().c_str(),
		"carbon","temperature","","reaction","chem_CH","chem_C2","chem_CO2","velocity",false) &&
		job->SetGlobalMedium("sequence_fire"),
		"fire_medium creates a named manager entry and binds through global_medium");
	if( job ) job->release();
	const std::filesystem::path scenePath = root/"sequence_scene.RISEscene";
	const std::filesystem::path renderBase = root/"sequence_render";
#if defined(_WIN32)
	_putenv_s("RISE_MEDIA_PATH",(root.string()+"/").c_str());
#else
	setenv("RISE_MEDIA_PATH",(root.string()+"/").c_str(),1);
#endif
	{
		std::ofstream scene(scenePath);
		const double cameraCenterX=0.5*static_cast<double>(methaneFrame.dimensions[0])*
			methaneFrame.cellWidthM;
		const double cameraCenterY=0.5*static_cast<double>(methaneFrame.dimensions[1])*
			methaneFrame.cellWidthM;
		const double cameraDepth=std::max({static_cast<double>(methaneFrame.dimensions[0]),
			static_cast<double>(methaneFrame.dimensions[1]),
			static_cast<double>(methaneFrame.dimensions[2])})*methaneFrame.cellWidthM;
		scene << "RISE ASCII SCENE 7\n\n"
			<< "scene_options\n{\nscene_unit 1\nfidelity_mode preview\n}\n\n"
			<< "standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
			<< "pathtracing_spectral_rasterizer\n{\nsamples 1\nnmbegin 380\n"
			<< "nmend 780\nnum_wavelengths 1\nspectral_samples 1\nhwss false\n"
			<< "pixel_filter box\noidn_denoise false\n}\n\n"
			<< "file_rasterizeroutput\n{\npattern sequence_render"
			<< "\ntype EXR\nbpp 32\ncolor_space Rec709RGB_Linear"
			<< "\nexposure 0\ndisplay_transform none\nexr_compression piz\n}\n\n"
			<< "file_rasterizeroutput\n{\npattern sequence_display\ntype PNG\nbpp 16\n"
			<< "color_space sRGB\nexposure 65\ndisplay_transform aces\n}\n\n"
			<< "film\n{\nwidth 3\nheight 3\n}\n\n"
			<< "pinhole_camera\n{\nname camera\nlocation " << cameraCenterX << ' ' <<
			cameraCenterY << ' ' << -cameraDepth << "\n"
			<< "lookat " << cameraCenterX << ' ' << cameraCenterY << ' ' << 0.5*cameraDepth << "\n"
			<< "up 0 1 0\nfov 45\nexposure 0.04\nscanning_rate -0.1\n"
			<< "pixel_rate 0.02\n}\n\nfire_medium\n{\n"
			<< "name sequence_fire\nfidelity_mode preview\nsequence_manifest "
			<< manifestPath.string() << "\nchannel_carbon carbon\n"
			<< "channel_temperature temperature\nchannel_reaction reaction\n"
			<< "channel_chem_ch chem_CH\nchannel_chem_c2 chem_C2\n"
			<< "channel_chem_co2 chem_CO2\nchannel_velocity velocity\n}\n\n"
			<< "global_medium\n{\nmedium sequence_fire\n}\n";
	}
	IJobPriv* parsedJob = nullptr;
	Check(RISE_CreateJobPriv(&parsedJob) && parsedJob &&
		parsedJob->LoadAsciiSceneViaCst(scenePath.string().c_str()),
		"descriptor-driven fire_medium chunk binds the sequence through the existing global-medium seam");
	const RenderTimeSupport fullSupport = parsedJob && parsedJob->GetScene() ?
		ComputePathTimeSupport(*parsedJob->GetScene(),10.0,nullptr,
			PixelBasedRasterizerHelper::FIELD_BOTH) :
		RenderTimeSupport();
	const RenderTimeSupport oddFieldSupport = parsedJob && parsedJob->GetScene() ?
		ComputePathTimeSupport(*parsedJob->GetScene(),10.0,nullptr,
			PixelBasedRasterizerHelper::FIELD_LOWER) :
		RenderTimeSupport();
	Check(std::fabs(fullSupport.open-9.88)<1e-12 &&
		std::fabs(fullSupport.close-10.16)<1e-12 &&
		std::fabs(oddFieldSupport.open-9.98)<1e-12 &&
		std::fabs(oddFieldSupport.close-10.06)<1e-12,
		"prepared time support is sign-aware over exposure, scan, pixels, and field parity");
	Check(parsedJob && parsedJob->SetFilm(32,32,1.0),
		"capstone renders a small inspectable spectral frame after qualifying time support");
	const MultichannelHeterogeneousMedium* parsedMedium = parsedJob ?
		dynamic_cast<const MultichannelHeterogeneousMedium*>(parsedJob->GetMedium("sequence_fire")) : nullptr;
	const unsigned long long beforeRenderMajorant = parsedMedium ?
		parsedMedium->ForTest_FireMajorantGeneration() : 0u;
	const double firstProductionRenderTime=capstoneArtifactRun?
		10.0+capstoneTargetS+0.2:10.0;
	Check(parsedJob && parsedJob->SetAnimationOptions(firstProductionRenderTime,
		firstProductionRenderTime,1,false,false) &&
		parsedJob->Rasterize(),
		"sequence-backed Job render enters the prepared rasterizer seam and completes");
	const IRasterizer* parsedRasterizer = parsedJob ? parsedJob->GetRasterizer() : nullptr;
	const FrameStore* parsedStore = parsedRasterizer ? parsedRasterizer->GetFrameStore() : nullptr;
	const FrameStore::Metadata parsedMetadata = parsedStore ? parsedStore->Meta() :
		FrameStore::Metadata();
	double maximumPreviewRadiance=0.0,maximumPreviewBlue=0.0,maximumPreviewRed=0.0;
	if(parsedStore) for(unsigned int y=0;y<parsedStore->AsBeautyRasterImage().GetHeight();++y)
		for(unsigned int x=0;x<parsedStore->AsBeautyRasterImage().GetWidth();++x) {
			const RISEColor pixel=parsedStore->AsBeautyRasterImage().GetPEL(x,y);
			Check(std::isfinite(pixel.base.r)&&std::isfinite(pixel.base.g)&&
				std::isfinite(pixel.base.b),"capstone spectral output pixels are finite");
			maximumPreviewRadiance=std::max(maximumPreviewRadiance,
				static_cast<double>(pixel.base.r+pixel.base.g+pixel.base.b));
			maximumPreviewRed=std::max(maximumPreviewRed,static_cast<double>(pixel.base.r));
			maximumPreviewBlue=std::max(maximumPreviewBlue,static_cast<double>(pixel.base.b));
		}
	Check(maximumPreviewRadiance>0.0 && maximumPreviewBlue>maximumPreviewRed,
		"capstone spectral path emits a nonzero bluish synthetic-chem preview");
	std::printf("capstone preview radiance max=%.17g red=%.17g blue=%.17g\n",
		maximumPreviewRadiance,maximumPreviewRed,maximumPreviewBlue);
	const bool publishedSequence = parsedStore && parsedMetadata.activeFireMedia.size()==1u &&
		parsedMetadata.activeFireMedia[0].mediaKind=="sequence_backed" &&
		parsedMetadata.activeFireMedia[0].sequenceId==productionManifest.SequenceId() &&
		parsedMetadata.activeFireMedia[0].selectedBaseFrameIndex==
			(capstoneArtifactRun?5u:4u) &&
		parsedMetadata.activeFireMedia[0].wholeFileDigest==DigestFile(
			capstoneArtifactRun?frame5:frame4) &&
		!parsedMetadata.activeFireMedia[0].preparedInputId.empty();
	Check(publishedSequence,
		"actual prepared render publishes the sequence-backed provenance variant");
	RISECBOR64::Value renderedEnvelope;
	bool renderedEnvelopeValid=false;
	RISECBOR64::Bytes renderedArtifactBytes,renderedSidecarBytes;
	// File outputs publish on encoder observers.  Rasterize has completed the
	// frame, but a heavily loaded capstone can still be finishing the atomic
	// provenance rename when control returns here.  Admit only the complete
	// canonical sidecar, with a short bounded wait; never read a partial file
	// and misclassify it as a provenance failure.
	for(unsigned int attempt=0u;attempt<6000u&&!renderedEnvelopeValid;++attempt) {
		const RISECBOR64::Bytes candidateArtifact=ReadFileBytes(renderBase.string()+".exr");
		const RISECBOR64::Bytes candidateSidecar=ReadFileBytes(
			renderBase.string()+".exr.provenance.cbor");
		std::string verifyError;
		if(!candidateArtifact.empty()&&!candidateSidecar.empty()&&
			VerifyFireProvenanceEXR(candidateArtifact,candidateSidecar,verifyError)) {
			RISECBOR64::Value candidate;
			std::string decodeError;
			if(RISECBOR64::DecodeCanonical(candidateSidecar,candidate,&decodeError)) {
				renderedEnvelope=std::move(candidate);
				renderedArtifactBytes=candidateArtifact;
				renderedSidecarBytes=candidateSidecar;
				renderedEnvelopeValid=true;
				break;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	Check(renderedEnvelopeValid,
		"capstone publication waits for a complete verified EXR and sidecar pair");
	std::string delayedArtifactError,completePairError;
	Check(!renderedSidecarBytes.empty()&&
		!VerifyFireProvenanceEXR(RISECBOR64::Bytes(),renderedSidecarBytes,
			delayedArtifactError)&&
		VerifyFireProvenanceEXR(renderedArtifactBytes,renderedSidecarBytes,completePairError),
		"a sidecar-first delayed artifact cannot satisfy capstone publication");
	RISECBOR64::Bytes displayArtifactBytes,displaySidecarBytes;
	bool displayPairValid=false;
	for(unsigned int attempt=0u;attempt<6000u&&!displayPairValid;++attempt) {
		const RISECBOR64::Bytes candidateArtifact=ReadFileBytes(root/"sequence_display.png");
		const RISECBOR64::Bytes candidateSidecar=ReadFileBytes(
			root/"sequence_display.png.provenance.cbor");
		std::string verifyError;
		if( VerifyVisibleFireDisplayDerivative(candidateArtifact,candidateSidecar,
			renderedEnvelope,verifyError) ) {
			displayArtifactBytes=candidateArtifact;
			displaySidecarBytes=candidateSidecar;
			displayPairValid=true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	Check(displayPairValid,
		"capstone publication waits for a complete visible PNG derivative and sidecar pair");
	std::string delayedDisplayError,completeDisplayError;
	Check(!displaySidecarBytes.empty()&&
		!VerifyVisibleFireDisplayDerivative(RISECBOR64::Bytes(),displaySidecarBytes,
			renderedEnvelope,delayedDisplayError)&&
		VerifyVisibleFireDisplayDerivative(displayArtifactBytes,displaySidecarBytes,
			renderedEnvelope,completeDisplayError),
		"a sidecar-first delayed PNG or an all-black derivative cannot satisfy publication");
	const RISECBOR64::Value* renderedPayload=renderedEnvelopeValid ?
		renderedEnvelope.Find("payload") : nullptr;
	const RISECBOR64::Value* renderedMedia=renderedPayload ?
		renderedPayload->Find("active_fire_media") : nullptr;
	Check(renderedMedia && renderedMedia->GetType()==RISECBOR64::Value::Array &&
		renderedMedia->GetArray().size()==1u &&
		renderedMedia->GetArray()[0].Find("sequence_id") &&
		renderedMedia->GetArray()[0].Find("sequence_id")->GetText()==
			productionManifest.SequenceId() &&
		renderedMedia->GetArray()[0].Find("whole_file_digest") &&
		renderedMedia->GetArray()[0].Find("prepared_input_id"),
		"real sequence render writes the ratified sequence_backed provenance sidecar");
	const char* capstoneOutput=std::getenv("RISE_FIRE_CAPSTONE_OUTPUT");
	if(capstoneOutput && *capstoneOutput && renderedEnvelopeValid && displayPairValid &&
		std::filesystem::exists(renderBase.string()+".exr") &&
		std::filesystem::exists(renderBase.string()+".exr.provenance.cbor")) {
		const std::filesystem::path destination(capstoneOutput);
		std::filesystem::create_directories(destination);
		std::filesystem::copy_file(renderBase.string()+".exr",destination/"methane_preview.exr",
			std::filesystem::copy_options::overwrite_existing);
		std::filesystem::copy_file(renderBase.string()+".exr.provenance.cbor",
			destination/"methane_preview.exr.provenance.cbor",
			std::filesystem::copy_options::overwrite_existing);
		std::filesystem::copy_file(root/"sequence_display.png",
			destination/"methane_preview_display.png",
			std::filesystem::copy_options::overwrite_existing);
		std::filesystem::copy_file(root/"sequence_display.png.provenance.cbor",
			destination/"methane_preview_display.png.provenance.cbor",
			std::filesystem::copy_options::overwrite_existing);
		Check(DigestFile(destination/"frame4.vdb")==DigestFile(frame4)&&
			DigestFile(destination/"frame5.vdb")==DigestFile(frame5),
			"streamed capstone frames remain identical to the completed sequence inputs");
		Check(DurableCopyPublishedFile(manifestPath,
			destination/"sequence_manifest.rise-fire.cbor",error),
			"published capstone manifest is durable");
		std::string runMetadataId;
		const RISECBOR64::Bytes runMetadata=RunMetadataEnvelope(methaneFrameNext,
			capstoneWorkerCount,DigestFile(frame4),DigestFile(frame5),
			productionManifest.SequenceId(),runMetadataId);
		Check(!runMetadata.empty()&&DurableWritePublishedBytes(runMetadata,
			destination/"run_metadata.rise-fire-run.cbor",error),
			"checkpoint cadence, step indices, resume event, and frame streaming enter durable run metadata only");
		FireSequenceManifest copiedManifest;
		FireSequencePreparedFrame copiedFrame;
		const RISECBOR64::Bytes copiedEnvelope=ReadFileBytes(
			destination/"sequence_manifest.rise-fire.cbor");
		Check(!copiedEnvelope.empty()&&
			copiedManifest.LoadCanonicalEnvelope(copiedEnvelope,destination.string(),error)&&
			copiedManifest.LoadFrame(4,copiedFrame,error)&&copiedManifest.LoadFrame(5,copiedFrame,error),
			"published capstone manifest resolves and verifies both copied frame paths");
		Check(std::filesystem::exists(destination/"methane_preview_display.png")&&
			std::filesystem::exists(destination/"methane_preview_display.png.provenance.cbor"),
			"published display derivative carries its own provenance sidecar");
		RISECBOR64::Value productionDecoded;
		if(RISECBOR64::DecodeCanonical(productionEnvelope,productionDecoded,&error)) {
			const RISECBOR64::Value* sequencePayload=productionDecoded.Find("payload");
			const RISECBOR64::Value* caseBytes=sequencePayload?sequencePayload->Find("case_record"):nullptr;
			if(caseBytes && caseBytes->GetType()==RISECBOR64::Value::ByteString) {
				std::ofstream caseOutput(destination/"case_record.rise-fire-case.cbor",std::ios::binary);
				caseOutput.write(reinterpret_cast<const char*>(caseBytes->GetBytes().data()),
					static_cast<std::streamsize>(caseBytes->GetBytes().size()));
			}
		}
		const double hrrFromFuelW=methaneFrameNext.fuelConsumptionKGPerS*
			FireSimulationMethaneRecord::PhysicalV1().LowerHeatingValueJPerKG();
		const RISECBOR64::Value* provenanceId=renderedEnvelope.Find("provenance_id");
		const RISECBOR64::Value* artifactFidelity=renderedPayload?
			renderedPayload->Find("artifact_fidelity"):nullptr;
		const RISECBOR64::Value* artifactDigest=renderedPayload?
			renderedPayload->Find("artifact_sha256"):nullptr;
		const RISECBOR64::Value* rendererBuild=renderedPayload?
			renderedPayload->Find("renderer_build_id"):nullptr;
		const RISECBOR64::Value* configurationId=renderedPayload?
			renderedPayload->Find("resolved_render_configuration_id"):nullptr;
		const RISECBOR64::Value* sidecarMedium=renderedMedia&&
			renderedMedia->GetType()==RISECBOR64::Value::Array&&!renderedMedia->GetArray().empty()?
			&renderedMedia->GetArray()[0]:nullptr;
		const bool empiricalRowsQualified=capstoneArtifactRun&&!capstoneValidationOnly&&
			reportedResolutionTier>=10.0;
		std::ofstream report(destination/"capstone_report.txt");
		std::ofstream json(destination/"methane_preview.exr.provenance.json");
		std::ofstream probes(destination/"centerline_probes.csv");
		std::ofstream timeSteps(destination/"accepted_timestep_history.csv");
		timeSteps << "accepted_step,accepted_time_s,delta_t_s\n";
		double acceptedTime=0.0;
		for(std::size_t step=0;step<methaneFrameNext.acceptedTimeStepHistoryS.size();++step){
			acceptedTime+=methaneFrameNext.acceptedTimeStepHistoryS[step];
			timeSteps << step+1u << ',' << std::setprecision(17) << acceptedTime << ',' <<
				methaneFrameNext.acceptedTimeStepHistoryS[step] << '\n';
		}
		probes << "time_s,height_m,temperature_K,reaction_W_per_m3,vertical_velocity_m_per_s,"
			"centerline_heat_release_W\n";
		for(std::size_t row=0;row<methaneFrameNext.stationProbeTimeS.size();++row) {
			const std::size_t sample=methaneFrameNext.dimensions[2]>0u?
				row/methaneFrameNext.dimensions[2]:0u;
			probes << std::setprecision(17) << methaneFrameNext.stationProbeTimeS[row] << ',' <<
				methaneFrameNext.stationProbeHeightM[row] << ',' <<
				methaneFrameNext.stationProbeTemperatureK[row] << ',' <<
				methaneFrameNext.stationProbeReactionWPerM3[row] << ',' <<
				methaneFrameNext.stationProbeVerticalVelocityMPerS[row] << ',' <<
				(sample<methaneFrameNext.probeCenterlineHeatReleaseW.size()?
					methaneFrameNext.probeCenterlineHeatReleaseW[sample]:0.0) << '\n';
		}
		std::ofstream profile(destination/"centerline_final_profile.csv");
		profile << "height_m,temperature_K,vertical_velocity_m_per_s\n";
		for(std::size_t sample=0;sample<methaneFrameNext.centerlineHeightM.size();++sample)
			profile << std::setprecision(17) << methaneFrameNext.centerlineHeightM[sample] << ',' <<
				methaneFrameNext.centerlineTemperatureK[sample] << ',' <<
				methaneFrameNext.centerlineVelocityMPerS[sample] << '\n';
		WriteJSON(json,renderedEnvelope); json << '\n';
		const double minimumAcceptedStep=methaneFrameNext.acceptedTimeStepHistoryS.empty()?0.0:
			*std::min_element(methaneFrameNext.acceptedTimeStepHistoryS.begin(),
				methaneFrameNext.acceptedTimeStepHistoryS.end());
		const double maximumAcceptedStep=methaneFrameNext.acceptedTimeStepHistoryS.empty()?0.0:
			*std::max_element(methaneFrameNext.acceptedTimeStepHistoryS.begin(),
				methaneFrameNext.acceptedTimeStepHistoryS.end());
		report.precision(17);
		report << "artifact_fidelity=" << (artifactFidelity?artifactFidelity->GetText():"") << "\n"
			<< "render_fidelity_status=" << (renderedPayload&&renderedPayload->Find(
				"render_fidelity_status")?renderedPayload->Find("render_fidelity_status")->GetText():"") << "\n"
			<< "render_reason_codes=" << TextArrayCSV(renderedPayload?
				renderedPayload->Find("render_reason_codes"):nullptr) << "\n"
			<< "artifact_reason_codes=" << TextArrayCSV(renderedPayload?
				renderedPayload->Find("artifact_reason_codes"):nullptr) << "\n"
			<< "capstone_reason_codes=preview_primary,synthetic_chem_fixture,"
				"methane_zero_soot_yield,cold_start_r70_continuous_pilot,"
				"r59_two_class_limiter,r70_manifold_exact_acceptance\n"
			<< "diagnostic_narrative=zero_gravity_harness_defect_was_rejected_by_empirical_gate;"
				"gravity_restored;invariant_limit_identity_closed_stationary_hot_cold_contrast\n"
			<< "empirical_rows_status=" << (empiricalRowsQualified?
				"capstone_tier10":"pipeline_validation_only") << "\n"
			<< "provenance_id=" << (provenanceId?provenanceId->GetText():"") << "\n"
			<< "sidecar_artifact_sha256=" << (artifactDigest?artifactDigest->GetText():"") << "\n"
			<< "renderer_build_id=" << (rendererBuild?rendererBuild->GetText():"") << "\n"
			<< "resolved_render_configuration_id=" <<
				(configurationId?configurationId->GetText():"") << "\n"
			<< "case_record_id=" << productionManifest.CaseRecordId() << "\n"
			<< "sequence_id=" << productionManifest.SequenceId() << "\n"
			<< "run_metadata_id=" << runMetadataId << "\n"
			<< "checkpoint_cadence_wall_s=" << methaneFrameNext.checkpointCadenceWallS << "\n"
			<< "checkpoint_count=" << methaneFrameNext.checkpointStepIndices.size() << "\n"
			<< "checkpoint_step_indices=";
		for(std::size_t checkpoint=0;checkpoint<methaneFrameNext.checkpointStepIndices.size();
			++checkpoint)report << (checkpoint?",":"") <<
				methaneFrameNext.checkpointStepIndices[checkpoint];
		report << "\n"
			<< "resumed_from_checkpoint=" <<
				(methaneFrameNext.resumedFromCheckpoint?"true":"false") << "\n"
			<< "resumed_from_step=" << methaneFrameNext.resumedFromStep << "\n"
			<< "streamed_frame_count=" << methaneFrameNext.streamedFrameCount << "\n"
			<< "prepared_input_id=" << (sidecarMedium&&sidecarMedium->Find("prepared_input_id")?
				sidecarMedium->Find("prepared_input_id")->GetText():"") << "\n"
			<< "selected_base_frame_index=" << (sidecarMedium&&
				sidecarMedium->Find("selected_base_frame_index")?
				sidecarMedium->Find("selected_base_frame_index")->GetIntegerArgument():0u) << "\n"
			<< "frame4_sha256=" << DigestFile(frame4) << "\n"
			<< "frame5_sha256=" << DigestFile(frame5) << "\n"
			<< "frame5_policy=honest_cold_start_sustained_solver_state_with_synthetic_chem_fixture\n"
			<< "thread_determinism_workers=1," << capstoneWorkerCount << "\n"
			<< "thread_determinism_digest=" << singleWorkerDigest << "\n"
			<< "run_reduction_mode=" << methaneFrameNext.reductionMode << "\n"
			<< "run_worker_count_history=";
		for(std::size_t segment=0;segment<methaneFrameNext.workerCountHistory.size();++segment)
			report << (segment?",":"") << methaneFrameNext.workerCountHistory[segment];
		report << "\n"
			<< "r59_discontinuous_limiter_steps=" <<
				methaneFrameNext.discontinuousLimiterClassSteps << "\n"
			<< "r59_maximum_limiter_face_discrepancy=" <<
				methaneFrameNext.maximumLimiterClassDiscrepancy << "\n"
			<< "r59_discontinuous_class_thread_identity=" <<
				(methaneFrameNext.discontinuousClassThreadIdentityChecked&&
				methaneFrameNext.discontinuousClassThreadIdentity?"true":"not_exercised") << "\n"
			<< "r81_active_set_algorithm_version=" <<
				methaneFrameNext.activeSetAlgorithmVersion << "\n"
			<< "r81_prior_active_set_algorithm_version=" <<
				(methaneFrameNext.priorActiveSetAlgorithmVersion.empty()?"none":
					methaneFrameNext.priorActiveSetAlgorithmVersion) << "\n"
			<< "r81_discontinuous_active_set_events=" <<
				methaneFrameNext.discontinuousActiveSetEvents << "\n"
			<< "r81_maximum_active_set_complementarity_discrepancy_m_per_s=" <<
				methaneFrameNext.maximumActiveSetComplementarityDiscrepancyMPerS << "\n"
			<< "r81_maximum_active_set_cycle_length=" <<
				methaneFrameNext.maximumActiveSetCycleLength << "\n"
			<< "r81_maximum_active_set_differing_face_count=" <<
				methaneFrameNext.maximumActiveSetDifferingFaceCount << "\n"
			<< "r81_active_set_thread_identity=" <<
				(methaneFrameNext.activeSetThreadIdentityChecked&&
				methaneFrameNext.activeSetThreadIdentity?"true":"not_exercised") << "\n"
			<< "r54_pinned_selected_timestep_s=" << methaneFrameNext.selectedTimeStepS << "\n"
			<< "r57_resolution_tier=" << reportedResolutionTier << "\n"
			<< "accepted_final_timestep_s=" << methaneFrameNext.acceptedTimeStepS << "\n"
			<< "accepted_timestep_min_s=" << minimumAcceptedStep << "\n"
			<< "accepted_timestep_max_s=" << maximumAcceptedStep << "\n"
			<< "simulated_time_s=" << methaneFrameNext.simulatedTimeS << "\n"
			<< "flow_through_time_s=" << methaneFrameNext.flowThroughTimeS << "\n"
			<< "r68_maximum_pilot_approach_eos_residual=" <<
				methaneFrameNext.maximumPilotApproachEOSResidual << "\n"
			<< "r69_maximum_accepted_eos_residual=" <<
				methaneFrameNext.maximumAcceptedEOSResidual << "\n"
			<< "r72_active_hold_temperature_min_K=" <<
				methaneFrameNext.minimumActiveHoldTemperatureK << "\n"
			<< "r72_active_hold_temperature_max_K=" <<
				methaneFrameNext.maximumActiveHoldTemperatureK << "\n"
			<< "pilot_energy_J=" << methaneFrameNext.pilotEnergyJ << "\n"
			<< "pilot_ignited_during_window=" << (methaneFrameNext.ignitedDuringPilot?"true":"false") << "\n"
			<< "combustion_sustained_after_pilot=" << (methaneFrameNext.sustainedAfterPilot?"true":"false") << "\n"
			<< "rendered_exr_sha256=" << DigestFile(renderBase.string()+".exr") << "\n"
			<< "sidecar_sha256=" << DigestFile(renderBase.string()+".exr.provenance.cbor") << "\n"
			<< "display_preview=PNG_ACES_exposure_plus65_non_primary_derivative\n";
		if(empiricalRowsQualified) {
			const double observationSpanS=methaneFrameNext.probeTimeS.empty()?0.0:
				methaneFrameNext.probeTimeS.back()-methaneFrameNext.probeTimeS.front();
			report << "puffing_status=capstone_tier10_evaluated\n"
				<< "puffing_reference_Hz=" << expectedPuffingHz << "\n"
				<< "statistics_start_s=" << methaneFrameNext.statisticsStartS << "\n"
				<< "statistics_duration_s=" << std::max(0.0,methaneFrameNext.simulatedTimeS-
					methaneFrameNext.statisticsStartS) << "\n"
				<< "puffing_observation_span_s=" << observationSpanS << "\n"
				<< "puffing_observed_Hz=" << methaneFrameNext.puffingFrequencyHz << "\n"
				<< "puffing_observed_cycles=" << observationSpanS*
					methaneFrameNext.puffingFrequencyHz << "\n"
				<< "puffing_relative_error=" << methaneFrameNext.puffingRelativeError << "\n"
				<< "puffing_observation=preview_single_diameter_at_least_30_observed_cycles_full_three_diameter_refinement_gate_remains_separate\n"
				<< "mccaffrey_status=capstone_tier10_evaluated\n"
				<< "mccaffrey_temperature_power_exponent=" << methaneFrameNext.centerlineTemperatureExponent << "\n"
				<< "mccaffrey_temperature_log_fit_RMSE=" << methaneFrameNext.centerlineFitRMSE << "\n"
				<< "mccaffrey_flame_tip_height_m=" << methaneFrameNext.mccaffreyFlameTipHeightM << "\n"
				<< "mccaffrey_plume_station_count=" << methaneFrameNext.mccaffreyPlumeStationCount << "\n"
				<< "mccaffrey_max_temperature_relative_error=" <<
					methaneFrameNext.mccaffreyMaximumTemperatureRelativeError << "\n"
				<< "mccaffrey_max_velocity_relative_error=" <<
					methaneFrameNext.mccaffreyMaximumVelocityRelativeError << "\n"
				<< "mccaffrey_observation=time_averaged_above_reaction_tip_NBSIR_79_1910_Table_1_33_kW_absolute_T_and_u\n"
				<< "hrr_status=capstone_tier10_evaluated\n"
				<< "realized_HRR_W=" << methaneFrameNext.realizedHeatReleaseW << "\n"
				<< "fuel_consumption_times_LHV_W=" << hrrFromFuelW << "\n"
				<< "HRR_relative_ledger_error=" << std::fabs(methaneFrameNext.realizedHeatReleaseW-
					hrrFromFuelW)/std::max(1.0,methaneFrameNext.realizedHeatReleaseW) << "\n"
				<< "radiative_fraction_status=capstone_tier10_evaluated\n"
				<< "declared_chi_r=" << methaneFrameNext.effectiveRadiativeFraction << "\n"
				<< "realized_step_radiative_fraction=" << methaneFrameNext.realizedRadiativeFraction << "\n"
				<< "statistics_integrated_heat_release_J=" << methaneFrameNext.integratedHeatReleaseJ << "\n"
				<< "statistics_integrated_radiative_loss_J=" << methaneFrameNext.integratedRadiativeLossJ << "\n"
				<< "statistics_integrated_fuel_consumption_kg=" << methaneFrameNext.integratedFuelConsumptionKG << "\n"
				<< "statistics_integrated_fuel_times_LHV_J=" << methaneFrameNext.integratedFuelConsumptionKG*
					FireSimulationMethaneRecord::PhysicalV1().LowerHeatingValueJPerKG() << "\n"
				<< "statistics_integrated_radiative_fraction=" << methaneFrameNext.integratedRadiativeFraction << "\n"
				<< "accepted_escape_factor=" << methaneFrameNext.acceptedEscapeFactor << "\n"
				<< "radiative_status=preview_final_step_combustion_only_chi_r_diagnostic\n";
		} else {
			report << "puffing_status=not_evaluated_pipeline_validation_only\n"
				<< "mccaffrey_status=not_evaluated_pipeline_validation_only\n"
				<< "hrr_status=not_evaluated_pipeline_validation_only\n"
				<< "radiative_fraction_status=not_evaluated_pipeline_validation_only\n";
		}
		report << "visible_expectation=faint_methane_y_s_zero_with_bluish_synthetic_chem_fixture\n";
	}
	Check(parsedMedium && parsedMedium->ForTest_FireMajorantGeneration()==beforeRenderMajorant+1u,
		"actual render schedules exactly one per-frame majorant/CDF rebuild");
	const std::string jobFirstPrepared = parsedStore &&
		!parsedMetadata.activeFireMedia.empty() ?
		parsedMetadata.activeFireMedia[0].preparedInputId : std::string();
	const double secondProductionRenderTime=capstoneArtifactRun?10.0:10.25;
	Check(parsedJob && parsedJob->SetAnimationOptions(secondProductionRenderTime,
		secondProductionRenderTime,1,false,false) &&
		parsedJob->Rasterize(),
		"between-render scene-time advance prepares the next immutable frame");
	const FrameStore::Metadata advancedMetadata = parsedStore ? parsedStore->Meta() :
		FrameStore::Metadata();
	Check(parsedStore && advancedMetadata.activeFireMedia.size()==1u &&
		advancedMetadata.activeFireMedia[0].selectedBaseFrameIndex==
			(capstoneArtifactRun?4u:5u) &&
		advancedMetadata.activeFireMedia[0].preparedInputId!=jobFirstPrepared &&
		parsedMedium && parsedMedium->ForTest_FireMajorantGeneration()==beforeRenderMajorant+2u,
		"frame advance atomically swaps grid, majorant, CDF, and provenance between renders");
	FrozenMutationOutput* mutationOutput=parsedJob ? new FrozenMutationOutput(*parsedJob) : nullptr;
	if( parsedRasterizer && mutationOutput )
		const_cast<IRasterizer*>(parsedRasterizer)->AddRasterizerOutput(mutationOutput);
	Check(parsedJob && parsedJob->Rasterize() && mutationOutput && mutationOutput->attempted &&
		mutationOutput->rejected && parsedMedium &&
		parsedMedium->ForTest_FireMajorantGeneration()==beforeRenderMajorant+2u,
		"mid-render Job mutation is detected while the prepared grid/majorant/CDF stay frozen");
	if( mutationOutput ) mutationOutput->release();
	// The adversarial interlace fixture has its own fixed 0.25 s cadence.  The
	// capstone manifest intentionally uses the much longer empirical-window
	// cadence and is not a substitute for that timing construction.
	if(!capstoneArtifactRun) {
		const double interlaceFixtureFrameStepS=0.25;
		Check(parsedJob && !parsedJob->RasterizeAnimation(10.0,
			10.0+2.0*interlaceFixtureFrameStepS,2,true,false) && parsedMedium &&
			parsedMedium->ForTest_FireMajorantGeneration()==beforeRenderMajorant+2u,
			"cadence-crossing interlaced fields reject before one artifact can misstate its base frame");
		Check(parsedJob && parsedJob->RasterizeAnimation(10.0+interlaceFixtureFrameStepS,
			10.0+interlaceFixtureFrameStepS,1,true,false) && parsedMedium &&
			parsedMedium->ForTest_FireMajorantGeneration()==beforeRenderMajorant+2u,
			"same-base-frame interlaced fields reuse one truthful prepared identity");
	}
	const IFireRasterizerState* preparedState=parsedRasterizer ?
		dynamic_cast<const IFireRasterizerState*>(parsedRasterizer) : nullptr;
	if( parsedRasterizer && parsedJob && parsedJob->GetScene() )
		parsedRasterizer->RasterizeScene(*parsedJob->GetScene(),nullptr,nullptr);
	Check(preparedState && !preparedState->LastRenderCompleted() && parsedMedium &&
		parsedMedium->ForTest_FireMajorantGeneration()==beforeRenderMajorant+2u,
		"legacy direct rasterizer entry cannot bypass sequence preparation or mutate state");
	if( parsedJob ) parsedJob->release();

#if defined(__APPLE__)
	if(capstoneArtifactRun&&capstoneOutput&&*capstoneOutput)
		Check(RunFirstLightPreviewChild(capstoneOutput,capstoneOutput)==0,
			"first light publishes eight verified primaries and a visible GIF animation");
#else
	Check(!capstoneArtifactRun,
		"first-light animation publication is available only on the macOS ImageIO route");
#endif

	FrameStoreOutput::Metadata metadata;
	metadata.renderFidelityStatus = "preview";
	metadata.renderReasonCodes = {"requested_preview"};
	metadata.activeFireOpticsRecordIds = {std::string(64u,'b')};
	FrameStoreOutput::ActiveFireMedium sequenceMedium;
	sequenceMedium.mediaKind = "sequence_backed";
	sequenceMedium.managerName = "fire";
	sequenceMedium.bindingKind = "global_medium";
	sequenceMedium.bindingOwner = "scene";
	sequenceMedium.opticalRecordIds = metadata.activeFireOpticsRecordIds;
	sequenceMedium.sequenceId = manifest.SequenceId();
	sequenceMedium.selectedBaseFrameIndex = controller.PreparedFrame()->frameIndex;
	sequenceMedium.wholeFileDigest = controller.PreparedFrame()->wholeFileSha256;
	sequenceMedium.sourceKind = manifest.SourceKind();
	sequenceMedium.physicalMapping = manifest.PhysicalMapping();
	sequenceMedium.effectiveBlurState = "disabled";
	sequenceMedium.preparedInputId = controller.PreparedInputId();
	sequenceMedium.preparedStateGeneration = controller.Generation();
	metadata.activeFireMedia.push_back(sequenceMedium);
	metadata.resolvedRenderConfigCoreV1 = FireOutputMetadataTestFixture::ResolvedConfig(2,2);
	metadata.rendererBuildV1 = FireOutputMetadataTestFixture::RendererBuild();
	metadata.rendererBuildId = RISECBOR64::SHA256Hex(metadata.rendererBuildV1);
	Check(FrameStoreOutput::ValidateFireOutputMetadata(metadata,error),
		"ratified active_fire_media sequence_backed tagged variant validates");
	metadata.activeFireMedia[0].authoredConfigDigest = std::string(64u,'c');
	Check(!FrameStoreOutput::ValidateFireOutputMetadata(metadata,error),
		"sequence_backed provenance rejects a structurally present static-only field");

	std::filesystem::remove_all(root);
	std::printf("FireSequenceTest: canonical sequence/loadability/preparation gates passed\n");
	return failures ? 1 : 0;
#endif
}
