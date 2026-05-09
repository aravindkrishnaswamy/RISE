//////////////////////////////////////////////////////////////////////
//
//  FileEncoderObserver.cpp - Implementation.  Each callback opens
//  a fresh DiskFileWriteBuffer (matching the legacy per-frame open
//  pattern from FileRasterizerOutput::WriteImageToFile), runs the
//  encoder, releases the buffer.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FileEncoderObserver.h"
#include "FrameStore.h"

#include "../Utilities/DiskFileWriteBuffer.h"
#include "../Interfaces/ILog.h"

#include <cstdio>
#include <cstring>

using namespace RISE;
using namespace RISE::Implementation;

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
}

FileEncoderObserver::~FileEncoderObserver()
{
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

	// Open the disk file.  If the open fails, mirror the legacy
	// "fall back to fro_temp_…" emergency-file behaviour from
	// FileRasterizerOutput.cpp:167-186 — we don't want to lose
	// the rendered data when a path is unwritable.
	DiskFileWriteBuffer* buf = new DiskFileWriteBuffer( filename );

	if ( !buf->ReadyToWrite() ) {
		safe_release( buf );

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

		buf = new DiskFileWriteBuffer( emergency );
		if ( !buf->ReadyToWrite() ) {
			GlobalLog()->PrintEasyError(
				"FileEncoderObserver:: Fatal error trying to write image, "
				"couldn't even write the emergency file!" );
			safe_release( buf );
			return;
		}
		GlobalLog()->PrintEx( eLog_Warning,
			"Failed to open specified file '%s', rendered scene written "
			"to emergency file '%s' instead!", filename, emergency );
		// Use the emergency filename as the effective filename for
		// the success log message below.
		std::strncpy( filename, emergency, MAX_BUFFER_SIZE );
		filename[ MAX_BUFFER_SIZE - 1 ] = '\0';
	}

	GlobalLog()->PrintNew( buf, __FILE__, __LINE__, "DiskFileWriteBuffer" );

	encoder_->Encode( *store_, *buf, opts_ );

	safe_release( buf );

	GlobalLog()->PrintEx( eLog_Event,
		"FileEncoderObserver:: Written to '%s'", filename );
}
