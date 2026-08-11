//////////////////////////////////////////////////////////////////////
//
//  FileEncoderObserver.cpp - Implementation.  Each callback writes
//  a fresh artifact/sidecar transaction while preserving the legacy
//  per-frame filename pattern from FileRasterizerOutput.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FileEncoderObserver.h"
#include "FrameStore.h"

#include "../RasterImages/EXRReader.h"
#include "../RasterImages/EXRWriter.h"
#include "../Utilities/DiskFileWriteBuffer.h"
#include "../Utilities/MemoryBuffer.h"
#include "../Utilities/RISECBOR64.h"
#include "../Interfaces/ILog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <system_error>
#include <vector>

using namespace RISE;
using namespace RISE::Implementation;

std::string RISE::Implementation::BuildFrameArtifactFilename(
	const std::string& pattern,
	const std::string& suffix,
	const unsigned int frame,
	const std::string& extension,
	const bool multiple )
{
	if( !multiple ) return pattern+suffix+"."+extension;
	std::string frameText = std::to_string(frame);
	if( frameText.size() < 4u ) frameText.insert(0u,4u-frameText.size(),'0');
	return pattern+suffix+frameText+"."+extension;
}

bool RISE::Implementation::DescribeFireFrameSequenceEncoding(
	const FireFrameSequenceEncoding encoding,
	const unsigned int framesPerSecond,
	FireFrameSequenceEncodingDescriptor& descriptor,
	std::string& error )
{
	descriptor = FireFrameSequenceEncodingDescriptor();
	error.clear();
	if( framesPerSecond == 0u ) {
		error = "movie frame rate is zero";
		return false;
	}
	descriptor.colorPrimaries = "bt2020";
	descriptor.transferFunction = "smpte_st_2084_pq";
	descriptor.ycbcrMatrix = "bt2020_nonconstant_luminance";
	descriptor.displayTransform = "rec709_linear_to_rec2020_pq";
	descriptor.referenceWhiteNits = 100u;
	descriptor.pqPeakNits = 10000u;
	descriptor.dimensionRounding = "round_up_to_even";
	descriptor.maxBFrames = 0u;
	descriptor.conversionBrightness = 0;
	descriptor.conversionContrast = 1 << 16;
	descriptor.conversionSaturation = 1 << 16;
	switch( encoding ) {
	case FireFrameSequenceEncoding::AppleProRes4444_12Bit:
		descriptor.backend = "avfoundation";
		descriptor.containerFormat = "MOV";
		descriptor.codec = "apple_prores_4444";
		descriptor.codecImplementation = "AVVideoCodecTypeAppleProRes4444";
		descriptor.codecProfile = "4444";
		descriptor.bitsPerChannel = 12u;
		descriptor.inputPixelFormat = "kCVPixelFormatType_64RGBAHalf";
		descriptor.outputPixelFormat = "prores_4444_12bit";
		descriptor.chromaSubsampling = "4:4:4";
		descriptor.alphaMode = "encoded";
		descriptor.colorRange = "avfoundation_codec_owned";
		descriptor.gopFrames = 1u;
		descriptor.rateControl = "constant_quality_intra";
		descriptor.encoderPreset = "not_configurable_by_avfoundation";
		descriptor.codecOptions = "no_compression_properties";
		descriptor.codecTag = "ap4h";
		descriptor.muxerFlags = "none";
		descriptor.conversionFilter = "avfoundation_managed";
		descriptor.conversionMatrix = "rec709_to_rec2020_d65";
		descriptor.conversionSourceRange = "full";
		descriptor.conversionDestinationRange = "avfoundation_codec_owned";
		descriptor.expectsMediaDataInRealTime = false;
		break;
	case FireFrameSequenceEncoding::AppleProRes4444_10Bit:
		descriptor.backend = "ffmpeg_libavcodec_libavformat_libswscale";
		descriptor.containerFormat = "MOV";
		descriptor.codec = "apple_prores_4444";
		descriptor.codecImplementation = "prores_ks";
		descriptor.codecProfile = "4444";
		descriptor.bitsPerChannel = 10u;
		descriptor.inputPixelFormat = "rgba64le";
		descriptor.outputPixelFormat = "yuva444p10le";
		descriptor.chromaSubsampling = "4:4:4";
		descriptor.alphaMode = "encoded";
		descriptor.colorRange = "full";
		descriptor.gopFrames = 1u;
		descriptor.rateControl = "qscale_global_quality";
		descriptor.encoderPreset = "none";
		descriptor.codecOptions = "profile=4444;global_quality=FF_QP2LAMBDA*5";
		descriptor.codecTag = "ap4h";
		descriptor.muxerFlags = "none";
		descriptor.conversionFilter = "sws_bilinear";
		descriptor.conversionMatrix = "sws_cs_bt2020";
		descriptor.conversionSourceRange = "full";
		descriptor.conversionDestinationRange = "full";
		descriptor.expectsMediaDataInRealTime = false;
		break;
	case FireFrameSequenceEncoding::HevcMain10_10Bit:
		descriptor.backend = "ffmpeg_libavcodec_libavformat_libswscale";
		descriptor.containerFormat = "MP4";
		descriptor.codec = "hevc_main10";
		descriptor.codecImplementation = "libx265";
		descriptor.codecProfile = "main10";
		descriptor.bitsPerChannel = 10u;
		descriptor.inputPixelFormat = "rgba64le";
		descriptor.outputPixelFormat = "yuv420p10le";
		descriptor.chromaSubsampling = "4:2:0";
		descriptor.alphaMode = "dropped";
		descriptor.colorRange = "limited";
		descriptor.gopFrames = 2u*framesPerSecond;
		descriptor.rateControl = "crf_20";
		descriptor.encoderPreset = "medium";
		descriptor.codecOptions = "crf=20:hdr10-opt=1:master-display=G(8500,39850)B(6550,2300)R(35400,14600)WP(15635,16450)L(100000000,1):max-cll=0,0";
		descriptor.codecTag = "hvc1";
		descriptor.muxerFlags = "+faststart";
		descriptor.conversionFilter = "sws_bilinear";
		descriptor.conversionMatrix = "sws_cs_bt2020";
		descriptor.conversionSourceRange = "full";
		descriptor.conversionDestinationRange = "limited";
		descriptor.expectsMediaDataInRealTime = false;
		break;
	default:
		error = "movie output encoding is unavailable";
		return false;
	}
	return true;
}

bool RISE::Implementation::ValidateFireFrameSequenceEncodingDescriptor(
	const FireFrameSequenceEncoding encoding,
	const unsigned int framesPerSecond,
	const FireFrameSequenceEncodingDescriptor& descriptor,
	std::string& error )
{
	FireFrameSequenceEncodingDescriptor expected;
	if( !DescribeFireFrameSequenceEncoding(encoding,framesPerSecond,expected,error) ) {
		return false;
	}
	const bool exact = descriptor.schemaVersion == expected.schemaVersion &&
		descriptor.backend == expected.backend &&
		descriptor.containerFormat == expected.containerFormat &&
		descriptor.codec == expected.codec &&
		descriptor.codecImplementation == expected.codecImplementation &&
		descriptor.codecProfile == expected.codecProfile &&
		descriptor.bitsPerChannel == expected.bitsPerChannel &&
		descriptor.inputPixelFormat == expected.inputPixelFormat &&
		descriptor.outputPixelFormat == expected.outputPixelFormat &&
		descriptor.chromaSubsampling == expected.chromaSubsampling &&
		descriptor.alphaMode == expected.alphaMode &&
		descriptor.colorRange == expected.colorRange &&
		descriptor.colorPrimaries == expected.colorPrimaries &&
		descriptor.transferFunction == expected.transferFunction &&
		descriptor.ycbcrMatrix == expected.ycbcrMatrix &&
		descriptor.displayTransform == expected.displayTransform &&
		descriptor.referenceWhiteNits == expected.referenceWhiteNits &&
		descriptor.pqPeakNits == expected.pqPeakNits &&
		descriptor.dimensionRounding == expected.dimensionRounding &&
		descriptor.maxBFrames == expected.maxBFrames &&
		descriptor.gopFrames == expected.gopFrames &&
		descriptor.rateControl == expected.rateControl &&
		descriptor.encoderPreset == expected.encoderPreset &&
		descriptor.codecOptions == expected.codecOptions &&
		descriptor.codecTag == expected.codecTag &&
		descriptor.muxerFlags == expected.muxerFlags &&
		descriptor.conversionFilter == expected.conversionFilter &&
		descriptor.conversionMatrix == expected.conversionMatrix &&
		descriptor.conversionSourceRange == expected.conversionSourceRange &&
		descriptor.conversionDestinationRange == expected.conversionDestinationRange &&
		descriptor.conversionBrightness == expected.conversionBrightness &&
		descriptor.conversionContrast == expected.conversionContrast &&
		descriptor.conversionSaturation == expected.conversionSaturation &&
		descriptor.expectsMediaDataInRealTime == expected.expectsMediaDataInRealTime;
	if( !exact ) error = "movie encoding descriptor differs from its authored schema-v1 values";
	return exact;
}

namespace
{
	const char kFireAttributePrefix[] = "riseFireProv_";
	std::mutex gFileTransactionMutex;
	std::atomic<FileTransactionContentionHook> gFileTransactionContentionHook{nullptr};
	std::atomic<void*> gFileTransactionContentionContext{nullptr};

	std::unique_lock<std::mutex> AcquireFileTransactionLock()
	{
		std::unique_lock<std::mutex> lock(gFileTransactionMutex,std::defer_lock);
		if( !lock.try_lock() ) {
			const FileTransactionContentionHook hook =
				gFileTransactionContentionHook.load(std::memory_order_acquire);
			if( hook ) hook(gFileTransactionContentionContext.load(
				std::memory_order_acquire));
			lock.lock();
		}
		return lock;
	}

	RISECBOR64::Value TextArray( const std::vector<std::string>& values )
	{
		RISECBOR64::Value::Values encoded;
		for( std::size_t i=0; i<values.size(); ++i ) {
			encoded.push_back(RISECBOR64::Value::String(values[i]));
		}
		return RISECBOR64::Value::ArrayValue(encoded);
	}

	RISECBOR64::Value ActiveFireMediaArray(
		const std::vector<FrameStoreOutput::ActiveFireMedium>& media )
	{
		using RISECBOR64::Value;
		Value::Values encoded;
		encoded.reserve(media.size());
		for( std::size_t i=0; i<media.size(); ++i ) {
			encoded.push_back(Value::MapValue({
				{ "authored_config_digest", Value::String(media[i].authoredConfigDigest) },
				{ "binding_kind", Value::String(media[i].bindingKind) },
				{ "binding_owner", Value::String(media[i].bindingOwner) },
				{ "manager_name", Value::String(media[i].managerName) },
				{ "media_kind", Value::String(media[i].mediaKind) },
				{ "optical_record_ids", TextArray(media[i].opticalRecordIds) }
			}));
		}
		return Value::ArrayValue(encoded);
	}

	bool ReadArtifact( const char* filename, RISECBOR64::Bytes& bytes )
	{
		std::ifstream input(filename,std::ios::binary);
		if( !input ) return false;
		input.seekg(0,std::ios::end);
		const std::streampos size = input.tellg();
		if( size < 0 ) return false;
		input.seekg(0,std::ios::beg);
		bytes.resize(static_cast<std::size_t>(size));
		if( size > 0 ) {
			input.read(reinterpret_cast<char*>(&bytes[0]),size);
		}
		return input.good() || input.eof();
	}

	bool ArtifactHasBytes( const char* filename )
	{
		std::ifstream input(filename,std::ios::binary);
		if( !input ) return false;
		input.seekg(0,std::ios::end);
		return input.tellg() > 0;
	}

	std::uint32_t ReadLE32( const RISECBOR64::Bytes& bytes, const std::size_t offset )
	{
		return static_cast<std::uint32_t>(bytes[offset]) |
			(static_cast<std::uint32_t>(bytes[offset+1]) << 8u) |
			(static_cast<std::uint32_t>(bytes[offset+2]) << 16u) |
			(static_cast<std::uint32_t>(bytes[offset+3]) << 24u);
	}

	std::uint64_t ReadLE64( const RISECBOR64::Bytes& bytes, const std::size_t offset )
	{
		std::uint64_t value = 0;
		for( unsigned int i=0; i<8u; ++i ) {
			value |= static_cast<std::uint64_t>(bytes[offset+i]) << (8u*i);
		}
		return value;
	}

	void AppendLE64( RISECBOR64::Bytes& bytes, const std::uint64_t value )
	{
		for( unsigned int i=0; i<8u; ++i ) {
			bytes.push_back(static_cast<unsigned char>((value >> (8u*i)) & 0xffu));
		}
	}

	bool ReadCString(
		const RISECBOR64::Bytes& bytes,
		std::size_t& cursor,
		std::string& value )
	{
		const std::size_t begin = cursor;
		while( cursor < bytes.size() && bytes[cursor] != 0u ) ++cursor;
		if( cursor >= bytes.size() ) return false;
		value.assign(reinterpret_cast<const char*>(&bytes[begin]),cursor-begin);
		++cursor;
		return true;
	}

	std::string EscapeJSONString( const std::string& value )
	{
		static const char hex[] = "0123456789abcdef";
		std::string escaped;
		escaped.push_back('"');
		for( std::size_t i=0; i<value.size(); ++i ) {
			const unsigned char c = static_cast<unsigned char>(value[i]);
			switch( c ) {
			case '"': escaped += "\\\""; break;
			case '\\': escaped += "\\\\"; break;
			case '\b': escaped += "\\b"; break;
			case '\f': escaped += "\\f"; break;
			case '\n': escaped += "\\n"; break;
			case '\r': escaped += "\\r"; break;
			case '\t': escaped += "\\t"; break;
			default:
				if( c < 0x20u ) {
					escaped += "\\u00";
					escaped.push_back(hex[c >> 4u]);
					escaped.push_back(hex[c & 0x0fu]);
				} else {
					escaped.push_back(static_cast<char>(c));
				}
				break;
			}
		}
		escaped.push_back('"');
		return escaped;
	}

	bool CanonicalJSON(
		const RISECBOR64::Value& value,
		std::string& json,
		std::string& error )
	{
		using RISECBOR64::Value;
		switch( value.GetType() ) {
		case Value::Null: json += "null"; return true;
		case Value::Boolean: json += value.GetBoolean() ? "true" : "false"; return true;
		case Value::UnsignedInteger:
			json += std::to_string(value.GetIntegerArgument()); return true;
		case Value::NegativeInteger:
			json.push_back('-');
			if( value.GetIntegerArgument() == std::numeric_limits<std::uint64_t>::max() ) {
				json += "18446744073709551616";
			} else {
				json += std::to_string(value.GetIntegerArgument()+1u);
			}
			return true;
		case Value::Float64:
		{
			if( value.GetFloat() == 0.0 ) {
				json.push_back('0');
				return true;
			}
			char buffer[64];
			const std::to_chars_result converted = std::to_chars(buffer,buffer+sizeof(buffer),
				value.GetFloat(),std::chars_format::general,
				std::numeric_limits<double>::max_digits10);
			if( converted.ec != std::errc() ) {
				error = "could not serialize binary64 as canonical JSON";
				return false;
			}
			json.append(buffer,converted.ptr);
			return true;
		}
		case Value::Text: json += EscapeJSONString(value.GetText()); return true;
		case Value::ByteString:
		{
			static const char hex[] = "0123456789abcdef";
			std::string encoded;
			encoded.reserve(value.GetBytes().size()*2u);
			for( std::size_t i=0; i<value.GetBytes().size(); ++i ) {
				encoded.push_back(hex[value.GetBytes()[i] >> 4u]);
				encoded.push_back(hex[value.GetBytes()[i] & 0x0fu]);
			}
			json += EscapeJSONString(encoded);
			return true;
		}
		case Value::Array:
			json.push_back('[');
			for( std::size_t i=0; i<value.GetArray().size(); ++i ) {
				if( i ) json.push_back(',');
				if( !CanonicalJSON(value.GetArray()[i],json,error) ) return false;
			}
			json.push_back(']');
			return true;
		case Value::Map:
		{
			std::vector<const std::pair<std::string,Value>*> sorted;
			for( std::size_t i=0; i<value.GetMap().size(); ++i ) sorted.push_back(&value.GetMap()[i]);
			std::sort(sorted.begin(),sorted.end(),[]( const auto* lhs, const auto* rhs ) {
				return lhs->first < rhs->first;
			});
			json.push_back('{');
			for( std::size_t i=0; i<sorted.size(); ++i ) {
				if( i ) json.push_back(',');
				json += EscapeJSONString(sorted[i]->first);
				json.push_back(':');
				if( !CanonicalJSON(sorted[i]->second,json,error) ) return false;
			}
			json.push_back('}');
			return true;
		}
		}
		error = "unknown canonical JSON value type";
		return false;
	}

	const char* ColorSpaceName( const COLOR_SPACE colorSpace )
	{
		switch( colorSpace ) {
		case eColorSpace_sRGB: return "srgb";
		case eColorSpace_Rec709RGB_Linear: return "rec709_linear";
		case eColorSpace_ROMMRGB_Linear: return "romm_linear";
		case eColorSpace_ProPhotoRGB: return "prophoto_rgb";
		}
		return "unknown";
	}

	const char* EXRCompressionName( const EXR_COMPRESSION compression )
	{
		switch( compression ) {
		case eExrCompression_None: return "none";
		case eExrCompression_Zip: return "zip";
		case eExrCompression_Piz: return "piz";
		case eExrCompression_Dwaa: return "dwaa";
		}
		return "unknown";
	}

	bool IsPrimaryArtifact( const IFrameEncoder& encoder, const EncodeOpts& opts )
	{
		return encoder.FormatName() == "EXR" && opts.bpp >= 32u &&
			!opts.denoisedDerivative &&
			(opts.colorSpace == eColorSpace_Rec709RGB_Linear ||
			 opts.colorSpace == eColorSpace_ROMMRGB_Linear) &&
			(opts.exrCompression == eExrCompression_None ||
			 opts.exrCompression == eExrCompression_Zip ||
			 opts.exrCompression == eExrCompression_Piz) &&
			opts.viewTransform.exposureEV == 0.0f &&
			opts.viewTransform.toneCurve == eDisplayTransform_None &&
			opts.viewTransform.whiteBalance._00 == 1.0 &&
			opts.viewTransform.whiteBalance._01 == 0.0 &&
			opts.viewTransform.whiteBalance._02 == 0.0 &&
			opts.viewTransform.whiteBalance._10 == 0.0 &&
			opts.viewTransform.whiteBalance._11 == 1.0 &&
			opts.viewTransform.whiteBalance._12 == 0.0 &&
			opts.viewTransform.whiteBalance._20 == 0.0 &&
			opts.viewTransform.whiteBalance._21 == 0.0 &&
			opts.viewTransform.whiteBalance._22 == 1.0;
	}

	std::vector<std::string> ArtifactReasons(
		const IFrameEncoder& encoder,
		const EncodeOpts& opts,
		const FrameStore::Metadata& metadata,
		const bool primary )
	{
		if( primary ) return std::vector<std::string>();
		std::set<std::string> reasons;
		const std::string format = encoder.FormatName();
		if( format != "EXR" || opts.bpp < 32u || opts.denoisedDerivative ||
			opts.exrCompression == eExrCompression_Dwaa ) reasons.insert("lossy_output");
		if( !encoder.SupportsHDR() || format == "HDR10_PNG" ) reasons.insert("integer_output");
		if( opts.viewTransform.exposureEV != 0.0f ||
			(!encoder.SupportsHDR() && metadata.cameraExposureEV != 0.0) ||
			(opts.viewTransform.toneCurve != eDisplayTransform_None &&
			 opts.viewTransform.toneCurveStrength > 0.0f) ||
			opts.colorSpace == eColorSpace_sRGB ||
			opts.colorSpace == eColorSpace_ProPhotoRGB ) {
			reasons.insert("display_transform_enabled");
		}
		if( opts.viewTransform.whiteBalance._00 != 1.0 ||
			opts.viewTransform.whiteBalance._01 != 0.0 ||
			opts.viewTransform.whiteBalance._02 != 0.0 ||
			opts.viewTransform.whiteBalance._10 != 0.0 ||
			opts.viewTransform.whiteBalance._11 != 1.0 ||
			opts.viewTransform.whiteBalance._12 != 0.0 ||
			opts.viewTransform.whiteBalance._20 != 0.0 ||
			opts.viewTransform.whiteBalance._21 != 0.0 ||
			opts.viewTransform.whiteBalance._22 != 1.0 ) {
			reasons.insert("white_balance_enabled");
		}
		return std::vector<std::string>(reasons.begin(),reasons.end());
	}

	bool ResolveEffectiveEncoderOptions(
		const IFrameEncoder& encoder,
		const EncodeOpts& requested,
		EncodeOpts& effective,
		std::string& error )
	{
		effective = requested;
		const std::string format = encoder.FormatName();
		switch( requested.colorSpace ) {
		case eColorSpace_sRGB:
		case eColorSpace_Rec709RGB_Linear:
		case eColorSpace_ROMMRGB_Linear:
		case eColorSpace_ProPhotoRGB:
			break;
		default:
			error = "authored color space is outside the fixed enum";
			return false;
		}
		switch( requested.viewTransform.toneCurve ) {
		case eDisplayTransform_None:
		case eDisplayTransform_Reinhard:
		case eDisplayTransform_ACES:
		case eDisplayTransform_AgX:
		case eDisplayTransform_Hable:
			break;
		default:
			error = "authored tone curve is outside the fixed enum";
			return false;
		}
		const FrameStoreOutput::ViewTransform& view = requested.viewTransform;
		if( !std::isfinite(view.exposureEV) ) {
			error = "authored exposure must be finite";
			return false;
		}
		if( !std::isfinite(view.toneCurveStrength) ||
			view.toneCurveStrength < 0.0f || view.toneCurveStrength > 1.0f ) {
			error = "authored tone curve strength must be finite and within [0, 1]";
			return false;
		}
		const double balance[] = {
			view.whiteBalance._00,view.whiteBalance._01,view.whiteBalance._02,
			view.whiteBalance._10,view.whiteBalance._11,view.whiteBalance._12,
			view.whiteBalance._20,view.whiteBalance._21,view.whiteBalance._22
		};
		for( const double component : balance ) {
			if( !std::isfinite(component) ) {
				error = "authored white balance must contain only finite values";
				return false;
			}
		}
		if( requested.includeAOVs || !requested.aovChannels.empty() ) {
			if( !encoder.SupportsAOVs() ) {
				error = "authored encoder does not support AOV channels";
				return false;
			}
		}
		if( requested.compressionLevel != -1 ) {
			error = "authored encoder does not expose compression_level";
			return false;
		}
		if( format != "EXR" && !requested.attrs.empty() ) {
			error = "authored encoder does not support custom attributes";
			return false;
		}
		const bool identityView = view.exposureEV == 0.0f &&
			view.toneCurve == eDisplayTransform_None && view.toneCurveStrength == 1.0f &&
			view.whiteBalance._00 == 1.0 && view.whiteBalance._01 == 0.0 &&
			view.whiteBalance._02 == 0.0 && view.whiteBalance._10 == 0.0 &&
			view.whiteBalance._11 == 1.0 && view.whiteBalance._12 == 0.0 &&
			view.whiteBalance._20 == 0.0 && view.whiteBalance._21 == 0.0 &&
			view.whiteBalance._22 == 1.0;
		if( (format == "EXR" || format == "HDR" || format == "RGBEA") &&
			!identityView ) {
			error = "authored HDR encoder does not apply a display transform";
			return false;
		}
		if( format == "PNG" ) {
			if( requested.bpp != 8u && requested.bpp != 16u ) {
				error = "PNG bits_per_channel must be 8 or 16";
				return false;
			}
		} else if( format == "EXR" ) {
			effective.bpp = requested.bpp >= 32u ? 32u : 16u;
			effective.viewTransform = FrameStoreOutput::ViewTransform();
			if( !std::isfinite(requested.exrPixelAspectRatio) ||
				requested.exrPixelAspectRatio <= 0.0f ) {
				error = "EXR pixel aspect ratio must be positive and finite";
				return false;
			}
			if( requested.exrCompression < eExrCompression_None ||
				requested.exrCompression > eExrCompression_Dwaa ) {
				error = "EXR compression is outside the authored enum";
				return false;
			}
		} else if( format == "RGBEA" ) {
			effective.bpp = 16u;
			effective.colorSpace = eColorSpace_ROMMRGB_Linear;
			effective.viewTransform = FrameStoreOutput::ViewTransform();
		} else if( format == "HDR10_PNG" ) {
			if( requested.viewTransform.toneCurve != eDisplayTransform_None ) {
				error = "HDR10_PNG does not apply an authored tone curve";
				return false;
			}
			effective.bpp = 16u;
			effective.exrWithAlpha = false;
		} else if( format == "TIFF" || format == "HDR" ||
			format == "TGA" || format == "PPM" ) {
			effective.bpp = 8u;
			if( format == "HDR" ) {
				effective.viewTransform = FrameStoreOutput::ViewTransform();
			}
		} else {
			error = "authored encoder format is outside the resolved output schema";
			return false;
		}
		return true;
	}

	bool BuildFireProvenance(
		const FrameStore::Metadata& metadata,
		const IFrameEncoder& encoder,
		const EncodeOpts& opts,
		const std::string& artifactSha256,
		const std::string& artifactFidelity,
		const std::vector<std::string>& artifactReasons,
		RISECBOR64::Bytes& sidecar,
		std::vector<std::pair<std::string,std::string> >& attributes,
		std::string& provenanceId,
		std::string& error )
	{
		using RISECBOR64::Value;
		if( !FrameStoreOutput::ValidateFireOutputMetadata(metadata,error) ) return false;
		Value resolvedConfig;
		Value rendererBuild;
		if( metadata.resolvedRenderConfigCoreV1.empty() ||
			!RISECBOR64::DecodeCanonical(metadata.resolvedRenderConfigCoreV1,
				resolvedConfig,&error) || resolvedConfig.GetType() != Value::Map ) {
			error = "resolved render configuration is unavailable or noncanonical: "+error;
			return false;
		}
		const Value* resolvedCamera = resolvedConfig.Find("camera");
		const Value* resolvedCameraEV = resolvedCamera ?
			resolvedCamera->Find("exposure_compensation_ev") : nullptr;
		if( !resolvedCameraEV || resolvedCameraEV->GetType() != Value::Float64 ||
			resolvedCameraEV->GetFloat() != metadata.cameraExposureEV ) {
			error = "resolved camera exposure does not match the encoded frame snapshot";
			return false;
		}
		if( metadata.rendererBuildV1.empty() ||
			!RISECBOR64::DecodeCanonical(metadata.rendererBuildV1,rendererBuild,&error) ||
			rendererBuild.GetType() != Value::Map || metadata.rendererBuildId.empty() ||
			metadata.rendererBuildId != RISECBOR64::SHA256Hex(metadata.rendererBuildV1) ) {
			error = "renderer build identity is unavailable or noncanonical";
			return false;
		}
		Value::Members resolvedMembers = resolvedConfig.GetMap();
		if( resolvedConfig.Find("output") ) {
			error = "resolved render configuration core already contains output settings";
			return false;
		}
		Value::Values aovChannels;
		for( std::size_t i=0; i<opts.aovChannels.size(); ++i ) {
			aovChannels.push_back(Value::Unsigned(
				static_cast<unsigned int>(opts.aovChannels[i])));
		}
		Value::Values authoredAttributes;
		for( std::size_t i=0; i<opts.attrs.size(); ++i ) {
			authoredAttributes.push_back(Value::MapValue({
				{ "name", Value::String(opts.attrs[i].first) },
				{ "value", Value::String(opts.attrs[i].second) }
			}));
		}
		const std::string outputFormat = encoder.FormatName();
		resolvedMembers.push_back(std::make_pair("output",Value::MapValue({
			{ "aov_channels", Value::ArrayValue(aovChannels) },
			{ "attributes", Value::ArrayValue(authoredAttributes) },
			{ "bits_per_channel", Value::Unsigned(opts.bpp) },
			{ "color_space", Value::String(outputFormat == "HDR10_PNG" ?
				"bt2020_pq" : ColorSpaceName(opts.colorSpace)) },
			{ "compression_level", Value::Signed(opts.compressionLevel) },
			{ "denoised_derivative", Value::Bool(opts.denoisedDerivative) },
			{ "exr_channel_p_linear", Value::Bool(false) },
			{ "exr_channel_x_sampling", Value::Unsigned(1u) },
			{ "exr_channel_y_sampling", Value::Unsigned(1u) },
			{ "exr_compression", Value::String(outputFormat == "EXR" ?
				EXRCompressionName(opts.exrCompression) : "not_applicable") },
			{ "exr_line_order", Value::String("increasing_y") },
			{ "exr_pixel_aspect_ratio", Value::Float(opts.exrPixelAspectRatio) },
			{ "exr_screen_window_center", Value::ArrayValue({
				Value::Float(0.0), Value::Float(0.0) }) },
			{ "exr_screen_window_width", Value::Float(1.0) },
			{ "exr_software", Value::String(EXRWriter::SoftwareAttribute()) },
			{ "exr_white_luminance", Value::Float(1.0) },
			{ "exr_with_alpha", Value::Bool(outputFormat == "EXR" && opts.exrWithAlpha) },
			{ "format", Value::String(encoder.FormatName()) },
			{ "frame_index", Value::Unsigned(opts.frame) },
			{ "include_aovs", Value::Bool(opts.includeAOVs) },
			{ "view_exposure_ev", Value::Float(opts.viewTransform.exposureEV) },
			{ "view_tone_curve", Value::Unsigned(static_cast<unsigned int>(opts.viewTransform.toneCurve)) },
			{ "view_tone_curve_strength", Value::Float(opts.viewTransform.toneCurveStrength) },
			{ "view_white_balance", Value::ArrayValue({
				Value::Float(opts.viewTransform.whiteBalance._00),
				Value::Float(opts.viewTransform.whiteBalance._01),
				Value::Float(opts.viewTransform.whiteBalance._02),
				Value::Float(opts.viewTransform.whiteBalance._10),
				Value::Float(opts.viewTransform.whiteBalance._11),
				Value::Float(opts.viewTransform.whiteBalance._12),
				Value::Float(opts.viewTransform.whiteBalance._20),
				Value::Float(opts.viewTransform.whiteBalance._21),
				Value::Float(opts.viewTransform.whiteBalance._22)
			}) }
		})));
		const Value finalizedConfig = Value::MapValue(resolvedMembers);
		RISECBOR64::Bytes finalizedConfigBytes;
		if( !RISECBOR64::Encode(finalizedConfig,finalizedConfigBytes,&error) ) return false;

		Value::Members payloadMembers = {
			{ "active_fire_media", ActiveFireMediaArray(metadata.activeFireMedia) },
			{ "active_fire_optics_record_ids", TextArray(metadata.activeFireOpticsRecordIds) },
			{ "artifact_fidelity", Value::String(artifactFidelity) },
			{ "artifact_reason_codes", TextArray(artifactReasons) },
			{ "artifact_sha256", Value::String(artifactSha256) },
			{ "record_kind", Value::String("fire_output_provenance") },
			{ "render_fidelity_status", Value::String(metadata.renderFidelityStatus) },
			{ "render_reason_codes", TextArray(metadata.renderReasonCodes) },
			{ "renderer_build_id", Value::String(metadata.rendererBuildId) },
			{ "renderer_build_v1", rendererBuild },
			{ "resolved_render_configuration_id", Value::String(RISECBOR64::SHA256Hex(finalizedConfigBytes)) },
			{ "resolved_render_configuration_v1", finalizedConfig },
			{ "schema_version", Value::Unsigned(1) }
		};
		if( artifactFidelity == "display_derivative" ) {
			if( metadata.primaryProvenanceId.empty() ||
				metadata.primaryArtifactSha256.empty() ||
				(metadata.primaryArtifactFidelity != "preview_primary" &&
				 metadata.primaryArtifactFidelity != "predictive_primary") ) {
				error = "display derivative has no finalized fire primary linkage";
				return false;
			}
			payloadMembers.push_back(std::make_pair("derivation_kind",
				Value::String("single_primary")));
			payloadMembers.push_back(std::make_pair("derived_from_frames",
				Value::ArrayValue({})));
			payloadMembers.push_back(std::make_pair("derived_from_primary",
				Value::MapValue({
					{ "artifact_sha256", Value::String(metadata.primaryArtifactSha256) },
					{ "provenance_id", Value::String(metadata.primaryProvenanceId) }
				})));
		}
		const Value payload = Value::MapValue(payloadMembers);
		RISECBOR64::Bytes payloadBytes;
		if( !RISECBOR64::Encode(payload,payloadBytes,&error) ) return false;
		provenanceId = RISECBOR64::SHA256Hex(payloadBytes);
		const Value envelope = Value::MapValue({
			{ "payload", payload },
			{ "provenance_id", Value::String(provenanceId) }
		});
		if( !RISECBOR64::Encode(envelope,sidecar,&error) ) return false;

		attributes.clear();
		for( std::size_t i=0; i<payloadMembers.size(); ++i ) {
			std::string json;
			if( !CanonicalJSON(payloadMembers[i].second,json,error) ) return false;
			attributes.push_back(std::make_pair(
				std::string(kFireAttributePrefix)+payloadMembers[i].first,json));
		}
		attributes.push_back(std::make_pair(
			std::string(kFireAttributePrefix)+"provenance_id",
			EscapeJSONString(provenanceId)));
		return true;
	}

	bool IsSHA256Hex( const std::string& value )
	{
		if( value.size() != 64u ) return false;
		for( std::size_t i=0; i<value.size(); ++i ) {
			if( !((value[i] >= '0' && value[i] <= '9') ||
				(value[i] >= 'a' && value[i] <= 'f')) ) return false;
		}
		return true;
	}

	bool ExactKeys( const RISECBOR64::Value& value,
		const std::initializer_list<const char*> keys )
	{
		if( value.GetType() != RISECBOR64::Value::Map ||
			value.GetMap().size() != keys.size() ) return false;
		for( const char* key : keys ) if( !value.Find(key) ) return false;
		return true;
	}

	bool ParseTextArray( const RISECBOR64::Value* value,
		std::vector<std::string>& parsed )
	{
		parsed.clear();
		if( !value || value->GetType() != RISECBOR64::Value::Array ) return false;
		for( const RISECBOR64::Value& item : value->GetArray() ) {
			if( item.GetType() != RISECBOR64::Value::Text ) return false;
			parsed.push_back(item.GetText());
		}
		return true;
	}

	bool ValidateFireProvenancePayload(
		const RISECBOR64::Value& payload,
		std::string& error )
	{
		using RISECBOR64::Value;
		const Value* fidelity = payload.Find("artifact_fidelity");
		if( !fidelity || fidelity->GetType() != Value::Text ) {
			error = "fire provenance artifact_fidelity is unavailable";
			return false;
		}
		const bool derivative = fidelity->GetText() == "display_derivative";
		if( fidelity->GetText() != "preview_primary" &&
			fidelity->GetText() != "predictive_primary" && !derivative ) {
			error = "fire provenance artifact_fidelity is outside schema-v1";
			return false;
		}
		const bool exactPayload = derivative ?
			ExactKeys(payload,{ "active_fire_media", "active_fire_optics_record_ids",
				"artifact_fidelity", "artifact_reason_codes", "artifact_sha256",
				"derivation_kind", "derived_from_frames", "derived_from_primary",
				"record_kind", "render_fidelity_status", "render_reason_codes",
				"renderer_build_id", "renderer_build_v1",
				"resolved_render_configuration_id",
				"resolved_render_configuration_v1", "schema_version" }) :
			ExactKeys(payload,{ "active_fire_media", "active_fire_optics_record_ids",
				"artifact_fidelity", "artifact_reason_codes", "artifact_sha256",
				"record_kind", "render_fidelity_status", "render_reason_codes",
				"renderer_build_id", "renderer_build_v1",
				"resolved_render_configuration_id",
				"resolved_render_configuration_v1", "schema_version" });
		const Value* schema = payload.Find("schema_version");
		const Value* recordKind = payload.Find("record_kind");
		const Value* artifactDigest = payload.Find("artifact_sha256");
		const Value* renderStatus = payload.Find("render_fidelity_status");
		if( !exactPayload || !schema || schema->GetType() != Value::UnsignedInteger ||
			schema->GetIntegerArgument() != 1u || !recordKind ||
			recordKind->GetType() != Value::Text ||
			recordKind->GetText() != "fire_output_provenance" || !artifactDigest ||
			artifactDigest->GetType() != Value::Text ||
			!IsSHA256Hex(artifactDigest->GetText()) || !renderStatus ||
			renderStatus->GetType() != Value::Text ) {
			error = "fire provenance payload header is outside schema-v1";
			return false;
		}

		std::vector<std::string> artifactReasons;
		if( !ParseTextArray(payload.Find("artifact_reason_codes"),artifactReasons) ) {
			error = "fire provenance artifact reason codes are not text";
			return false;
		}
		const std::set<std::string> allowedArtifactReasons = {
			"display_transform_enabled", "integer_output", "lossy_output",
			"white_balance_enabled" };
		if( !std::is_sorted(artifactReasons.begin(),artifactReasons.end()) ||
			std::adjacent_find(artifactReasons.begin(),artifactReasons.end()) !=
				artifactReasons.end() ) {
			error = "fire provenance artifact reason codes are duplicated or unsorted";
			return false;
		}
		for( const std::string& reason : artifactReasons ) {
			if( !allowedArtifactReasons.count(reason) ) {
				error = "fire provenance artifact reason code is outside schema-v1";
				return false;
			}
		}
		if( !derivative && !artifactReasons.empty() ) {
			error = "fire provenance primary artifact reason codes are not empty";
			return false;
		}

		FrameStoreOutput::Metadata metadata;
		metadata.renderFidelityStatus = renderStatus->GetText();
		if( !ParseTextArray(payload.Find("render_reason_codes"),
			metadata.renderReasonCodes) ||
			!ParseTextArray(payload.Find("active_fire_optics_record_ids"),
				metadata.activeFireOpticsRecordIds) ) {
			error = "fire provenance render reasons or optical IDs are not text arrays";
			return false;
		}
		const Value* media = payload.Find("active_fire_media");
		if( !media || media->GetType() != Value::Array ) {
			error = "fire provenance active media are not an array";
			return false;
		}
		for( const Value& encodedMedium : media->GetArray() ) {
			if( !ExactKeys(encodedMedium,{ "authored_config_digest", "binding_kind",
				"binding_owner", "manager_name", "media_kind", "optical_record_ids" }) ) {
				error = "fire provenance active medium is outside schema-v1";
				return false;
			}
			for( const char* key : { "authored_config_digest", "binding_kind",
				"binding_owner", "manager_name", "media_kind" } ) {
				if( encodedMedium.Find(key)->GetType() != Value::Text ) {
					error = "fire provenance active medium text field has the wrong type";
					return false;
				}
			}
			FrameStoreOutput::ActiveFireMedium decoded;
			decoded.authoredConfigDigest = encodedMedium.Find("authored_config_digest")->GetText();
			decoded.bindingKind = encodedMedium.Find("binding_kind")->GetText();
			decoded.bindingOwner = encodedMedium.Find("binding_owner")->GetText();
			decoded.managerName = encodedMedium.Find("manager_name")->GetText();
			decoded.mediaKind = encodedMedium.Find("media_kind")->GetText();
			if( !ParseTextArray(encodedMedium.Find("optical_record_ids"),
				decoded.opticalRecordIds) ) {
				error = "fire provenance active medium optical IDs are not text";
				return false;
			}
			metadata.activeFireMedia.push_back(decoded);
		}

		const Value* build = payload.Find("renderer_build_v1");
		const Value* buildId = payload.Find("renderer_build_id");
		if( !build || build->GetType() != Value::Map || !buildId ||
			buildId->GetType() != Value::Text ||
			!RISECBOR64::Encode(*build,metadata.rendererBuildV1,&error) ||
			buildId->GetText() != RISECBOR64::SHA256Hex(metadata.rendererBuildV1) ) {
			error = "fire provenance renderer_build_id does not bind renderer_build_v1";
			return false;
		}
		metadata.rendererBuildId = buildId->GetText();

		const Value* config = payload.Find("resolved_render_configuration_v1");
		const Value* configId = payload.Find("resolved_render_configuration_id");
		const Value* output = config && config->GetType() == Value::Map ?
			config->Find("output") : nullptr;
		if( !config || config->GetType() != Value::Map || !configId ||
			configId->GetType() != Value::Text || !output ||
			!ExactKeys(*output,{ "aov_channels", "attributes", "bits_per_channel",
				"color_space", "compression_level", "denoised_derivative",
				"exr_channel_p_linear", "exr_channel_x_sampling",
				"exr_channel_y_sampling", "exr_compression", "exr_line_order",
				"exr_pixel_aspect_ratio", "exr_screen_window_center",
				"exr_screen_window_width", "exr_software", "exr_white_luminance",
				"exr_with_alpha", "format", "frame_index",
				"include_aovs", "view_exposure_ev", "view_tone_curve",
				"view_tone_curve_strength", "view_white_balance" }) ) {
			error = "fire provenance resolved output configuration is outside schema-v1";
			return false;
		}
		RISECBOR64::Bytes finalizedConfigBytes;
		if( !RISECBOR64::Encode(*config,finalizedConfigBytes,&error) ||
			configId->GetText() != RISECBOR64::SHA256Hex(finalizedConfigBytes) ) {
			error = "fire provenance resolved configuration ID does not bind its bytes";
			return false;
		}
		const Value* format = output->Find("format");
		const Value* bits = output->Find("bits_per_channel");
		const Value* aovChannels = output->Find("aov_channels");
		const Value* attributes = output->Find("attributes");
		const Value* pLinear = output->Find("exr_channel_p_linear");
		const Value* xSampling = output->Find("exr_channel_x_sampling");
		const Value* ySampling = output->Find("exr_channel_y_sampling");
		const Value* lineOrder = output->Find("exr_line_order");
		const Value* pixelAspectRatio = output->Find("exr_pixel_aspect_ratio");
		const Value* screenCenter = output->Find("exr_screen_window_center");
		const Value* screenWidth = output->Find("exr_screen_window_width");
		const Value* software = output->Find("exr_software");
		const Value* whiteLuminance = output->Find("exr_white_luminance");
		const Value* whiteBalance = output->Find("view_white_balance");
		if( !format || format->GetType() != Value::Text || format->GetText() != "EXR" ||
			!bits || bits->GetType() != Value::UnsignedInteger ||
			(bits->GetIntegerArgument() != 16u && bits->GetIntegerArgument() != 32u) ||
			!aovChannels || aovChannels->GetType() != Value::Array || !attributes ||
			attributes->GetType() != Value::Array || !whiteBalance ||
			whiteBalance->GetType() != Value::Array ||
			whiteBalance->GetArray().size() != 9u || !pixelAspectRatio ||
			pixelAspectRatio->GetType() != Value::Float64 ||
			!std::isfinite(pixelAspectRatio->GetFloat()) ||
			pixelAspectRatio->GetFloat() <= 0.0 ) {
			error = "fire provenance EXR output surface is outside schema-v1";
			return false;
		}
		if( !pLinear || pLinear->GetType() != Value::Boolean || pLinear->GetBoolean() ||
			!xSampling || xSampling->GetType() != Value::UnsignedInteger ||
			xSampling->GetIntegerArgument() != 1u || !ySampling ||
			ySampling->GetType() != Value::UnsignedInteger ||
			ySampling->GetIntegerArgument() != 1u || !lineOrder ||
			lineOrder->GetType() != Value::Text ||
			lineOrder->GetText() != "increasing_y" || !screenCenter ||
			screenCenter->GetType() != Value::Array ||
			screenCenter->GetArray().size() != 2u ||
			screenCenter->GetArray()[0].GetType() != Value::Float64 ||
			screenCenter->GetArray()[1].GetType() != Value::Float64 ||
			screenCenter->GetArray()[0].GetFloat() != 0.0 ||
			screenCenter->GetArray()[1].GetFloat() != 0.0 || !screenWidth ||
			screenWidth->GetType() != Value::Float64 ||
			screenWidth->GetFloat() != 1.0 || !software ||
			software->GetType() != Value::Text ||
			software->GetText() != EXRWriter::SoftwareAttribute() || !whiteLuminance ||
			whiteLuminance->GetType() != Value::Float64 ||
			whiteLuminance->GetFloat() != 1.0 ) {
			error = "fire provenance EXR interpretation profile is outside schema-v1";
			return false;
		}
		for( const Value& channel : aovChannels->GetArray() ) {
			if( channel.GetType() != Value::UnsignedInteger ||
				channel.GetIntegerArgument() >=
					static_cast<std::uint64_t>(FrameStoreOutput::ChannelId::COUNT) ) {
				error = "fire provenance output AOV channel is outside schema-v1";
				return false;
			}
		}
		for( const Value& attribute : attributes->GetArray() ) {
			if( !ExactKeys(attribute,{"name","value"}) ||
				attribute.Find("name")->GetType() != Value::Text ||
				attribute.Find("value")->GetType() != Value::Text ) {
				error = "fire provenance output attribute is outside schema-v1";
				return false;
			}
		}
		for( const Value& component : whiteBalance->GetArray() ) {
			if( component.GetType() != Value::Float64 ) {
				error = "fire provenance output white balance has the wrong type";
				return false;
			}
		}
		for( const char* key : { "denoised_derivative", "exr_with_alpha", "include_aovs" } ) {
			if( output->Find(key)->GetType() != Value::Boolean ) {
				error = "fire provenance output boolean has the wrong type";
				return false;
			}
		}
		for( const char* key : { "color_space", "exr_compression" } ) {
			if( output->Find(key)->GetType() != Value::Text ) {
				error = "fire provenance output enum has the wrong type";
				return false;
			}
		}
		const std::string& colorSpace = output->Find("color_space")->GetText();
		const std::string& exrCompression = output->Find("exr_compression")->GetText();
		if( colorSpace != "srgb" && colorSpace != "rec709_linear" &&
			colorSpace != "romm_linear" && colorSpace != "prophoto_rgb" ) {
			error = "fire provenance output color space is outside schema-v1";
			return false;
		}
		if( exrCompression != "none" && exrCompression != "zip" &&
			exrCompression != "piz" && exrCompression != "dwaa" ) {
			error = "fire provenance EXR compression is outside schema-v1";
			return false;
		}
		for( const char* key : { "view_exposure_ev", "view_tone_curve_strength" } ) {
			if( output->Find(key)->GetType() != Value::Float64 ) {
				error = "fire provenance output scalar has the wrong type";
				return false;
			}
		}
		const double toneCurveStrength =
			output->Find("view_tone_curve_strength")->GetFloat();
		if( toneCurveStrength < 0.0 || toneCurveStrength > 1.0 ) {
			error = "fire provenance output tone curve strength is outside [0, 1]";
			return false;
		}
		for( const char* key : { "frame_index", "view_tone_curve" } ) {
			if( output->Find(key)->GetType() != Value::UnsignedInteger ) {
				error = "fire provenance output integer has the wrong type";
				return false;
			}
		}
		if( output->Find("view_tone_curve")->GetIntegerArgument() >
			static_cast<std::uint64_t>(eDisplayTransform_Hable) ) {
			error = "fire provenance output tone curve is outside schema-v1";
			return false;
		}
		if( output->Find("compression_level")->GetType() != Value::NegativeInteger ||
			output->Find("compression_level")->GetIntegerArgument() != 0u ) {
			error = "fire provenance output compression level is outside schema-v1";
			return false;
		}
		const Value::Values& balance = whiteBalance->GetArray();
		const bool identityBalance = balance[0].GetFloat() == 1.0 &&
			balance[1].GetFloat() == 0.0 && balance[2].GetFloat() == 0.0 &&
			balance[3].GetFloat() == 0.0 && balance[4].GetFloat() == 1.0 &&
			balance[5].GetFloat() == 0.0 && balance[6].GetFloat() == 0.0 &&
			balance[7].GetFloat() == 0.0 && balance[8].GetFloat() == 1.0;
		if( !derivative ) {
			const bool losslessCompression = exrCompression == "none" ||
				exrCompression == "zip" || exrCompression == "piz";
			const bool linearColor = colorSpace == "rec709_linear" ||
				colorSpace == "romm_linear";
			if( bits->GetIntegerArgument() != 32u || !linearColor ||
				!losslessCompression || output->Find("denoised_derivative")->GetBoolean() ||
				output->Find("view_exposure_ev")->GetFloat() != 0.0 ||
				output->Find("view_tone_curve")->GetIntegerArgument() !=
					static_cast<std::uint64_t>(eDisplayTransform_None) ||
				output->Find("view_tone_curve_strength")->GetFloat() != 1.0 ||
				!identityBalance ) {
				error = "fire provenance primary output is not raw lossless FP32";
				return false;
			}
		} else {
			std::set<std::string> expectedReasons;
			if( bits->GetIntegerArgument() < 32u || exrCompression == "dwaa" ||
				output->Find("denoised_derivative")->GetBoolean() ) {
				expectedReasons.insert("lossy_output");
			}
			if( output->Find("view_exposure_ev")->GetFloat() != 0.0 ||
				(output->Find("view_tone_curve")->GetIntegerArgument() !=
					static_cast<std::uint64_t>(eDisplayTransform_None) &&
				 output->Find("view_tone_curve_strength")->GetFloat() > 0.0) ||
				colorSpace == "srgb" || colorSpace == "prophoto_rgb" ) {
				expectedReasons.insert("display_transform_enabled");
			}
			if( !identityBalance ) expectedReasons.insert("white_balance_enabled");
			const std::vector<std::string> expected(
				expectedReasons.begin(),expectedReasons.end());
			if( artifactReasons != expected ) {
				error = "fire provenance derivative reason codes do not match the resolved output";
				return false;
			}
		}

		Value::Members coreMembers;
		for( const auto& member : config->GetMap() ) {
			if( member.first != "output" ) coreMembers.push_back(member);
		}
		if( !RISECBOR64::Encode(Value::MapValue(coreMembers),
			metadata.resolvedRenderConfigCoreV1,&error) ) return false;

		if( derivative ) {
			const Value* derivationKind = payload.Find("derivation_kind");
			const Value* frames = payload.Find("derived_from_frames");
			const Value* primary = payload.Find("derived_from_primary");
			if( !derivationKind || derivationKind->GetType() != Value::Text ||
				derivationKind->GetText() != "single_primary" || !frames ||
				frames->GetType() != Value::Array || !frames->GetArray().empty() ||
				!primary || !ExactKeys(*primary,{"artifact_sha256","provenance_id"}) ||
				primary->Find("artifact_sha256")->GetType() != Value::Text ||
				primary->Find("provenance_id")->GetType() != Value::Text ) {
				error = "fire provenance derivative linkage is outside schema-v1";
				return false;
			}
			metadata.primaryArtifactSha256 = primary->Find("artifact_sha256")->GetText();
			metadata.primaryProvenanceId = primary->Find("provenance_id")->GetText();
			metadata.primaryArtifactFidelity = "preview_primary";
		}
		if( !FrameStoreOutput::ValidateFireOutputMetadata(metadata,error) ) {
			error = "fire provenance payload metadata is invalid: "+error;
			return false;
		}
		if( !derivative && fidelity->GetText() != "preview_primary" ) {
			error = "fire provenance predictive primary is unavailable in schema-v1";
			return false;
		}
		return true;
	}

	bool BuildFireFrameSequenceProvenance(
		const FrameStoreOutput::Metadata& metadata,
		const FireFrameSequenceEncoding encoding,
		const std::string& artifactSha256,
		const unsigned int width,
		const unsigned int height,
		const unsigned int framesPerSecond,
		const std::vector<FireFramePrimary>& frames,
		RISECBOR64::Bytes& sidecar,
		std::string& error )
	{
		using RISECBOR64::Value;
		if( !FrameStoreOutput::ValidateFireOutputMetadata(metadata,error) ) return false;
		if( metadata.renderFidelityStatus != "predictive" &&
			metadata.renderFidelityStatus != "preview" ) {
			error = "movie has no predictive or preview fire-render status";
			return false;
		}
		if( frames.empty() || framesPerSecond == 0u || width == 0u || height == 0u ) {
			error = "movie frame-sequence dimensions, rate, or links are empty";
			return false;
		}
		Value::Values links;
		links.reserve(frames.size());
		for( std::size_t i=0; i<frames.size(); ++i ) {
			if( (i && frames[i].frameIndex != frames[i-1u].frameIndex+1u) ||
				!IsSHA256Hex(frames[i].provenanceId) ||
				!IsSHA256Hex(frames[i].artifactSha256) ) {
				error = "movie primary links are not contiguous or complete SHA-256 identities";
				return false;
			}
			links.push_back(Value::MapValue({
				{ "artifact_sha256", Value::String(frames[i].artifactSha256) },
				{ "frame_index", Value::Unsigned(frames[i].frameIndex) },
				{ "provenance_id", Value::String(frames[i].provenanceId) }
			}));
		}

		Value resolvedConfig;
		Value rendererBuild;
		if( metadata.resolvedRenderConfigCoreV1.empty() ||
			!RISECBOR64::DecodeCanonical(metadata.resolvedRenderConfigCoreV1,
				resolvedConfig,&error) || resolvedConfig.GetType() != Value::Map ) {
			error = "resolved render configuration is unavailable or noncanonical: "+error;
			return false;
		}
		if( resolvedConfig.Find("output") ) {
			error = "resolved render configuration core already contains output settings";
			return false;
		}
		if( metadata.rendererBuildV1.empty() ||
			!RISECBOR64::DecodeCanonical(metadata.rendererBuildV1,rendererBuild,&error) ||
			rendererBuild.GetType() != Value::Map ||
			!IsSHA256Hex(metadata.rendererBuildId) ||
			metadata.rendererBuildId != RISECBOR64::SHA256Hex(metadata.rendererBuildV1) ) {
			error = "renderer build identity is unavailable or noncanonical";
			return false;
		}
		FireFrameSequenceEncodingDescriptor descriptor;
		if( !DescribeFireFrameSequenceEncoding(
			encoding,framesPerSecond,descriptor,error) ) return false;
		const Value encodingDescriptor = Value::MapValue({
			{ "alpha_mode", Value::String(descriptor.alphaMode) },
			{ "backend", Value::String(descriptor.backend) },
			{ "bits_per_channel", Value::Unsigned(descriptor.bitsPerChannel) },
			{ "chroma_subsampling", Value::String(descriptor.chromaSubsampling) },
			{ "codec", Value::String(descriptor.codec) },
			{ "codec_implementation", Value::String(descriptor.codecImplementation) },
			{ "codec_options", Value::String(descriptor.codecOptions) },
			{ "codec_profile", Value::String(descriptor.codecProfile) },
			{ "codec_tag", Value::String(descriptor.codecTag) },
			{ "color_primaries", Value::String(descriptor.colorPrimaries) },
			{ "color_range", Value::String(descriptor.colorRange) },
			{ "container_format", Value::String(descriptor.containerFormat) },
			{ "conversion_destination_range", Value::String(
				descriptor.conversionDestinationRange) },
			{ "conversion_brightness", Value::Signed(descriptor.conversionBrightness) },
			{ "conversion_contrast", Value::Signed(descriptor.conversionContrast) },
			{ "conversion_filter", Value::String(descriptor.conversionFilter) },
			{ "conversion_matrix", Value::String(descriptor.conversionMatrix) },
			{ "conversion_saturation", Value::Signed(descriptor.conversionSaturation) },
			{ "conversion_source_range", Value::String(
				descriptor.conversionSourceRange) },
			{ "dimension_rounding", Value::String(descriptor.dimensionRounding) },
			{ "display_transform", Value::String(descriptor.displayTransform) },
			{ "encoder_preset", Value::String(descriptor.encoderPreset) },
			{ "expects_media_data_in_real_time", Value::Bool(
				descriptor.expectsMediaDataInRealTime) },
			{ "gop_frames", Value::Unsigned(descriptor.gopFrames) },
			{ "input_pixel_format", Value::String(descriptor.inputPixelFormat) },
			{ "max_b_frames", Value::Unsigned(descriptor.maxBFrames) },
			{ "muxer_flags", Value::String(descriptor.muxerFlags) },
			{ "output_pixel_format", Value::String(descriptor.outputPixelFormat) },
			{ "pq_peak_nits", Value::Unsigned(descriptor.pqPeakNits) },
			{ "rate_control", Value::String(descriptor.rateControl) },
			{ "reference_white_nits", Value::Unsigned(descriptor.referenceWhiteNits) },
			{ "schema_version", Value::Unsigned(descriptor.schemaVersion) },
			{ "transfer_function", Value::String(descriptor.transferFunction) },
			{ "ycbcr_matrix", Value::String(descriptor.ycbcrMatrix) }
		});
		Value::Members resolvedMembers = resolvedConfig.GetMap();
		resolvedMembers.push_back(std::make_pair("output",Value::MapValue({
			{ "bits_per_channel", Value::Unsigned(descriptor.bitsPerChannel) },
			{ "codec", Value::String(descriptor.codec) },
			{ "color_space", Value::String("rec2020_pq") },
			{ "display_transform", Value::String("rec709_linear_to_rec2020_pq") },
			{ "encoding", encodingDescriptor },
			{ "first_frame_index", Value::Unsigned(frames.front().frameIndex) },
			{ "format", Value::String(descriptor.containerFormat) },
			{ "frame_count", Value::Unsigned(frames.size()) },
			{ "frames_per_second", Value::Unsigned(framesPerSecond) },
			{ "height", Value::Unsigned(height) },
			{ "width", Value::Unsigned(width) }
		})));
		const Value finalizedConfig = Value::MapValue(resolvedMembers);
		RISECBOR64::Bytes finalizedConfigBytes;
		if( !RISECBOR64::Encode(finalizedConfig,finalizedConfigBytes,&error) ) return false;

		const Value payload = Value::MapValue({
			{ "active_fire_media", ActiveFireMediaArray(metadata.activeFireMedia) },
			{ "active_fire_optics_record_ids", TextArray(metadata.activeFireOpticsRecordIds) },
			{ "artifact_fidelity", Value::String("display_derivative") },
			{ "artifact_reason_codes", TextArray({
				"display_transform_enabled","integer_output","lossy_output" }) },
			{ "artifact_sha256", Value::String(artifactSha256) },
			{ "derivation_kind", Value::String("frame_sequence") },
			{ "derived_from_frames", Value::ArrayValue(links) },
			{ "derived_from_primary", Value() },
			{ "record_kind", Value::String("fire_output_provenance") },
			{ "render_fidelity_status", Value::String(metadata.renderFidelityStatus) },
			{ "render_reason_codes", TextArray(metadata.renderReasonCodes) },
			{ "renderer_build_id", Value::String(metadata.rendererBuildId) },
			{ "renderer_build_v1", rendererBuild },
			{ "resolved_render_configuration_id", Value::String(
				RISECBOR64::SHA256Hex(finalizedConfigBytes)) },
			{ "resolved_render_configuration_v1", finalizedConfig },
			{ "schema_version", Value::Unsigned(1u) }
		});
		RISECBOR64::Bytes payloadBytes;
		if( !RISECBOR64::Encode(payload,payloadBytes,&error) ) return false;
		const Value envelope = Value::MapValue({
			{ "payload", payload },
			{ "provenance_id", Value::String(RISECBOR64::SHA256Hex(payloadBytes)) }
		});
		return RISECBOR64::Encode(envelope,sidecar,&error);
	}

	bool WriteClosedFile(
		const std::string& filename,
		const RISECBOR64::Bytes& bytes,
		std::string& error
		)
	{
		DiskFileWriteBuffer* output = new DiskFileWriteBuffer(filename.c_str());
		const bool success = output->ReadyToWrite() &&
			output->setBytes(bytes.empty() ? 0 : &bytes[0],
				static_cast<unsigned int>(bytes.size())) && output->Close();
		safe_release(output);
		if( !success ) error = "could not write and close '"+filename+"'";
		return success;
	}

	std::string UniqueSibling( const std::string& filename, const char* role )
	{
		static std::atomic<std::uint64_t> counter(0u);
		const std::uint64_t serial = counter.fetch_add(1u,std::memory_order_relaxed);
		const std::uint64_t ticks = static_cast<std::uint64_t>(
			std::chrono::steady_clock::now().time_since_epoch().count());
		return filename+".rise-tmp."+role+"."+std::to_string(ticks)+"."+
			std::to_string(serial);
	}

	bool EncodeToClosedFile(
		const FrameStore& store,
		IFrameEncoder& encoder,
		const EncodeOpts& opts,
		const std::string& filename,
		std::string& error )
	{
		DiskFileWriteBuffer* artifact = new DiskFileWriteBuffer(filename.c_str());
		if( !artifact->ReadyToWrite() ) {
			error = "could not open the temporary artifact";
			safe_release(artifact);
			return false;
		}
		bool encoded = true;
		try {
			encoder.Encode(store,*artifact,opts);
		}
		catch( ... ) {
			encoded = false;
		}
		const bool artifactClosed = artifact->Close();
		safe_release(artifact);
		if( !encoded || !artifactClosed || !ArtifactHasBytes(filename.c_str()) ) {
			std::remove(filename.c_str());
			error = "artifact encoding produced no finalized bytes";
			return false;
		}
		return true;
	}

	bool IsDirectory( const std::string& filename )
	{
		std::error_code ec;
		return std::filesystem::is_directory(std::filesystem::path(filename),ec);
	}

	bool MoveExistingToBackup(
		const std::string& filename,
		const std::string& backup,
		bool& moved,
		std::string& error )
	{
		moved = false;
		if( IsDirectory(filename) ) {
			error = "refusing to replace directory '"+filename+"'";
			return false;
		}
		errno = 0;
		if( std::rename(filename.c_str(),backup.c_str()) == 0 ) {
			moved = true;
			return true;
		}
		if( errno == ENOENT ) return true;
		error = "could not preserve existing file '"+filename+"'";
		return false;
	}

	void RestoreBackup(
		const std::string& filename,
		const std::string& backup,
		const bool moved )
	{
		if( !moved ) return;
		std::remove(filename.c_str());
		std::rename(backup.c_str(),filename.c_str());
	}

	struct EXRChannelFacts
	{
		std::uint32_t pixelType = 0u;
		bool pLinear = false;
		std::int32_t xSampling = 0;
		std::int32_t ySampling = 0;
	};

	struct EXRHeaderFacts
	{
		std::map<std::string,std::string> fireAttributes;
		std::map<std::string,std::string> stringAttributes;
		std::map<std::string,EXRChannelFacts> channels;
		std::set<std::string> attributeNames;
		int compression = -1;
		int lineOrder = -1;
		std::array<std::int32_t,4u> dataWindow{};
		std::array<std::int32_t,4u> displayWindow{};
		std::array<float,8u> chromaticities{};
		std::array<float,2u> screenWindowCenter{};
		float pixelAspectRatio = 0.0f;
		float screenWindowWidth = 0.0f;
		float whiteLuminance = 0.0f;
		std::size_t headerEnd = 0u;
		bool hasDataWindow = false;
		bool hasDisplayWindow = false;
		bool hasChromaticities = false;
		bool hasPixelAspectRatio = false;
		bool hasScreenWindowCenter = false;
		bool hasScreenWindowWidth = false;
		bool hasWhiteLuminance = false;
	};

	float ReadLEFloat( const RISECBOR64::Bytes& bytes, const std::size_t offset )
	{
		const std::uint32_t bits = ReadLE32(bytes,offset);
		float value = 0.0f;
		std::memcpy(&value,&bits,sizeof(value));
		return value;
	}

	bool ReadCStringWithin(
		const RISECBOR64::Bytes& bytes,
		std::size_t& cursor,
		const std::size_t end,
		std::string& value )
	{
		const std::size_t begin = cursor;
		while( cursor < end && bytes[cursor] != 0u ) ++cursor;
		if( cursor >= end ) return false;
		value.assign(reinterpret_cast<const char*>(&bytes[begin]),cursor-begin);
		++cursor;
		return true;
	}

	bool ReadEXRHeaderFacts(
		const RISECBOR64::Bytes& encoded,
		EXRHeaderFacts& facts,
		std::string& error )
	{
		facts = EXRHeaderFacts();
		if( encoded.size() < 17u || ReadLE32(encoded,0u) != 20000630u ) {
			error = "artifact is not a supported OpenEXR byte stream";
			return false;
		}
		const std::uint32_t version = ReadLE32(encoded,4u);
		if( (version & 0xffu) != 2u || (version & ~(0xffu|0x400u)) != 0u ) {
			error = "OpenEXR version flags are outside the single-part scanline profile";
			return false;
		}
		std::size_t cursor = 8u;
		while( cursor < encoded.size() ) {
			std::string name;
			if( !ReadCString(encoded,cursor,name) ) {
				error = "unterminated OpenEXR attribute name";
				return false;
			}
			if( name.empty() ) {
				facts.headerEnd = cursor;
				if( facts.channels.empty() || facts.compression < 0 ||
					facts.lineOrder < 0 || !facts.hasDataWindow ||
					!facts.hasDisplayWindow || !facts.hasPixelAspectRatio ||
					!facts.hasScreenWindowCenter || !facts.hasScreenWindowWidth ) {
					error = "OpenEXR required scanline header attributes are incomplete";
					return false;
				}
				return true;
			}
			if( !facts.attributeNames.insert(name).second ) {
				error = "OpenEXR header attribute is duplicated";
				return false;
			}
			std::string type;
			if( !ReadCString(encoded,cursor,type) || cursor+4u > encoded.size() ) {
				error = "truncated OpenEXR attribute header";
				return false;
			}
			const std::uint32_t size = ReadLE32(encoded,cursor);
			cursor += 4u;
			if( size > encoded.size()-cursor ) {
				error = "truncated OpenEXR attribute value";
				return false;
			}
			const std::size_t valueEnd = cursor+size;
			if( type == "string" ) {
				const std::string value(reinterpret_cast<const char*>(&encoded[cursor]),size);
				facts.stringAttributes.emplace(name,value);
				if( name.compare(0u,std::strlen(kFireAttributePrefix),kFireAttributePrefix) == 0 &&
					!facts.fireAttributes.emplace(name,value).second ) {
					error = "fire provenance EXR attribute is not a unique string";
					return false;
				}
			} else if( name.compare(0u,std::strlen(kFireAttributePrefix),
				kFireAttributePrefix) == 0 ) {
				error = "fire provenance EXR attribute is not a unique string";
				return false;
			}
			if( name == "compression" ) {
				if( type != "compression" || size != 1u ) {
					error = "OpenEXR compression attribute is malformed";
					return false;
				}
				facts.compression = encoded[cursor];
			} else if( name == "channels" ) {
				if( type != "chlist" ) {
					error = "OpenEXR channels attribute is malformed";
					return false;
				}
				std::size_t channelCursor = cursor;
				bool terminated = false;
				while( channelCursor < valueEnd ) {
					std::string channel;
					if( !ReadCStringWithin(encoded,channelCursor,valueEnd,channel) ) break;
					if( channel.empty() ) {
						terminated = channelCursor == valueEnd;
						break;
					}
					if( channelCursor+16u > valueEnd ) break;
					const std::uint32_t pixelType = ReadLE32(encoded,channelCursor);
					const unsigned char linear = encoded[channelCursor+4u];
					const bool reservedZero = encoded[channelCursor+5u] == 0u &&
						encoded[channelCursor+6u] == 0u && encoded[channelCursor+7u] == 0u;
					EXRChannelFacts channelFacts;
					channelFacts.pixelType = pixelType;
					channelFacts.pLinear = linear != 0u;
					channelFacts.xSampling = static_cast<std::int32_t>(
						ReadLE32(encoded,channelCursor+8u));
					channelFacts.ySampling = static_cast<std::int32_t>(
						ReadLE32(encoded,channelCursor+12u));
					if( pixelType > 2u || linear > 1u || !reservedZero ||
						!facts.channels.emplace(channel,channelFacts).second ) {
						error = "OpenEXR channel list is invalid";
						return false;
					}
					channelCursor += 16u;
				}
				if( !terminated || facts.channels.empty() ) {
					error = "OpenEXR channel list is truncated";
					return false;
				}
			} else if( name == "dataWindow" || name == "displayWindow" ) {
				if( type != "box2i" || size != 16u ) {
					error = "OpenEXR image window attribute is malformed";
					return false;
				}
				std::array<std::int32_t,4u> window{};
				for( std::size_t i=0; i<window.size(); ++i ) {
					window[i] = static_cast<std::int32_t>(ReadLE32(encoded,cursor+i*4u));
				}
				if( name == "dataWindow" ) {
					facts.dataWindow = window;
					facts.hasDataWindow = true;
				} else {
					facts.displayWindow = window;
					facts.hasDisplayWindow = true;
				}
			} else if( name == "lineOrder" ) {
				if( type != "lineOrder" || size != 1u || encoded[cursor] > 2u ) {
					error = "OpenEXR line order attribute is malformed";
					return false;
				}
				facts.lineOrder = encoded[cursor];
			} else if( name == "pixelAspectRatio" ) {
				if( type != "float" || size != 4u ) {
					error = "OpenEXR pixel aspect ratio attribute is malformed";
					return false;
				}
				facts.pixelAspectRatio = ReadLEFloat(encoded,cursor);
				facts.hasPixelAspectRatio = std::isfinite(facts.pixelAspectRatio) &&
					facts.pixelAspectRatio > 0.0f;
				if( !facts.hasPixelAspectRatio ) {
					error = "OpenEXR pixel aspect ratio is not positive and finite";
					return false;
				}
			} else if( name == "screenWindowCenter" ) {
				if( type != "v2f" || size != 8u ) {
					error = "OpenEXR screen-window center attribute is malformed";
					return false;
				}
				facts.screenWindowCenter = {
					ReadLEFloat(encoded,cursor), ReadLEFloat(encoded,cursor+4u) };
				facts.hasScreenWindowCenter =
					std::isfinite(facts.screenWindowCenter[0]) &&
					std::isfinite(facts.screenWindowCenter[1]);
				if( !facts.hasScreenWindowCenter ) {
					error = "OpenEXR screen-window center is not finite";
					return false;
				}
			} else if( name == "screenWindowWidth" ) {
				if( type != "float" || size != 4u ) {
					error = "OpenEXR screen-window width attribute is malformed";
					return false;
				}
				facts.screenWindowWidth = ReadLEFloat(encoded,cursor);
				facts.hasScreenWindowWidth = std::isfinite(facts.screenWindowWidth) &&
					facts.screenWindowWidth > 0.0f;
				if( !facts.hasScreenWindowWidth ) {
					error = "OpenEXR screen-window width is not positive and finite";
					return false;
				}
			} else if( name == "whiteLuminance" ) {
				if( type != "float" || size != 4u ) {
					error = "OpenEXR white luminance attribute is malformed";
					return false;
				}
				facts.whiteLuminance = ReadLEFloat(encoded,cursor);
				facts.hasWhiteLuminance = std::isfinite(facts.whiteLuminance) &&
					facts.whiteLuminance > 0.0f;
				if( !facts.hasWhiteLuminance ) {
					error = "OpenEXR white luminance is not positive and finite";
					return false;
				}
			} else if( name == "chromaticities" ) {
				if( type != "chromaticities" || size != 32u ) {
					error = "OpenEXR chromaticities attribute is malformed";
					return false;
				}
				for( std::size_t i=0; i<facts.chromaticities.size(); ++i ) {
					facts.chromaticities[i] = ReadLEFloat(encoded,cursor+i*4u);
				}
				facts.hasChromaticities = true;
			}
			cursor += size;
		}
		error = "OpenEXR header terminator is missing";
		return false;
	}

	bool ValidateEXRScanlineStructure(
		const RISECBOR64::Bytes& encoded,
		const EXRHeaderFacts& facts,
		std::string& error )
	{
		if( facts.lineOrder != 0 || facts.dataWindow[0] > facts.dataWindow[2] ||
			facts.dataWindow[1] > facts.dataWindow[3] ) {
			error = "OpenEXR is not an increasing-order nonempty scanline image";
			return false;
		}
		for( const auto& channel : facts.channels ) {
			if( channel.second.xSampling != 1 || channel.second.ySampling != 1 ) {
				error = "OpenEXR channel sampling is not one sample per image pixel";
				return false;
			}
		}
		const std::uint64_t height = static_cast<std::uint64_t>(
			static_cast<std::int64_t>(facts.dataWindow[3])-
			static_cast<std::int64_t>(facts.dataWindow[1])+1);
		const std::uint64_t linesPerChunk = facts.compression == 0 ? 1u :
			(facts.compression == 3 ? 16u :
				((facts.compression == 4 || facts.compression == 8) ? 32u : 0u));
		if( linesPerChunk == 0u ) {
			error = "OpenEXR compression is outside the supported scanline profile";
			return false;
		}
		const std::uint64_t chunkCount = (height+linesPerChunk-1u)/linesPerChunk;
		if( chunkCount == 0u || chunkCount >
			(static_cast<std::uint64_t>(encoded.size()-facts.headerEnd)/8u) ) {
			error = "OpenEXR chunk offset table is missing or truncated";
			return false;
		}
		const std::size_t tableBytes = static_cast<std::size_t>(chunkCount*8u);
		std::uint64_t expectedOffset = static_cast<std::uint64_t>(facts.headerEnd+tableBytes);
		for( std::uint64_t i=0u; i<chunkCount; ++i ) {
			const std::uint64_t offset = ReadLE64(encoded,
				facts.headerEnd+static_cast<std::size_t>(i*8u));
			if( offset != expectedOffset || offset+8u > encoded.size() ) {
				error = "OpenEXR chunk offset table is not contiguous or in range";
				return false;
			}
			const std::int32_t y = static_cast<std::int32_t>(
				ReadLE32(encoded,static_cast<std::size_t>(offset)));
			const std::int64_t expectedY = static_cast<std::int64_t>(facts.dataWindow[1])+
				static_cast<std::int64_t>(i*linesPerChunk);
			const std::uint32_t packedBytes = ReadLE32(encoded,
				static_cast<std::size_t>(offset)+4u);
			if( static_cast<std::int64_t>(y) != expectedY || packedBytes == 0u ||
				packedBytes > encoded.size()-static_cast<std::size_t>(offset)-8u ) {
				error = "OpenEXR scanline pixel chunk is missing or truncated";
				return false;
			}
			expectedOffset = offset+8u+packedBytes;
		}
		if( expectedOffset != encoded.size() ) {
			error = "OpenEXR scanline chunks do not cover the exact artifact bytes";
			return false;
		}
		return true;
	}

	bool ValidateEXRPixelDecode(
		const RISECBOR64::Bytes& encoded,
		const EXRHeaderFacts& facts,
		std::string& error )
	{
#ifndef NO_EXR_SUPPORT
		if( encoded.empty() || encoded.size() >
			static_cast<std::size_t>(std::numeric_limits<int>::max()) ) {
			error = "OpenEXR artifact is outside the in-memory decoder size range";
			return false;
		}
		MemoryBuffer* buffer = new MemoryBuffer(static_cast<unsigned int>(encoded.size()));
		bool decoded = false;
		try {
			if( !buffer->setBytes(encoded.data(),static_cast<unsigned int>(encoded.size())) ||
				!buffer->seek(IBuffer::START,0) ) {
				error = "OpenEXR artifact could not be staged for authoritative decode";
			} else {
				EXRReader reader(*buffer,eColorSpace_Rec709RGB_Linear);
				unsigned int width = 0u;
				unsigned int height = 0u;
				reader.BeginRead(width,height);
				reader.EndRead();
				const std::uint64_t expectedWidth = static_cast<std::uint64_t>(
					static_cast<std::int64_t>(facts.dataWindow[2])-
					static_cast<std::int64_t>(facts.dataWindow[0])+1);
				const std::uint64_t expectedHeight = static_cast<std::uint64_t>(
					static_cast<std::int64_t>(facts.dataWindow[3])-
					static_cast<std::int64_t>(facts.dataWindow[1])+1);
				decoded = width == expectedWidth && height == expectedHeight;
				if( !decoded ) error = "OpenEXR decoded dimensions do not match its data window";
			}
		}
		catch( const std::exception& ex ) {
			error = std::string("OpenEXR pixel decode failed: ")+ex.what();
		}
		catch( ... ) {
			error = "OpenEXR pixel decode failed with a nonstandard exception";
		}
		buffer->release();
		return decoded;
#else
		(void)encoded;
		(void)facts;
		error = "OpenEXR pixel decode support is unavailable";
		return false;
#endif
	}

	bool ResolveFireEXRPixelAspectRatio(
		const FrameStore::Metadata& metadata,
		float& ratio,
		std::string& error )
	{
		using RISECBOR64::Value;
		Value config;
		if( !RISECBOR64::DecodeCanonical(metadata.resolvedRenderConfigCoreV1,
			config,&error) || config.GetType() != Value::Map ) {
			error = "resolved fire configuration is unavailable for EXR pixel aspect ratio: "+error;
			return false;
		}
		const Value* film = config.Find("film");
		const Value* encodedRatio = film ? film->Find("pixel_aspect_ratio") : nullptr;
		if( !encodedRatio || encodedRatio->GetType() != Value::Float64 ||
			!std::isfinite(encodedRatio->GetFloat()) || encodedRatio->GetFloat() <= 0.0 ||
			encodedRatio->GetFloat() > std::numeric_limits<float>::max() ) {
			error = "resolved fire configuration has no encodable pixel aspect ratio";
			return false;
		}
		ratio = static_cast<float>(encodedRatio->GetFloat());
		return true;
	}

	bool ValidateEXRHeaderAgainstPayload(
		const EXRHeaderFacts& facts,
		const RISECBOR64::Value& payload,
		std::string& error )
	{
		using RISECBOR64::Value;
		const Value* config = payload.Find("resolved_render_configuration_v1");
		const Value* output = config ? config->Find("output") : nullptr;
		if( !output ) {
			error = "fire provenance EXR output claim is unavailable";
			return false;
		}
		const std::uint64_t bits = output->Find("bits_per_channel")->GetIntegerArgument();
		const std::uint32_t expectedPixelType = bits == 32u ? 2u : 1u;
		const bool expectedPLinear = output->Find("exr_channel_p_linear")->GetBoolean();
		const std::int32_t expectedXSampling = static_cast<std::int32_t>(
			output->Find("exr_channel_x_sampling")->GetIntegerArgument());
		const std::int32_t expectedYSampling = static_cast<std::int32_t>(
			output->Find("exr_channel_y_sampling")->GetIntegerArgument());
		for( const char* channel : { "R", "G", "B" } ) {
			const auto found = facts.channels.find(channel);
			if( found == facts.channels.end() ||
				found->second.pixelType != expectedPixelType ) {
				error = "OpenEXR channel precision does not match the resolved output claim";
				return false;
			}
			if( found->second.pLinear != expectedPLinear ||
				found->second.xSampling != expectedXSampling ||
				found->second.ySampling != expectedYSampling ) {
				error = "OpenEXR channel profile does not match the resolved output claim";
				return false;
			}
		}
		const auto alpha = facts.channels.find("A");
		const bool claimedAlpha = output->Find("exr_with_alpha")->GetBoolean();
		if( (alpha != facts.channels.end()) != claimedAlpha ||
			(alpha != facts.channels.end() &&
				(alpha->second.pixelType != expectedPixelType ||
				 alpha->second.pLinear != expectedPLinear ||
				 alpha->second.xSampling != expectedXSampling ||
				 alpha->second.ySampling != expectedYSampling)) ) {
			error = "OpenEXR alpha channels do not match the resolved output claim";
			return false;
		}
		if( output->Find("include_aovs")->GetBoolean() ||
			!output->Find("aov_channels")->GetArray().empty() ||
			facts.channels.size() != (claimedAlpha ? 4u : 3u) ) {
			error = "OpenEXR channel set does not match the resolved output claim";
			return false;
		}
		const std::string& compression = output->Find("exr_compression")->GetText();
		const int expectedCompression = compression == "none" ? 0 :
			(compression == "zip" ? 3 : (compression == "piz" ? 4 : 8));
		if( facts.compression != expectedCompression ) {
			error = "OpenEXR compression does not match the resolved output claim";
			return false;
		}
		if( facts.lineOrder != 0 ||
			output->Find("exr_line_order")->GetText() != "increasing_y" ) {
			error = "OpenEXR line order does not match the resolved output claim";
			return false;
		}
		const std::string& colorSpace = output->Find("color_space")->GetText();
		const std::array<float,8u> expectedChromaticities =
			(colorSpace == "romm_linear" || colorSpace == "prophoto_rgb") ?
			std::array<float,8u>{ 0.7347f,0.2653f,0.1596f,0.8404f,
				0.0366f,0.0001f,0.3457f,0.3585f } :
			std::array<float,8u>{ 0.6400f,0.3300f,0.3000f,0.6000f,
				0.1500f,0.0600f,0.3127f,0.3290f };
		if( !facts.hasChromaticities || facts.chromaticities != expectedChromaticities ) {
			error = "OpenEXR chromaticities do not match the resolved output claim";
			return false;
		}
		const Value* film = config->Find("film");
		const std::uint64_t width = film->Find("width")->GetIntegerArgument();
		const std::uint64_t height = film->Find("height")->GetIntegerArgument();
		if( width == 0u || height == 0u || width > 0x7fffffffu ||
			height > 0x7fffffffu || facts.dataWindow !=
				std::array<std::int32_t,4u>{ 0,0,static_cast<std::int32_t>(width-1u),
					static_cast<std::int32_t>(height-1u) } ||
			facts.displayWindow != facts.dataWindow ) {
			error = "OpenEXR image windows do not match the resolved film dimensions";
			return false;
		}
		const float configuredPixelAspectRatio = static_cast<float>(
			output->Find("exr_pixel_aspect_ratio")->GetFloat());
		if( configuredPixelAspectRatio != static_cast<float>(
			film->Find("pixel_aspect_ratio")->GetFloat()) ||
			facts.pixelAspectRatio != configuredPixelAspectRatio ) {
			error = "OpenEXR pixel aspect ratio does not match the resolved film claim";
			return false;
		}
		const Value::Values& screenCenter =
			output->Find("exr_screen_window_center")->GetArray();
		if( facts.screenWindowCenter != std::array<float,2u>{
				static_cast<float>(screenCenter[0].GetFloat()),
				static_cast<float>(screenCenter[1].GetFloat()) } ||
			facts.screenWindowWidth != static_cast<float>(
				output->Find("exr_screen_window_width")->GetFloat()) ||
			facts.whiteLuminance != static_cast<float>(
				output->Find("exr_white_luminance")->GetFloat()) ) {
			error = "OpenEXR interpretation attributes do not match the resolved output claim";
			return false;
		}
		const auto software = facts.stringAttributes.find("software");
		if( software == facts.stringAttributes.end() ||
			software->second != output->Find("exr_software")->GetText() ) {
			error = "OpenEXR software attribute does not match the resolved output claim";
			return false;
		}
		std::set<std::string> expectedAttributes = {
			"channels", "chromaticities", "compression", "dataWindow",
			"displayWindow", "lineOrder", "pixelAspectRatio", "screenWindowCenter",
			"screenWindowWidth", "software", "whiteLuminance"
		};
		for( const Value& attribute : output->Find("attributes")->GetArray() ) {
			const std::string& name = attribute.Find("name")->GetText();
			const std::string& value = attribute.Find("value")->GetText();
			expectedAttributes.insert(name);
			const auto actual = facts.stringAttributes.find(name);
			if( actual == facts.stringAttributes.end() || actual->second != value ) {
				error = "OpenEXR authored attributes do not match the resolved output claim";
				return false;
			}
		}
		for( const auto& attribute : facts.fireAttributes ) {
			expectedAttributes.insert(attribute.first);
		}
		if( facts.attributeNames != expectedAttributes ) {
			error = "OpenEXR contains an undeclared header attribute";
			return false;
		}
		return true;
	}
}

void RISE::Implementation::SetFileTransactionContentionHookForTests(
	const FileTransactionContentionHook hook,
	void* const context )
{
	gFileTransactionContentionContext.store(context,std::memory_order_release);
	gFileTransactionContentionHook.store(hook,std::memory_order_release);
}

bool RISE::Implementation::StripFireProvenanceEXRAttributes(
	const std::vector<unsigned char>& encoded,
	std::vector<unsigned char>& stripped,
	std::string& error )
{
	error.clear();
	stripped.clear();
	EXRHeaderFacts facts;
	if( !ReadEXRHeaderFacts(encoded,facts,error) ||
		!ValidateEXRScanlineStructure(encoded,facts,error) ) return false;
	stripped.insert(stripped.end(),encoded.begin(),encoded.begin()+8u);
	std::size_t cursor = 8u;
	bool retainedLongName = false;
	while( cursor < encoded.size() ) {
		const std::size_t attributeBegin = cursor;
		std::string name;
		if( !ReadCString(encoded,cursor,name) ) {
			error = "unterminated OpenEXR attribute name";
			return false;
		}
		if( name.empty() ) break;
		std::string type;
		if( !ReadCString(encoded,cursor,type) || cursor+4u > encoded.size() ) {
			error = "truncated OpenEXR attribute header";
			return false;
		}
		const std::uint32_t size = ReadLE32(encoded,cursor);
		cursor += 4u;
		if( size > encoded.size()-cursor ) {
			error = "truncated OpenEXR attribute value";
			return false;
		}
		cursor += size;
		if( name.compare(0u,std::strlen(kFireAttributePrefix),kFireAttributePrefix) != 0 ) {
			retainedLongName = retainedLongName || name.size() > 31u;
			stripped.insert(stripped.end(),encoded.begin()+attributeBegin,encoded.begin()+cursor);
		}
	}
	// OpenEXR sets LONG_NAMES_FLAG when any provenance attribute name exceeds
	// 31 bytes. Removing those attributes must also remove the header flag.
	if( !retainedLongName ) {
		const std::uint32_t strippedVersion = ReadLE32(stripped,4u) & ~0x400u;
		stripped[4] = static_cast<unsigned char>(strippedVersion & 0xffu);
		stripped[5] = static_cast<unsigned char>((strippedVersion >> 8u) & 0xffu);
		stripped[6] = static_cast<unsigned char>((strippedVersion >> 16u) & 0xffu);
		stripped[7] = static_cast<unsigned char>((strippedVersion >> 24u) & 0xffu);
	}
	if( cursor >= encoded.size() ) {
		error = "OpenEXR header terminator is missing";
		return false;
	}
	const std::size_t oldHeaderEnd = cursor;
	stripped.push_back(0u);
	const std::size_t newHeaderEnd = stripped.size();
	if( oldHeaderEnd+8u > encoded.size() ) {
		error = "OpenEXR chunk offset table is missing";
		return false;
	}
	const std::uint64_t firstChunk = ReadLE64(encoded,oldHeaderEnd);
	if( firstChunk < oldHeaderEnd || firstChunk >= encoded.size() ||
		(firstChunk-oldHeaderEnd)%8u != 0u ) {
		error = "OpenEXR chunk offset table is malformed";
		return false;
	}
	const std::size_t tableBytes = static_cast<std::size_t>(firstChunk-oldHeaderEnd);
	if( tableBytes == 0u || oldHeaderEnd+tableBytes > encoded.size() ) {
		error = "OpenEXR chunk offset table is empty or truncated";
		return false;
	}
	const std::uint64_t delta = static_cast<std::uint64_t>(oldHeaderEnd-newHeaderEnd);
	for( std::size_t offset=0u; offset<tableBytes; offset+=8u ) {
		const std::uint64_t chunk = ReadLE64(encoded,oldHeaderEnd+offset);
		if( chunk < delta || chunk >= encoded.size() ) {
			error = "OpenEXR chunk offset is out of range";
			return false;
		}
		AppendLE64(stripped,chunk-delta);
	}
	stripped.insert(stripped.end(),encoded.begin()+oldHeaderEnd+tableBytes,encoded.end());
	return true;
}

bool RISE::Implementation::VerifyFireProvenanceEXR(
	const std::vector<unsigned char>& encodedArtifact,
	const std::vector<unsigned char>& encodedSidecar,
	std::string& error )
{
	using RISECBOR64::Value;
	error.clear();
	Value envelope;
	if( !RISECBOR64::DecodeCanonical(encodedSidecar,envelope,&error) ||
		envelope.GetType() != Value::Map || envelope.GetMap().size() != 2u ) {
		error = "fire provenance sidecar is not the canonical two-member envelope: "+error;
		return false;
	}
	const Value* payload = envelope.Find("payload");
	const Value* provenanceId = envelope.Find("provenance_id");
	if( !payload || payload->GetType() != Value::Map || !provenanceId ||
		provenanceId->GetType() != Value::Text ) {
		error = "fire provenance envelope is missing payload or provenance_id";
		return false;
	}
	if( !ValidateFireProvenancePayload(*payload,error) ) return false;
	RISECBOR64::Bytes payloadBytes;
	if( !RISECBOR64::Encode(*payload,payloadBytes,&error) ||
		provenanceId->GetText() != RISECBOR64::SHA256Hex(payloadBytes) ) {
		error = "fire provenance_id does not hash the exact payload preimage";
		return false;
	}
	const Value* artifactDigest = payload->Find("artifact_sha256");
	if( !artifactDigest || artifactDigest->GetType() != Value::Text ) {
		error = "fire provenance payload is missing artifact_sha256";
		return false;
	}
	EXRHeaderFacts facts;
	if( !ReadEXRHeaderFacts(encodedArtifact,facts,error) ||
		!ValidateEXRScanlineStructure(encodedArtifact,facts,error) ||
		!ValidateEXRHeaderAgainstPayload(facts,*payload,error) ||
		!ValidateEXRPixelDecode(encodedArtifact,facts,error) ) return false;
	RISECBOR64::Bytes stripped;
	if( !StripFireProvenanceEXRAttributes(encodedArtifact,stripped,error) ||
		artifactDigest->GetText() != RISECBOR64::SHA256Hex(stripped) ) {
		if( error.empty() ) error = "fire artifact_sha256 does not hash the stripped EXR";
		return false;
	}
	std::map<std::string,std::string> expected;
	for( std::size_t i=0; i<payload->GetMap().size(); ++i ) {
		std::string json;
		if( !CanonicalJSON(payload->GetMap()[i].second,json,error) ) return false;
		expected[std::string(kFireAttributePrefix)+payload->GetMap()[i].first] = json;
	}
	expected[std::string(kFireAttributePrefix)+"provenance_id"] =
		EscapeJSONString(provenanceId->GetText());
	if( facts.fireAttributes != expected ) {
		error = "EXR fire-provenance attributes do not match the authoritative sidecar";
		return false;
	}
	return true;
}

bool RISE::Implementation::PublishFireFrameSequenceFileTransaction(
	const FrameStoreOutput::Metadata& metadata,
	const FireFrameSequenceEncoding encoding,
	const std::string& closedTemporaryArtifactFilename,
	const std::string& artifactFilename,
	const unsigned int width,
	const unsigned int height,
	const unsigned int framesPerSecond,
	const unsigned int encodedFrameCount,
	const std::vector<FireFramePrimary>& frames,
	const FireFrameSequenceArtifactValidator validateArtifact,
	std::string& error )
{
	auto transactionLock = AcquireFileTransactionLock();
	error.clear();
	if( encodedFrameCount != frames.size() || frames.empty() ) {
		error = "encoded movie frame count does not match the primary-link array";
		std::remove(closedTemporaryArtifactFilename.c_str());
		return false;
	}
	if( width == 0u || height == 0u || framesPerSecond == 0u ) {
		error = "encoded movie dimensions and frame rate must be nonzero";
		std::remove(closedTemporaryArtifactFilename.c_str());
		return false;
	}
	RISECBOR64::Bytes artifactBytes;
	if( !ReadArtifact(closedTemporaryArtifactFilename.c_str(),artifactBytes) ||
		artifactBytes.empty() ) {
		error = "closed temporary movie is missing or empty";
		std::remove(closedTemporaryArtifactFilename.c_str());
		return false;
	}
	if( !validateArtifact ) {
		error = "authored movie decoder validation is unavailable";
		std::remove(closedTemporaryArtifactFilename.c_str());
		return false;
	}
	if( !validateArtifact(closedTemporaryArtifactFilename,encoding,width,height,
		framesPerSecond,frames,error) ) {
		if( error.empty() ) error = "authored movie decoder rejected the finalized artifact";
		std::remove(closedTemporaryArtifactFilename.c_str());
		return false;
	}
	RISECBOR64::Bytes validatedArtifactBytes;
	if( !ReadArtifact(closedTemporaryArtifactFilename.c_str(),validatedArtifactBytes) ||
		validatedArtifactBytes != artifactBytes ) {
		error = "closed movie changed while its finalized artifact was being validated";
		std::remove(closedTemporaryArtifactFilename.c_str());
		return false;
	}
	RISECBOR64::Bytes sidecarBytes;
	if( !BuildFireFrameSequenceProvenance(metadata,encoding,
		RISECBOR64::SHA256Hex(artifactBytes),width,height,framesPerSecond,
		frames,sidecarBytes,error) ) {
		std::remove(closedTemporaryArtifactFilename.c_str());
		return false;
	}

	const std::string sidecarFilename = artifactFilename+".provenance.cbor";
	const std::string sidecarTemporary = UniqueSibling(sidecarFilename,"sidecar");
	if( !WriteClosedFile(sidecarTemporary,sidecarBytes,error) ) {
		std::remove(closedTemporaryArtifactFilename.c_str());
		std::remove(sidecarTemporary.c_str());
		return false;
	}
	const std::string artifactBackup = UniqueSibling(artifactFilename,"artifact-backup");
	const std::string sidecarBackup = UniqueSibling(sidecarFilename,"sidecar-backup");
	bool movedArtifact = false;
	bool movedSidecar = false;
	if( !MoveExistingToBackup(artifactFilename,artifactBackup,movedArtifact,error) ||
		!MoveExistingToBackup(sidecarFilename,sidecarBackup,movedSidecar,error) ) {
		RestoreBackup(artifactFilename,artifactBackup,movedArtifact);
		RestoreBackup(sidecarFilename,sidecarBackup,movedSidecar);
		std::remove(closedTemporaryArtifactFilename.c_str());
		std::remove(sidecarTemporary.c_str());
		return false;
	}
	if( std::rename(sidecarTemporary.c_str(),sidecarFilename.c_str()) != 0 ) {
		RestoreBackup(artifactFilename,artifactBackup,movedArtifact);
		RestoreBackup(sidecarFilename,sidecarBackup,movedSidecar);
		std::remove(closedTemporaryArtifactFilename.c_str());
		std::remove(sidecarTemporary.c_str());
		error = "could not commit the movie provenance sidecar";
		return false;
	}
	if( std::rename(closedTemporaryArtifactFilename.c_str(),artifactFilename.c_str()) != 0 ) {
		std::remove(sidecarFilename.c_str());
		RestoreBackup(artifactFilename,artifactBackup,movedArtifact);
		RestoreBackup(sidecarFilename,sidecarBackup,movedSidecar);
		std::remove(closedTemporaryArtifactFilename.c_str());
		error = "could not commit the movie artifact";
		return false;
	}
	std::remove(artifactBackup.c_str());
	std::remove(sidecarBackup.c_str());
	return true;
}

bool RISE::Implementation::PublishUnprovenancedFileTransaction(
	const std::string& closedTemporaryArtifactFilename,
	const std::string& artifactFilename,
	std::string& error )
{
	auto transactionLock = AcquireFileTransactionLock();
	error.clear();
	RISECBOR64::Bytes artifactBytes;
	if( !ReadArtifact(closedTemporaryArtifactFilename.c_str(),artifactBytes) ||
		artifactBytes.empty() ) {
		error = "closed temporary artifact is missing or empty";
		std::remove(closedTemporaryArtifactFilename.c_str());
		return false;
	}
	const std::string sidecarFilename = artifactFilename+".provenance.cbor";
	const std::string artifactBackup = UniqueSibling(artifactFilename,"artifact-backup");
	const std::string sidecarBackup = UniqueSibling(sidecarFilename,"sidecar-backup");
	bool movedArtifact = false;
	bool movedSidecar = false;
	if( !MoveExistingToBackup(artifactFilename,artifactBackup,movedArtifact,error) ||
		!MoveExistingToBackup(sidecarFilename,sidecarBackup,movedSidecar,error) ) {
		RestoreBackup(artifactFilename,artifactBackup,movedArtifact);
		RestoreBackup(sidecarFilename,sidecarBackup,movedSidecar);
		std::remove(closedTemporaryArtifactFilename.c_str());
		return false;
	}
	if( std::rename(closedTemporaryArtifactFilename.c_str(),artifactFilename.c_str()) != 0 ) {
		RestoreBackup(artifactFilename,artifactBackup,movedArtifact);
		RestoreBackup(sidecarFilename,sidecarBackup,movedSidecar);
		std::remove(closedTemporaryArtifactFilename.c_str());
		error = "could not commit the unprovenanced artifact";
		return false;
	}
	std::remove(artifactBackup.c_str());
	std::remove(sidecarBackup.c_str());
	return true;
}

bool RISE::Implementation::EncodeFrameStoreFileTransaction(
	FrameStore& store,
	IFrameEncoder& encoder,
	const EncodeOpts& opts,
	const std::string& artifactFilename,
	std::string& error
	)
{
	auto transactionLock = AcquireFileTransactionLock();
	error.clear();
	EncodeOpts transactionOpts;
	if( !ResolveEffectiveEncoderOptions(encoder,opts,transactionOpts,error) ) return false;
	if( !transactionOpts.useMetadataSnapshot ) {
		transactionOpts.metadataSnapshot = store.Meta();
		transactionOpts.useMetadataSnapshot = true;
	}
	if( encoder.FormatName() == "EXR" &&
		!transactionOpts.metadataSnapshot.renderFidelityStatus.empty() &&
		!ResolveFireEXRPixelAspectRatio(transactionOpts.metadataSnapshot,
			transactionOpts.exrPixelAspectRatio,error) ) return false;
	for( std::size_t i=0; i<transactionOpts.attrs.size(); ++i ) {
		if( transactionOpts.attrs[i].first.compare(0u,
			std::strlen(kFireAttributePrefix),kFireAttributePrefix) == 0 ) {
			error = "caller-supplied attributes may not use the riseFireProv_ prefix";
			return false;
		}
	}
	const std::string artifactBase = UniqueSibling(artifactFilename,"artifact-base");
	const std::string artifactTemporary = UniqueSibling(artifactFilename,"artifact-final");
	const std::string sidecarFilename = artifactFilename+".provenance.cbor";
	const std::string sidecarTemporary = UniqueSibling(sidecarFilename,"sidecar");
	if( !EncodeToClosedFile(store,encoder,transactionOpts,artifactBase,error) ) return false;

	const bool needsProvenance =
		!transactionOpts.metadataSnapshot.renderFidelityStatus.empty();
	std::string artifactHash;
	std::string artifactFidelity;
	std::string provenanceId;
	if( needsProvenance ) {
		RISECBOR64::Bytes baseBytes;
		if( !ReadArtifact(artifactBase.c_str(),baseBytes) ) {
			std::remove(artifactBase.c_str());
			error = "could not read the base artifact for hashing";
			return false;
		}
		artifactHash = RISECBOR64::SHA256Hex(baseBytes);
		const bool primary = IsPrimaryArtifact(encoder,transactionOpts);
		artifactFidelity = primary ?
			(transactionOpts.metadataSnapshot.renderFidelityStatus == "predictive" ?
				"predictive_primary" : "preview_primary") : "display_derivative";
		const std::vector<std::string> artifactReasons =
			ArtifactReasons(encoder,transactionOpts,
				transactionOpts.metadataSnapshot,primary);
		RISECBOR64::Bytes sidecarBytes;
		std::vector<std::pair<std::string,std::string> > fireAttributes;
		if( !BuildFireProvenance(transactionOpts.metadataSnapshot,encoder,
			transactionOpts,artifactHash,artifactFidelity,artifactReasons,
			sidecarBytes,fireAttributes,provenanceId,error) ) {
			std::remove(artifactBase.c_str());
			return false;
		}
		if( encoder.FormatName() == "EXR" ) {
			transactionOpts.attrs.insert(transactionOpts.attrs.end(),
				fireAttributes.begin(),fireAttributes.end());
			if( !EncodeToClosedFile(store,encoder,transactionOpts,artifactTemporary,error) ) {
				std::remove(artifactBase.c_str());
				return false;
			}
			RISECBOR64::Bytes finalBytes;
			RISECBOR64::Bytes strippedBytes;
			const bool finalRead = ReadArtifact(artifactTemporary.c_str(),finalBytes);
			const bool stripSucceeded = finalRead &&
				StripFireProvenanceEXRAttributes(finalBytes,strippedBytes,error);
			if( !stripSucceeded || strippedBytes != baseBytes ) {
				if( stripSucceeded && error.empty() ) {
					std::size_t mismatch = 0u;
					while( mismatch < strippedBytes.size() && mismatch < baseBytes.size() &&
						strippedBytes[mismatch] == baseBytes[mismatch] ) ++mismatch;
					error = "EXR provenance attributes changed the stripped artifact preimage "
						"(base="+std::to_string(baseBytes.size())+", stripped="+
						std::to_string(strippedBytes.size())+", first mismatch="+
						std::to_string(mismatch)+")";
				}
				std::remove(artifactBase.c_str());
				std::remove(artifactTemporary.c_str());
				if( error.empty() ) error = "could not read or strip the EXR provenance attributes";
				return false;
			}
			if( !VerifyFireProvenanceEXR(finalBytes,sidecarBytes,error) ) {
				std::remove(artifactBase.c_str());
				std::remove(artifactTemporary.c_str());
				return false;
			}
		} else if( std::rename(artifactBase.c_str(),artifactTemporary.c_str()) != 0 ) {
			std::remove(artifactBase.c_str());
			error = "could not finalize the temporary artifact";
			return false;
		}
		std::remove(artifactBase.c_str());
		if( !WriteClosedFile(sidecarTemporary,sidecarBytes,error) ) {
			std::remove(artifactTemporary.c_str());
			std::remove(sidecarTemporary.c_str());
			return false;
		}
	} else {
		if( std::rename(artifactBase.c_str(),artifactTemporary.c_str()) != 0 ) {
			std::remove(artifactBase.c_str());
			error = "could not finalize the temporary artifact";
			return false;
		}
	}

	const std::string artifactBackup = UniqueSibling(artifactFilename,"artifact-backup");
	const std::string sidecarBackup = UniqueSibling(sidecarFilename,"sidecar-backup");
	bool movedArtifact = false;
	bool movedSidecar = false;
	if( !MoveExistingToBackup(artifactFilename,artifactBackup,movedArtifact,error) ||
		!MoveExistingToBackup(sidecarFilename,sidecarBackup,movedSidecar,error) ) {
		RestoreBackup(artifactFilename,artifactBackup,movedArtifact);
		RestoreBackup(sidecarFilename,sidecarBackup,movedSidecar);
		std::remove(artifactTemporary.c_str());
		std::remove(sidecarTemporary.c_str());
		return false;
	}
	if( needsProvenance &&
		std::rename(sidecarTemporary.c_str(),sidecarFilename.c_str()) != 0 ) {
		RestoreBackup(artifactFilename,artifactBackup,movedArtifact);
		RestoreBackup(sidecarFilename,sidecarBackup,movedSidecar);
		std::remove(artifactTemporary.c_str());
		std::remove(sidecarTemporary.c_str());
		error = "could not commit the provenance sidecar";
		return false;
	}
	if( std::rename(artifactTemporary.c_str(),artifactFilename.c_str()) != 0 ) {
		if( needsProvenance ) std::remove(sidecarFilename.c_str());
		RestoreBackup(artifactFilename,artifactBackup,movedArtifact);
		RestoreBackup(sidecarFilename,sidecarBackup,movedSidecar);
		std::remove(artifactTemporary.c_str());
		error = "could not commit the encoded artifact";
		return false;
	}
	std::remove(artifactBackup.c_str());
	std::remove(sidecarBackup.c_str());
	if( needsProvenance && artifactFidelity != "display_derivative" ) {
		store.SetPrimaryFireArtifact(provenanceId,artifactHash,artifactFidelity);
	}
	return true;
}

FileEncoderObserver::FileEncoderObserver(
	FrameStore*         store,
	IFrameEncoder*      encoder,
	const EncodeOpts&   opts,
	const std::string&  filenamePattern,
	bool                bMultiple )
	: store_( store )
	, encoder_( encoder )
	, opts_( opts )
	, pattern_( filenamePattern )
	, bMultiple_( bMultiple )
{
	if ( store_ ) store_->addref();
	if ( encoder_ ) encoder_->addref();
}

FileEncoderObserver::~FileEncoderObserver()
{
	if ( encoder_ ) encoder_->release();
	if ( store_ ) store_->release();
}

void FileEncoderObserver::OnFrameComplete( unsigned int frame, uint64_t /*generation*/ )
{
	// No-suffix file — matches legacy
	// FileRasterizerOutput::OutputImage → WriteImageToFile(..., "").
	WriteFile( frame, "" );
}

void FileEncoderObserver::OnPreDenoiseComplete( unsigned int frame, uint64_t /*generation*/ )
{
	// Matches legacy
	// FileRasterizerOutput::OutputPreDenoisedImage → WriteImageToFile(..., "")
	// (the pre-denoise pass writes to the SAME filename as the
	// non-denoised path; the denoised pass writes to "_denoised").
	WriteFile( frame, "" );
}

void FileEncoderObserver::OnDenoiseComplete( unsigned int frame, uint64_t /*generation*/ )
{
	// Matches legacy
	// FileRasterizerOutput::OutputDenoisedImage → WriteImageToFile(..., "_denoised").
	WriteFile( frame, "_denoised", true );
}

void FileEncoderObserver::WriteFile(
	unsigned int frame,
	const char* suffix,
	const bool denoisedDerivative )
{
	if ( !store_ || !encoder_ ) return;

	// Determine the file extension from the encoder's first listed
	// extension.  This is the canonical extension per
	// IFrameEncoder.h:117.
	std::string ext;
	const auto exts = encoder_->Extensions();
	if ( !exts.empty() ) ext = exts.front();
	else                  ext = "out";

	std::string filename = BuildFrameArtifactFilename(
		pattern_,suffix ? suffix : "",frame,ext,bMultiple_);

	EncodeOpts frameOpts = opts_;
	frameOpts.frame = frame;
	frameOpts.denoisedDerivative = denoisedDerivative;
	const bool primaryArtifact = IsPrimaryArtifact(*encoder_,frameOpts);
	std::string writeError;
	if( !EncodeFrameStoreFileTransaction(*store_,*encoder_,frameOpts,filename,
		writeError) ) {
		if( !store_->Meta().renderFidelityStatus.empty() ) {
			if( primaryArtifact ) {
				GlobalLog()->PrintEx( eLog_Error,
					"FileEncoderObserver:: output_provenance_unavailable for '%s': %s",
					filename.c_str(),writeError.c_str() );
				throw std::runtime_error("output_provenance_unavailable: "+writeError);
			}
			GlobalLog()->PrintEx( eLog_Warning,
				"FileEncoderObserver:: display derivative transaction failed for '%s' "
				"(%s); the finalized fire primary remains valid",
				filename.c_str(),writeError.c_str() );
			return;
		}
		const FileEncoderObserver* pMe = this;
		const std::string emergencyPattern = "fro_temp_"+
			std::to_string(static_cast<unsigned long>(
				reinterpret_cast<uintptr_t>(pMe)))+
			(suffix ? suffix : "")+(bMultiple_ ? "_" : "");
		const std::string emergency = BuildFrameArtifactFilename(
			emergencyPattern,"",frame,ext,bMultiple_);

		std::string emergencyError;
		if( !EncodeFrameStoreFileTransaction(*store_,*encoder_,frameOpts,emergency,
			emergencyError) ) {
			GlobalLog()->PrintEx( eLog_Error,
				"FileEncoderObserver:: artifact transaction failed for '%s' (%s); "
				"emergency transaction also failed for '%s' (%s)",
				filename.c_str(),writeError.c_str(),emergency.c_str(),
				emergencyError.c_str() );
			return;
		}
		GlobalLog()->PrintEx( eLog_Warning,
			"Artifact transaction failed for '%s' (%s); rendered scene written "
			"transactionally to emergency file '%s' instead!",
			filename.c_str(),writeError.c_str(),emergency.c_str() );
		// Use the emergency filename as the effective filename for
		// the success log message below.
		filename = emergency;
	}

	GlobalLog()->PrintEx( eLog_Event,
		"FileEncoderObserver:: Written to '%s'", filename.c_str() );
}
