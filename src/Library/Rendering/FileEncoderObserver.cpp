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
	if ( !store_ || !encoder_ || store_->Width() == 0 || store_->Height() == 0 ) return;

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

	// Stage each frame beside its destination. If staging is unavailable,
	// allow the historical direct-write path for writable destinations.
	// Publication failures retain completed staging bytes. Other failures
	// may try a new exclusive emergency file without trusting an old candidate.
	const auto encode = [&]( DiskFileWriteBuffer& buffer ) {
		encoder_->Encode( *store_, buffer, opts_ );
		return true;
	};
	DiskFileWriteBuffer::WriteDiagnostics primary;
	if( !DiskFileWriteBuffer::WriteAtomically(filename, encode, DiskFileWriteBuffer::StagingPolicy::AllowInPlace, &primary) ) {
		if( !primary.recoveryPath.empty() ) {
			GlobalLog()->PrintEx(eLog_Warning, "Could not publish '%s'; completed image retained at '%s'",
				filename.c_str(), primary.recoveryPath.c_str());
			return;
		}
		const std::string emergency = "fro_temp_" +
			std::to_string( reinterpret_cast<uintptr_t>( this ) ) + suffix +
			(bMultiple_ ? "_" : "") + frameNumber + "." + ext;
		GlobalLog()->PrintEx( eLog_Warning,
			"Could not publish '%s', trying emergency file '%s'",
			filename.c_str(), emergency.c_str() );
		DiskFileWriteBuffer::WriteDiagnostics recovery;
		if( !DiskFileWriteBuffer::WriteNewFile(emergency, encode, &recovery) ) {
			GlobalLog()->PrintEx(eLog_Error, "FileEncoderObserver: Could not write emergency file '%s': %s",
				emergency.c_str(), recovery.error.message().c_str());
			return;
		}
		filename = recovery.recoveryPath;
	}
	GlobalLog()->PrintEx( eLog_Event, "FileEncoderObserver:: Written to '%s'", filename.c_str() );
}
