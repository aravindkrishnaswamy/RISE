//////////////////////////////////////////////////////////////////////
//
//  IFrameEncoder.h - Interface for FrameStore-aware byte encoders.
//
//  Decouples "render produced this HDR FrameStore" from "save it as
//  PNG / EXR / TIFF / etc." — the existing IRasterImageWriter
//  hierarchy handles per-format byte production, but each is locked
//  to a specific construction-time set of options inside
//  FileRasterizerOutput.  IFrameEncoder makes "pick a format at
//  runtime, encode the current FrameStore" a first-class operation:
//  the GUI's Save-As menu, scene-declared file outputs, future
//  network mirrors, and Phase-2 multi-channel EXR all use this.
//
//  Encoders are NOT rasterizer-output sinks (those still implement
//  IRasterizerOutput in Phase 1).  Encoders are stateless byte
//  producers: given a FrameStore + EncodeOpts, write to an
//  IWriteBuffer.  Lifetime: encoders are registered into
//  FrameEncoderRegistry::Get() and live for the program's lifetime
//  (refcounted via Reference).
//
//  Byte-identity contract: for the same FrameStore beauty channel,
//  same EncodeOpts, and same DisplayTransform parameters, the
//  output of an IFrameEncoder MUST match what the legacy
//  FileRasterizerOutput::WriteImageToFile pipeline produces for the
//  same scene-file output declaration.  This is the L2/L3 regression
//  gate; FrameStoreEncoderTest verifies it for every concrete encoder.
//
//  Author: design landing L2
//  License: see LICENSE.TXT
//
//////////////////////////////////////////////////////////////////////

#ifndef IFRAMEENCODER_
#define IFRAMEENCODER_

#include <string>
#include <vector>
#include <utility>

#include "IReference.h"
#include "IWriteBuffer.h"
#include "../Utilities/Color/Color.h"
#include "../Rendering/Channel.h"
#include "../Rendering/ViewTransform.h"
#include "../Rendering/TargetFormat.h"
#include "../Rendering/FrameStore.h"
#include "../RasterImages/EXRCompression.h"

namespace RISE
{

	//! Encoding options.  All fields have sensible defaults; encoders
	//! pull only the keys they care about and ignore the rest.
	//!
	//! Adding a new format-specific field is preferred over the
	//! string-keyed `attrs` map because it keeps the API
	//! type-checked and avoids silent typos.  Bloat is bounded —
	//! the struct is ~80 bytes today and only grows on new format
	//! additions.
	struct EncodeOpts
	{
		//! Display transform applied before the format-specific
		//! transfer function.  HDR archival formats (EXR, .hdr,
		//! RGBEA) ignore this — see encoder docs.  Default identity
		//! (no exposure, no tone curve).
		FrameStoreOutput::ViewTransform viewTransform;

		//! Output color space written to the file.  Maps to the
		//! existing COLOR_SPACE enum used by writers.
		COLOR_SPACE colorSpace = eColorSpace_sRGB;

		//! Bits-per-channel for formats that support it (PNG: 8
		//! or 16; TIFF: 8 or 16; rest: ignored).
		unsigned char bpp = 8;

		//! EXR-specific knobs (ignored for other formats).
		EXR_COMPRESSION exrCompression = eExrCompression_Piz;
		bool            exrWithAlpha   = true;

		//! Animation frame index — passed to file consumers that
		//! template filenames; the encoder itself doesn't use it.
		unsigned frame = 0;

		//! AOV inclusion for multi-channel formats (L7+).
		//! Currently unused by all 7 base encoders; reserved.
		bool                                    includeAOVs = false;
		std::vector<FrameStoreOutput::ChannelId> aovChannels;

		//! Compression level (format-specific; -1 = format default).
		//! PNG uses 0..9; not consulted today since the existing
		//! PNGWriter has its own internal default.
		int compressionLevel = -1;

		//! Free-form metadata (EXR custom attrs, PNG iTXt, etc.).
		//! Reserved for future use; current encoders ignore it.
		std::vector<std::pair<std::string, std::string>> attrs;
	};

	//! Stateless format-aware byte encoder.  Wraps an existing
	//! IRasterImageWriter behind a FrameStore-friendly facade.
	class IFrameEncoder : public virtual IReference
	{
	protected:
		IFrameEncoder() {}
		virtual ~IFrameEncoder() {}

	public:
		//! Format identifier ("PNG", "EXR", "TIFF", "HDR",
		//! "RGBEA", "TGA", "PPM").  Used by registry lookups
		//! and UI save-as dropdowns.
		virtual std::string FormatName() const = 0;

		//! File extensions WITHOUT leading dot ("png", "exr",
		//! "tif"/"tiff").  First entry is the canonical default.
		virtual std::vector<std::string> Extensions() const = 0;

		//! True if the format preserves > 1.0 linear values
		//! (EXR, .hdr, RGBEA).  Used by Save-As dialogs to warn
		//! when an HDR scene would be clipped by an LDR target.
		virtual bool SupportsHDR() const = 0;

		//! True if the format can carry AOV channels alongside
		//! beauty (multichannel EXR).  Reserved; all current
		//! encoders return false.
		virtual bool SupportsAOVs() const = 0;

		//! Encode the FrameStore's beauty + alpha channels into
		//! `dst` using the per-format byte production rules and
		//! the supplied options.  Throws no exceptions: write
		//! errors are reported via the underlying IWriteBuffer
		//! (which already has its own error semantics).
		//!
		//! The encoder may NOT take ownership of `dst` — caller
		//! retains the IWriteBuffer reference.  Caller is also
		//! responsible for ensuring no concurrent writes to the
		//! FrameStore during the call (typical pattern: invoke
		//! from IRenderObserver::OnFrameComplete, which fires
		//! after the rasterizer has stopped writing).
		virtual void Encode(
			const FrameStore&    store,
			IWriteBuffer&        dst,
			const EncodeOpts&    opts ) = 0;
	};
}

#endif
