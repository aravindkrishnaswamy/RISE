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
#include <vector>

#include "../Interfaces/IRenderObserver.h"
#include "../Interfaces/IFrameEncoder.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class FrameStore;

		struct FireFramePrimary
		{
			unsigned int frameIndex = 0;
			std::string provenanceId;
			std::string artifactSha256;
		};

		enum class FireFrameSequenceEncoding
		{
			AppleProRes4444_12Bit,
			AppleProRes4444_10Bit,
			HevcMain10_10Bit
		};

		typedef bool (*FireFrameSequenceArtifactValidator)(
			const std::string& closedArtifactFilename,
			FireFrameSequenceEncoding encoding,
			unsigned int width,
			unsigned int height,
			unsigned int framesPerSecond,
			const std::vector<FireFramePrimary>& frames,
			std::string& error
			);

		struct FireFrameSequenceEncodingDescriptor
		{
			unsigned int schemaVersion = 1u;
			std::string backend;
			std::string containerFormat;
			std::string codec;
			std::string codecImplementation;
			std::string codecProfile;
			unsigned int bitsPerChannel = 0u;
			std::string inputPixelFormat;
			std::string outputPixelFormat;
			std::string chromaSubsampling;
			std::string alphaMode;
			std::string colorRange;
			std::string colorPrimaries;
			std::string transferFunction;
			std::string ycbcrMatrix;
			std::string displayTransform;
			unsigned int referenceWhiteNits = 0u;
			unsigned int pqPeakNits = 0u;
			std::string dimensionRounding;
			unsigned int maxBFrames = 0u;
			unsigned int gopFrames = 0u;
			std::string rateControl;
			std::string encoderPreset;
			std::string codecOptions;
			std::string codecTag;
			std::string muxerFlags;
			std::string conversionFilter;
			std::string conversionMatrix;
			std::string conversionSourceRange;
			std::string conversionDestinationRange;
			int conversionBrightness = 0;
			int conversionContrast = 0;
			int conversionSaturation = 0;
			bool expectsMediaDataInRealTime = false;
		};

		//! Returns the versioned, complete render-affecting parameter surface
		//! for a movie encoding.  This descriptor is shared by preflight and
		//! provenance so an encoder-setting change cannot remain unattested.
		bool DescribeFireFrameSequenceEncoding(
			FireFrameSequenceEncoding encoding,
			unsigned int framesPerSecond,
			FireFrameSequenceEncodingDescriptor& descriptor,
			std::string& error
			);

		bool ValidateFireFrameSequenceEncodingDescriptor(
			FireFrameSequenceEncoding encoding,
			unsigned int framesPerSecond,
			const FireFrameSequenceEncodingDescriptor& descriptor,
			std::string& error
			);

		//! Encode to temporary files and publish the artifact only after its
		//! required fire-provenance sidecar has been finalized.  On failure no
		//! artifact remains without its matching sidecar.
		bool EncodeFrameStoreFileTransaction(
			FrameStore& store,
			IFrameEncoder& encoder,
			const EncodeOpts& opts,
			const std::string& artifactFilename,
			std::string& error
			);

		using FileTransactionContentionHook = void (*)(void* context);
		//! Test instrumentation: observes an actual failed try-lock before a
		//! file transaction blocks on the global publication mutex. Null clears.
		void SetFileTransactionContentionHookForTests(
			FileTransactionContentionHook hook,
			void* context
			);

		//! Publish an already-closed display movie and its authoritative
		//! frame-sequence provenance sidecar as one artifact transaction.
		//! The temporary movie is consumed on success and removed on failure.
		//! Publication requires the authored backend to decode every frame and
		//! bind the finalized container facts to the supplied expectations.
		bool PublishFireFrameSequenceFileTransaction(
			const FrameStoreOutput::Metadata& metadata,
			FireFrameSequenceEncoding encoding,
			const std::string& closedTemporaryArtifactFilename,
			const std::string& artifactFilename,
			unsigned int width,
			unsigned int height,
			unsigned int framesPerSecond,
			unsigned int encodedFrameCount,
			const std::vector<FireFramePrimary>& frames,
			FireFrameSequenceArtifactValidator validateArtifact,
			std::string& error
			);

		//! Publish an artifact that carries no fire provenance while
		//! transactionally retiring any sidecar left by the prior artifact.
		//! The previous artifact/sidecar pair is restored if publication fails.
		bool PublishUnprovenancedFileTransaction(
			const std::string& closedTemporaryArtifactFilename,
			const std::string& artifactFilename,
			std::string& error
			);

		//! Deterministically removes all riseFireProv_ string attributes from
		//! a single-part scanline EXR and rebases its chunk-offset table.
		//! The resulting bytes are the artifact_sha256 preimage pinned by P-4.
		bool StripFireProvenanceEXRAttributes(
			const std::vector<unsigned char>& encoded,
			std::vector<unsigned char>& stripped,
			std::string& error
			);

		//! Verifies the authoritative envelope, its one-preimage ID, the
		//! attribute-stripped artifact digest, and every mirrored EXR field.
		bool VerifyFireProvenanceEXR(
			const std::vector<unsigned char>& encodedArtifact,
			const std::vector<unsigned char>& encodedSidecar,
			std::string& error
			);

		std::string BuildFrameArtifactFilename(
			const std::string& pattern,
			const std::string& suffix,
			unsigned int frame,
			const std::string& extension,
			bool multiple
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
			void WriteFile( unsigned int frame, const char* suffix,
				bool denoisedDerivative = false );

			FrameStore*    store_;     // addref'd in ctor
			IFrameEncoder* encoder_;   // addref'd in ctor
			EncodeOpts     opts_;
			std::string    pattern_;
			bool           bMultiple_;
		};
	}
}

#endif
