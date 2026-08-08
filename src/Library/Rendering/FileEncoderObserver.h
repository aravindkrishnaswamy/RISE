//////////////////////////////////////////////////////////////////////
//
//  FileEncoderObserver.h - IRenderObserver that, on each
//  OnFrameComplete / OnPreDenoiseComplete / OnDenoiseComplete
//  callback, transactionally writes an IFrameEncoder artifact and
//  its required provenance sidecar from the FrameStore.
//
//  This is the L3 counterpart to L2's IFrameEncoder: L2 produced
//  bytes from a FrameStore + EncodeOpts, while this class wires
//  those bytes to a disk file (matching the legacy
//  FileRasterizerOutput::WriteImageToFile filename templating
//  and "_denoised" suffix conventions).
//
//  Filename layout matches FileRasterizerOutput.cpp:156-160:
//    bMultiple == true   →  "<pattern><suffix>NNNN.<ext>"
//    bMultiple == false  →  "<pattern><suffix>.<ext>"
//
//  The observer retains both the IFrameEncoder and FrameStore so they
//  survive registry replacement and rasterizer/Job teardown.
//
//  Author: design landing L3
//  License: see LICENSE.TXT
//
//////////////////////////////////////////////////////////////////////

#ifndef FILEENCODEROBSERVER_H_
#define FILEENCODEROBSERVER_H_

#include <string>

#include "../Interfaces/IRenderObserver.h"
#include "../Interfaces/IFrameEncoder.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class FrameStore;

		//! Encode to temporary files and publish the artifact only after its
		//! required fire-provenance sidecar has been finalized.  On failure no
		//! artifact remains without its matching sidecar.
		bool EncodeFrameStoreFileTransaction(
			const FrameStore& store,
			IFrameEncoder& encoder,
			const EncodeOpts& opts,
			const std::string& artifactFilename,
			std::string& error
			);

		class FileEncoderObserver : public virtual IRenderObserver,
		                            public virtual Reference
		{
		public:
			//! @param store        FrameStore the observer reads
			//!                     from on each callback.  Caller's
			//!                     reference is addref'd internally.
			//! @param encoder      Format encoder (typically from
			//!                     FrameEncoderRegistry).  Retained.
			//! @param opts         Encoder options.  Copied into
			//!                     the observer (caller may mutate
			//!                     their copy after construction).
			//! @param filenamePattern  Filename without extension or
			//!                     frame-number suffix.  E.g. for
			//!                     pattern "out" / type PNG /
			//!                     bMultiple=true / frame 7 the
			//!                     written file is "out0007.png".
			//! @param bMultiple    If true, embed the 4-digit frame
			//!                     number in the filename (animation
			//!                     mode).  Mirrors FileRasterizerOutput.
			FileEncoderObserver(
				FrameStore*         store,
				IFrameEncoder*      encoder,
				const EncodeOpts&   opts,
				const std::string&  filenamePattern,
				bool                bMultiple );

			// IRenderObserver
			void OnFrameComplete( unsigned int frame, uint64_t generation ) override;
			void OnPreDenoiseComplete( unsigned int frame, uint64_t generation ) override;
			void OnDenoiseComplete( unsigned int frame, uint64_t generation ) override;

		protected:
			virtual ~FileEncoderObserver();

		private:
			void WriteFile( unsigned int frame, const char* suffix );

			FrameStore*    store_;     // addref'd in ctor
			IFrameEncoder* encoder_;   // addref'd in ctor
			EncodeOpts     opts_;
			std::string    pattern_;
			bool           bMultiple_;
		};
	}
}

#endif
