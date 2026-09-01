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
#include "../src/Library/Utilities/Reference.h"
#include "../tools/fire_simulator_core.h"
#include "FireOutputMetadataTestFixture.h"
#include "FireProductionCalibrationMath.h"
#include "FireProductionCalibrationMirror.h"
#include "FireProductionRoundoffTraceAdapter.h"
#include "FireProductionRoundoffWalker.h"
#include "fire_production_trace/SourceManifest.h"

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
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
#include <type_traits>
#include <vector>

#if defined(__APPLE__)
#include <unistd.h>
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
		std::filesystem::path temporalSnapshotDirectory;
		double temporalSnapshotCadenceS=0.0;
		double maximumProductionSourceStepS=0.0;
		std::filesystem::path productionOnsetDiagnosticDirectory;
		double productionOnsetStopVelocityMPerS=0.0;
	};

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
		const double cellsAcrossBurner=CapstonePoolDiameterM/checkpoint.cellWidthM;
		std::ofstream summary(outputDirectory/"puffing_spectrum_evidence.v1",std::ios::trunc);
		summary<<std::setprecision(17)
			<<"artifact_fidelity simulation_evidence\n"
			<<"checkpoint_sha256 "<<DigestFile(checkpointPath)<<"\n"
			<<"frame_times_sha256 "<<DigestFile(simulationDirectory/"frame_times.csv")<<"\n"
			<<"display_inputs_sha256 "<<DigestFile(outputDirectory/"display_inputs.csv")<<"\n"
			<<"centerline_signal_sha256 "<<DigestFile(outputDirectory/"centerline_signal.csv")<<"\n"
			<<"lit_area_signal_sha256 "<<DigestFile(outputDirectory/"lit_area_signal.csv")<<"\n"
			<<"full_spectrum_sha256 "<<DigestFile(outputDirectory/"full_spectrum.csv")<<"\n"
			<<"resolution_tier "<<resolutionTier<<"\n"
			<<"burner_diameter_m "<<CapstonePoolDiameterM<<"\n"
			<<"cell_width_m "<<checkpoint.cellWidthM<<"\n"
			<<"burner_cells_across "<<cellsAcrossBurner<<"\n"
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
				"manifold_max,manifold_p95,manifold_p50,tail_cells,tail_drained_m3\n";
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
						profileLayoutMS=std::chrono::duration<double,std::milli>(
							std::chrono::steady_clock::now()-profileStageStart).count();
						profileStageStart=std::chrono::steady_clock::now();
						solverPhase="production resident attempt";
						RISE::FireProductionResidentStepResult production;
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
						if(const char* auditPath=std::getenv("RISE_FIRE_MOMENTUM_AUDIT_PATH"))
							effectiveMomentumAuditPath=auditPath;
						if(disposition==RISE::FireProductionResidentStepAttemptDisposition::Accepted&&
							!persistence.productionOnsetDiagnosticDirectory.empty())
							for(std::size_t threshold=0u;threshold<productionOnsetVelocityThresholds.size();
								++threshold)if(!productionOnsetThresholdCaptured[threshold]&&
								attemptMaximumVelocity>=productionOnsetVelocityThresholds[threshold]){
								onsetThresholdIndex=threshold;auditColumnX=attemptMaximumX;
								auditColumnY=attemptMaximumY;std::ostringstream name;
								name<<"threshold_"<<static_cast<unsigned int>(
									productionOnsetVelocityThresholds[threshold])<<".raw.csv";
								effectiveMomentumAuditPath=
									persistence.productionOnsetDiagnosticDirectory/name.str();break;
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
								forceAuditDiagnostics.outwardLambdaPerS==
									production.forceDiagnostics.outwardLambdaPerS;
							const bool forceScheduleIdentity=forceDiagnosticsIdentity&&
								forceCPU.schedule.substepCount==production.forceSchedule.substepCount&&
								forceCPU.schedule.substepTimeS==production.forceSchedule.substepTimeS;
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
								const double representedStep=static_cast<double>(production.representedTimeStepS);
								double columnStressMaximum=0.0,columnBuoyancyMaximum=0.0,
									columnAdvectionMaximum=0.0,columnSourceMaximum=0.0,
									columnPressureMaximum=0.0,columnRestorationMaximum=0.0,
									columnTotalMaximum=0.0,columnClosureMaximum=0.0,
									columnVremanMinimum=std::numeric_limits<double>::infinity(),
									columnVremanMaximum=0.0;
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
									const double stress=(forceCPU.momentumKGPerM2S[2][face]-beginning-gravity)/
										representedStep;
									const double buoyancy=gravity/representedStep;
									const double source=request.momentumSourceIncrement[2][face]/representedStep;
									const double advection=(production.transportedDual.momentum[2][face]-
										request.momentumSourceIncrement[2][face]-
										forceCPU.momentumKGPerM2S[2][face])/representedStep;
									const double pressure=(columnPhysicalMomentum[z]-
										production.transportedDual.momentum[2][face])/representedStep;
									const double restoration=(production.projection.momentumKGPerM2S[2][face]-
										columnPhysicalMomentum[z])/representedStep;
									const double total=(production.projection.momentumKGPerM2S[2][face]-beginning)/
										representedStep;
									const double closure=total-(stress+buoyancy+advection+source+pressure+restoration);
									const std::size_t lowerZ=z==0u?0u:z-1u;
									const std::size_t upperZ=std::min(z,request.force.shape.nz-1u);
									const std::size_t lowerCell=columnX+request.force.shape.nx*(columnY+
										request.force.shape.ny*lowerZ);
									const std::size_t upperCell=columnX+request.force.shape.nx*(columnY+
										request.force.shape.ny*upperZ);
									const double lowerVreman=
										forceFieldsCPU.eddyKinematicViscosityM2PerS[lowerCell];
									const double upperVreman=
										forceFieldsCPU.eddyKinematicViscosityM2PerS[upperCell];
									columnStressMaximum=std::max(columnStressMaximum,std::fabs(stress));
									columnBuoyancyMaximum=std::max(columnBuoyancyMaximum,std::fabs(buoyancy));
									columnAdvectionMaximum=std::max(columnAdvectionMaximum,std::fabs(advection));
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
									advancedOK=false;break;}
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
									"difference_count,force_inclusive_provisional_difference_max\n";
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
									forceInclusiveProvisionalDifferenceMaximum<<'\n';
								audit.close();
								if(!audit){lastAdvanceError="production momentum audit write failed";
									advancedOK=false;break;}
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
										if(!columnCopied){lastAdvanceError=
											"production onset skipped-threshold publication failed";
											advancedOK=false;break;}
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
										production.manifoldTailDrainedVolumeM3<<'\n';
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

	int RunProductionCheckpointPhysicsDiagnosticChild(
		const std::filesystem::path& checkpointPath,const std::filesystem::path& outputPath)
	{
		MethaneRunCheckpoint checkpoint;std::string error;
		if(!LoadMethaneRunCheckpoint(checkpointPath,checkpoint,error))return 90;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		FireCase::AuthoredV1 authored;authored.fuelRecordId=fuel.RecordId();
		authored.poolDiameterM=CapstonePoolDiameterM;
		authored.heatReleaseRateKW=CapstoneHeatReleaseRateKW;authored.envelope={{0.0,1.0}};
		authored.durationS=1.0;authored.quality="dstar";authored.numericDStarTier=10.0;
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
		double heatReleaseW=0.0,fuelConsumptionKGPerS=0.0;
		for(MethaneSourcePacket& packet:packets){
			RepresentMethaneSourcePacketBinary32(packet);
			if(!CertifiedBinary32SourcePacket(packet,fuel))return 103;
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
			"velocity=%.17g dt=%.17g gprime=%.17g HRR=%.17g consumption_LHV=%.17g\n",
			static_cast<unsigned long long>(checkpoint.acceptedSteps),checkpoint.simulationTimeS,
			maximumSpeed,checkpoint.lastAcceptedStepS,maximumReducedGravity,heatReleaseW,
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
		const std::filesystem::path& outputDirectory)
	{
#if !defined(RISE_ENABLE_OPENVDB)
		(void)resolutionTier;(void)outputDirectory;return 90;
#else
		if(resolutionTier!=6.0&&resolutionTier!=8.0&&resolutionTier!=10.0)return 91;
		std::error_code directoryError;
		std::filesystem::create_directories(outputDirectory/"checkpoints",directoryError);
		std::filesystem::create_directories(outputDirectory/"budgets",directoryError);
		if(directoryError)return 92;
		double targetTimeS=2.2;
		if(const char* target=std::getenv("RISE_FIRE_ONSET_TARGET_S")){
			char* end=nullptr;targetTimeS=std::strtod(target,&end);
			if(!end||*end!='\0'||!std::isfinite(targetTimeS)||!(targetTimeS>0.0))return 91;
		}
		RunPersistenceOptions persistence;
		persistence.productionMetal=true;
		persistence.singleStageFCTDiagnostic=
			std::getenv("RISE_FIRE_SINGLE_STAGE_FCT_ONSET")!=nullptr;
		persistence.compatibleMomentumDiagnostic=!persistence.singleStageFCTDiagnostic;
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
		if(const char* resumeCheckpoint=std::getenv("RISE_FIRE_ONSET_RESUME_CHECKPOINT")){
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
		std::ofstream summary(summaryPath,std::ios::trunc);
		summary<<std::setprecision(17)<<"schema rise.fire.production.onset_campaign.summary.v2\n"
			<<"resolution_tier "<<resolutionTier<<"\n"
			<<"operator_mode "<<(persistence.singleStageFCTDiagnostic?
				"production_single_stage_fct_diagnostic_v1":"compatible_momentum_diagnostic")<<"\n"
			<<"compatible_momentum_diagnostic "<<
				(persistence.compatibleMomentumDiagnostic?1:0)<<"\n"
			<<"single_stage_fct_diagnostic "<<
				(persistence.singleStageFCTDiagnostic?1:0)<<"\n"
			<<"producer_build_id "<<producerBuildId<<"\n"
			<<"producer_executable_sha256 "<<producerExecutableDigest<<"\n"
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
		for(const unsigned int threshold:{15u,30u,60u}){
			const std::filesystem::path budget=outputDirectory/"budgets"/
				("threshold_"+std::to_string(threshold)+".raw.csv");
			summary<<"threshold_"<<threshold<<"_captured "<<
				(std::filesystem::exists(budget)?1:0)<<"\n";
			if(std::filesystem::exists(budget))summary<<"threshold_"<<threshold<<
				"_summary_sha256 "<<DigestFile(budget)<<"\n"<<"threshold_"<<threshold<<
				"_column_sha256 "<<DigestFile(budget.string()+".column.csv")<<"\n";
		}
		summary.close();
		if(!summary||(!persistence.compatibleMomentumDiagnostic&&
			!persistence.singleStageFCTDiagnostic)||DigestFile(trajectory).empty()||
			DigestFile(retryTrajectory).empty()||
			(!persistence.singleStageFCTDiagnostic&&
			 DigestFile(outputDirectory/"final.checkpoint").empty()))return 94;
		std::fprintf(stderr,"PRODUCTION_ONSET_CAMPAIGN%s tier=%.0f target=%.17g time=%.17g steps=%zu "
			"wall_s=%.17g operator=%s build=%s trajectory=%s "
			"summary=%s\n",reachedTarget?"":"_STOP",resolutionTier,targetTimeS,
			result.simulatedTimeS,
			result.acceptedTimeStepHistoryS.size(),wallS,persistence.singleStageFCTDiagnostic?
				"production_single_stage_fct_diagnostic_v1":"compatible_momentum_diagnostic",
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
		double replayResolutionTier=0.0;
		if(checkpoint.dimensions==std::array<std::size_t,3>{{52u,52u,80u}})
			replayResolutionTier=6.0;
		else if(checkpoint.dimensions==std::array<std::size_t,3>{{69u,69u,106u}})
			replayResolutionTier=8.0;
		else if(checkpoint.dimensions==std::array<std::size_t,3>{{86u,86u,132u}})
			replayResolutionTier=10.0;
		else return 90;
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
	first.assemblyReserveFactor=first.feasibilityFactor;
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
		a.lowFlux==b.lowFlux&&a.fluxDelta==b.fluxDelta&&a.lowState==b.lowState&&
		a.limiterRatio==b.limiterRatio&&a.sharedFaceAlpha==b.sharedFaceAlpha&&
		a.accepted==b.accepted;};
	bool freshAlpha=false;if(cpu)for(unsigned int axis=0u;axis<3u;++axis)
		for(std::size_t face=0u;face<averageCPU.sharedFaceAlpha[axis].size();++face)
			freshAlpha=freshAlpha||averageCPU.sharedFaceAlpha[axis][face]!=0.5f*(
				firstCPU.sharedFaceAlpha[axis][face]+secondCPU.sharedFaceAlpha[axis][face]);
	const bool passed=computed&&metal.failureBitmap==
		std::array<std::uint32_t,3>{{0u,0u,0u}}&&metal.firstFluxPair.lowFlux==firstPair.lowFlux&&
		metal.firstFluxPair.fluxDelta==firstPair.fluxDelta&&
		metal.secondFluxPair.lowFlux==secondPair.lowFlux&&
		metal.secondFluxPair.fluxDelta==secondPair.fluxDelta&&
		metal.averagedFluxPair.lowFlux==averagePair.lowFlux&&
		metal.averagedFluxPair.fluxDelta==averagePair.fluxDelta&&
		sameSolve(metal.firstSolve,firstCPU)&&sameSolve(metal.secondSolve,secondCPU)&&
		sameSolve(metal.averagedSolve,averageCPU)&&freshAlpha&&metal.commandCommitCount==7u;
	std::fprintf(stderr,"SCALAR_FCT_METAL_STAGES computed=%d passed=%d fresh_alpha=%d "
		"commits=%u failures=%u/%u/%u error=%s\n",computed?1:0,passed?1:0,
		freshAlpha?1:0,metal.commandCommitCount,metal.failureBitmap[0],metal.failureBitmap[1],
		metal.failureBitmap[2],stageError.c_str());return passed?0:181;
}
#endif

int main(int argc,char** argv)
{
#if defined(__APPLE__)
	if(argc==2&&std::strcmp(argv[1],"--fire-production-scalar-fct-metal-stages")==0)
		return RunProductionScalarFCTMetalStageFixture();
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
	{
		MethaneRunCheckpoint tier8,tier10;double tier8Width=0.0,tier10Width=0.0;
		const bool tier8Built=CanonicalCapstoneGridForTier(
			8.0,tier8.dimensions,tier8Width);
		const bool tier10Built=CanonicalCapstoneGridForTier(
			10.0,tier10.dimensions,tier10Width);
		tier8.cellWidthM=tier8Width;tier10.cellWidthM=tier10Width;
		Check(tier8Built&&tier10Built&&
			ProductionPuffingSpectrumTierMatches(tier8,8.0)&&
			!ProductionPuffingSpectrumTierMatches(tier8,10.0)&&
			ProductionPuffingSpectrumTierMatches(tier10,10.0)&&
			!ProductionPuffingSpectrumTierMatches(tier10,8.0),
			"puffing spectrum binds the reported tier to the checkpoint grid metadata");
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
