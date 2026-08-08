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

#include "../Utilities/DiskFileWriteBuffer.h"
#include "../Utilities/RISECBOR64.h"
#include "../Interfaces/ILog.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
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

namespace
{
	const char kFireAttributePrefix[] = "riseFireProv_";
	std::mutex gFileTransactionMutex;

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
		const bool primary )
	{
		if( primary ) return std::vector<std::string>();
		std::set<std::string> reasons;
		const std::string format = encoder.FormatName();
		if( format != "EXR" || opts.bpp < 32u ||
			opts.exrCompression == eExrCompression_Dwaa ) reasons.insert("lossy_output");
		if( !encoder.SupportsHDR() || format == "HDR10_PNG" ) reasons.insert("integer_output");
		if( opts.viewTransform.exposureEV != 0.0f ||
			opts.viewTransform.toneCurve != eDisplayTransform_None ||
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
		Value resolvedConfig;
		Value rendererBuild;
		if( metadata.resolvedRenderConfigCoreV1.empty() ||
			!RISECBOR64::DecodeCanonical(metadata.resolvedRenderConfigCoreV1,
				resolvedConfig,&error) || resolvedConfig.GetType() != Value::Map ) {
			error = "resolved render configuration is unavailable or noncanonical: "+error;
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
		resolvedMembers.push_back(std::make_pair("output",Value::MapValue({
			{ "bits_per_channel", Value::Unsigned(opts.bpp) },
			{ "color_space", Value::String(ColorSpaceName(opts.colorSpace)) },
			{ "compression_level", Value::Signed(opts.compressionLevel) },
			{ "denoised_derivative", Value::Bool(opts.denoisedDerivative) },
			{ "exr_compression", Value::String(EXRCompressionName(opts.exrCompression)) },
			{ "exr_with_alpha", Value::Bool(opts.exrWithAlpha) },
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

	bool BuildFireFrameSequenceProvenance(
		const FrameStoreOutput::Metadata& metadata,
		const std::string& artifactSha256,
		const unsigned int width,
		const unsigned int height,
		const unsigned int framesPerSecond,
		const std::vector<FireFramePrimary>& frames,
		RISECBOR64::Bytes& sidecar,
		std::string& error )
	{
		using RISECBOR64::Value;
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
		Value::Members resolvedMembers = resolvedConfig.GetMap();
		resolvedMembers.push_back(std::make_pair("output",Value::MapValue({
			{ "bits_per_channel", Value::Unsigned(12u) },
			{ "codec", Value::String("apple_prores_4444") },
			{ "color_space", Value::String("rec2020_pq") },
			{ "display_transform", Value::String("rec709_linear_to_rec2020_pq") },
			{ "first_frame_index", Value::Unsigned(frames.front().frameIndex) },
			{ "format", Value::String("MOV") },
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

	bool ReadFireEXRAttributes(
		const RISECBOR64::Bytes& encoded,
		std::map<std::string,std::string>& attributes,
		std::string& error )
	{
		attributes.clear();
		if( encoded.size() < 17u || ReadLE32(encoded,0u) != 20000630u ) {
			error = "artifact is not a supported OpenEXR byte stream";
			return false;
		}
		std::size_t cursor = 8u;
		while( cursor < encoded.size() ) {
			std::string name;
			if( !ReadCString(encoded,cursor,name) ) {
				error = "unterminated OpenEXR attribute name";
				return false;
			}
			if( name.empty() ) return true;
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
			if( name.compare(0u,std::strlen(kFireAttributePrefix),kFireAttributePrefix) == 0 ) {
				if( type != "string" || !attributes.emplace(name,std::string(
					reinterpret_cast<const char*>(&encoded[cursor]),size)).second ) {
					error = "fire provenance EXR attribute is not a unique string";
					return false;
				}
			}
			cursor += size;
		}
		error = "OpenEXR header terminator is missing";
		return false;
	}
}

bool RISE::Implementation::StripFireProvenanceEXRAttributes(
	const std::vector<unsigned char>& encoded,
	std::vector<unsigned char>& stripped,
	std::string& error )
{
	error.clear();
	stripped.clear();
	if( encoded.size() < 17u || ReadLE32(encoded,0u) != 20000630u ) {
		error = "artifact is not a supported OpenEXR byte stream";
		return false;
	}
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
	if( firstChunk < oldHeaderEnd || firstChunk > encoded.size() ||
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
		if( chunk < delta || chunk > encoded.size() ) {
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
	RISECBOR64::Bytes stripped;
	if( !StripFireProvenanceEXRAttributes(encodedArtifact,stripped,error) ||
		artifactDigest->GetText() != RISECBOR64::SHA256Hex(stripped) ) {
		if( error.empty() ) error = "fire artifact_sha256 does not hash the stripped EXR";
		return false;
	}
	std::map<std::string,std::string> actual;
	if( !ReadFireEXRAttributes(encodedArtifact,actual,error) ) return false;
	std::map<std::string,std::string> expected;
	for( std::size_t i=0; i<payload->GetMap().size(); ++i ) {
		std::string json;
		if( !CanonicalJSON(payload->GetMap()[i].second,json,error) ) return false;
		expected[std::string(kFireAttributePrefix)+payload->GetMap()[i].first] = json;
	}
	expected[std::string(kFireAttributePrefix)+"provenance_id"] =
		EscapeJSONString(provenanceId->GetText());
	if( actual != expected ) {
		error = "EXR fire-provenance attributes do not match the authoritative sidecar";
		return false;
	}
	return true;
}

bool RISE::Implementation::PublishFireFrameSequenceFileTransaction(
	const FrameStoreOutput::Metadata& metadata,
	const std::string& closedTemporaryArtifactFilename,
	const std::string& artifactFilename,
	const unsigned int width,
	const unsigned int height,
	const unsigned int framesPerSecond,
	const unsigned int encodedFrameCount,
	const std::vector<FireFramePrimary>& frames,
	std::string& error )
{
	std::lock_guard<std::mutex> transactionLock(gFileTransactionMutex);
	error.clear();
	if( encodedFrameCount != frames.size() ) {
		error = "encoded movie frame count does not match the primary-link array";
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
	RISECBOR64::Bytes sidecarBytes;
	if( !BuildFireFrameSequenceProvenance(metadata,
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

bool RISE::Implementation::EncodeFrameStoreFileTransaction(
	FrameStore& store,
	IFrameEncoder& encoder,
	const EncodeOpts& opts,
	const std::string& artifactFilename,
	std::string& error
	)
{
	std::lock_guard<std::mutex> transactionLock(gFileTransactionMutex);
	error.clear();
	EncodeOpts transactionOpts = opts;
	transactionOpts.metadataSnapshot = store.Meta();
	transactionOpts.useMetadataSnapshot = true;
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
			ArtifactReasons(encoder,transactionOpts,primary);
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

	// Build the filename — same templating as
	// FileRasterizerOutput.cpp:156-160.
	static const int MAX_BUFFER_SIZE = 2048;
	char filename[MAX_BUFFER_SIZE];
	if ( bMultiple_ ) {
		snprintf( filename, MAX_BUFFER_SIZE, "%s%s%.4u.%s",
			pattern_.c_str(), suffix, frame, ext.c_str() );
	} else {
		snprintf( filename, MAX_BUFFER_SIZE, "%s%s.%s",
			pattern_.c_str(), suffix, ext.c_str() );
	}

	EncodeOpts frameOpts = opts_;
	frameOpts.frame = frame;
	frameOpts.denoisedDerivative = denoisedDerivative;
	std::string writeError;
	if( !EncodeFrameStoreFileTransaction(*store_,*encoder_,frameOpts,filename,
		writeError) ) {
		if( !store_->Meta().renderFidelityStatus.empty() ) {
			GlobalLog()->PrintEx( eLog_Error,
				"FileEncoderObserver:: output_provenance_unavailable for '%s': %s",
				filename,writeError.c_str() );
			throw std::runtime_error("output_provenance_unavailable: "+writeError);
		}
		const FileEncoderObserver* pMe = this;
		char emergency[MAX_BUFFER_SIZE];
		if ( bMultiple_ ) {
			snprintf( emergency, MAX_BUFFER_SIZE,
				"fro_temp_%lu%s_%.4u.%s",
				static_cast<unsigned long>( reinterpret_cast<uintptr_t>( pMe ) ),
				suffix, frame, ext.c_str() );
		} else {
			snprintf( emergency, MAX_BUFFER_SIZE,
				"fro_temp_%lu%s.%s",
				static_cast<unsigned long>( reinterpret_cast<uintptr_t>( pMe ) ),
				suffix, ext.c_str() );
		}

		std::string emergencyError;
		if( !EncodeFrameStoreFileTransaction(*store_,*encoder_,frameOpts,emergency,
			emergencyError) ) {
			GlobalLog()->PrintEx( eLog_Error,
				"FileEncoderObserver:: artifact transaction failed for '%s' (%s); "
				"emergency transaction also failed for '%s' (%s)",
				filename,writeError.c_str(),emergency,emergencyError.c_str() );
			return;
		}
		GlobalLog()->PrintEx( eLog_Warning,
			"Artifact transaction failed for '%s' (%s); rendered scene written "
			"transactionally to emergency file '%s' instead!",
			filename,writeError.c_str(),emergency );
		// Use the emergency filename as the effective filename for
		// the success log message below.
		std::strncpy( filename, emergency, MAX_BUFFER_SIZE );
		filename[ MAX_BUFFER_SIZE - 1 ] = '\0';
	}

	GlobalLog()->PrintEx( eLog_Event,
		"FileEncoderObserver:: Written to '%s'", filename );
}
