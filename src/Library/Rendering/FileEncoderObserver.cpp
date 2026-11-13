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

	// Animated output has a minimum of four digits for the frame number.
	std::string frameNumber;
	if ( bMultiple_ ) {
		frameNumber = std::to_string( frame );
		if( frameNumber.size() < 4 ) frameNumber.insert( 0, 4 - frameNumber.size(), '0' );
	}
	std::string filename = pattern_ + suffix + frameNumber + "." + ext;

	// Open the disk file.  If the open fails, mirror the legacy
	// "fall back to fro_temp_…" emergency-file behaviour from
	// FileRasterizerOutput.cpp:167-186 — we don't want to lose
	// the rendered data when a path is unwritable.
	DiskFileWriteBuffer* buf = new DiskFileWriteBuffer( filename.c_str() );

	if ( !buf->ReadyToWrite() ) {
		safe_release( buf );

		const std::string emergency = "fro_temp_" +
			std::to_string( reinterpret_cast<uintptr_t>( this ) ) + suffix +
			(bMultiple_ ? "_" : "") + frameNumber + "." + ext;

		buf = new DiskFileWriteBuffer( emergency.c_str() );
		if ( !buf->ReadyToWrite() ) {
			GlobalLog()->PrintEasyError(
				"FileEncoderObserver:: Fatal error trying to write image, "
				"couldn't even write the emergency file!" );
			safe_release( buf );
			return;
		}
		GlobalLog()->PrintEx( eLog_Warning,
			"Failed to open specified file '%s', trying emergency file '%s'",
			filename.c_str(), emergency.c_str() );
		filename = emergency;
	}

	GlobalLog()->PrintNew( buf, __FILE__, __LINE__, "DiskFileWriteBuffer" );

	encoder_->Encode( *store_, *buf, opts_ );

	const bool written = buf->Finish( true );
	safe_release( buf );

	if( written ) {
		GlobalLog()->PrintEx( eLog_Event,
			"FileEncoderObserver:: Written to '%s'", filename.c_str() );
	}
}
