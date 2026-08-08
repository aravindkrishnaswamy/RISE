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

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	RISECBOR64::Value TextArray( const std::vector<std::string>& values )
	{
		RISECBOR64::Value::Values encoded;
		for( std::size_t i=0; i<values.size(); ++i ) {
			encoded.push_back(RISECBOR64::Value::String(values[i]));
		}
		return RISECBOR64::Value::ArrayValue(encoded);
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

	bool BuildFireProvenanceSidecar(
		const FrameStore::Metadata& metadata,
		const char* artifactFilename,
		RISECBOR64::Bytes& encoded,
		std::string& error
		)
	{
		RISECBOR64::Bytes artifactBytes;
		if( !ReadArtifact(artifactFilename,artifactBytes) ) {
			error = "could not read the finalized artifact for hashing";
			return false;
		}
		using RISECBOR64::Value;
		const Value provenance = Value::MapValue({
			{ "active_fire_optics_record_ids",
				TextArray(metadata.activeFireOpticsRecordIds) },
			{ "artifact_sha256", Value::String(RISECBOR64::SHA256Hex(artifactBytes)) },
			{ "record_kind", Value::String("fire_output_provenance") },
			{ "render_fidelity_status", Value::String(metadata.renderFidelityStatus) },
			{ "render_reason_codes", TextArray(metadata.renderReasonCodes) },
			{ "schema_version", Value::Unsigned(1) }
		});
		return RISECBOR64::Encode(provenance,encoded,&error);
	}

	bool RemoveIfPresent( const std::string& filename, std::string& error )
	{
		errno = 0;
		if( std::remove(filename.c_str()) == 0 || errno == ENOENT ) return true;
		error = "could not replace '"+filename+"'";
		return false;
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
}

bool RISE::Implementation::EncodeFrameStoreFileTransaction(
	const FrameStore& store,
	IFrameEncoder& encoder,
	const EncodeOpts& opts,
	const std::string& artifactFilename,
	std::string& error
	)
{
	error.clear();
	EncodeOpts transactionOpts = opts;
	transactionOpts.metadataSnapshot = store.Meta();
	transactionOpts.useMetadataSnapshot = true;
	const std::string artifactTemporary = artifactFilename+".rise-tmp";
	const std::string sidecarFilename = artifactFilename+".provenance.cbor";
	const std::string sidecarTemporary = sidecarFilename+".rise-tmp";
	std::remove(artifactTemporary.c_str());
	std::remove(sidecarTemporary.c_str());

	DiskFileWriteBuffer* artifact = new DiskFileWriteBuffer(artifactTemporary.c_str());
	if( !artifact->ReadyToWrite() ) {
		error = "could not open the temporary artifact";
		safe_release(artifact);
		return false;
	}
	bool encoded = true;
	try {
		encoder.Encode(store,*artifact,transactionOpts);
	}
	catch( ... ) {
		encoded = false;
	}
	const bool artifactClosed = artifact->Close();
	safe_release(artifact);
	if( !encoded || !artifactClosed || !ArtifactHasBytes(artifactTemporary.c_str()) ) {
		std::remove(artifactTemporary.c_str());
		error = "artifact encoding produced no finalized bytes";
		return false;
	}

	const bool needsProvenance =
		!transactionOpts.metadataSnapshot.renderFidelityStatus.empty();
	if( needsProvenance ) {
		RISECBOR64::Bytes sidecarBytes;
		if( !BuildFireProvenanceSidecar(transactionOpts.metadataSnapshot,
			artifactTemporary.c_str(),
			sidecarBytes,error) ||
			!WriteClosedFile(sidecarTemporary,sidecarBytes,error) ||
			!RemoveIfPresent(artifactFilename,error) ||
			!RemoveIfPresent(sidecarFilename,error) ||
			std::rename(sidecarTemporary.c_str(),sidecarFilename.c_str()) != 0 ) {
			std::remove(artifactTemporary.c_str());
			std::remove(sidecarTemporary.c_str());
			if( error.empty() ) error = "could not commit the provenance sidecar";
			return false;
		}
	} else if( !RemoveIfPresent(artifactFilename,error) ||
		!RemoveIfPresent(sidecarFilename,error) ) {
		std::remove(artifactTemporary.c_str());
		return false;
	}

	if( std::rename(artifactTemporary.c_str(),artifactFilename.c_str()) != 0 ) {
		std::remove(artifactTemporary.c_str());
		if( needsProvenance ) std::remove(sidecarFilename.c_str());
		if( error.empty() ) error = "could not commit the encoded artifact";
		return false;
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
	WriteFile( frame, "_denoised" );
}

void FileEncoderObserver::WriteFile( unsigned int frame, const char* suffix )
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

	std::string writeError;
	if( !EncodeFrameStoreFileTransaction(*store_,*encoder_,opts_,filename,
		writeError) ) {
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
		if( !EncodeFrameStoreFileTransaction(*store_,*encoder_,opts_,emergency,
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
