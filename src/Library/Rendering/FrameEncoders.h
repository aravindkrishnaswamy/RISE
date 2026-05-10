//////////////////////////////////////////////////////////////////////
//
//  FrameEncoders.h - Concrete IFrameEncoder implementations for the
//  7 file formats RISE has historically supported, plus the
//  FrameEncoderRegistry singleton that hosts them.
//
//  Each concrete encoder is a thin wrapper over an existing
//  IRasterImageWriter (PNGWriter, EXRWriter, etc.).  The Encode
//  method:
//    1. Constructs the format-specific writer via RISE_API_Create*Writer.
//    2. Optionally wraps it in DisplayTransformWriter for LDR
//       formats with a non-trivial ViewTransform (matching the
//       legacy FileRasterizerOutput::WriteImageToFile gate).
//    3. Calls store.AsBeautyRasterImage().DumpImage(effectiveWriter)
//       which walks pixels in the same row-major order as
//       RasterImage_Template::DumpImage — guaranteeing byte-identical
//       output to the legacy pipeline (the L2 regression gate).
//    4. Releases the writers (and wrapper) via the existing
//       Reference/safe_release machinery.
//
//  HDR formats (EXR, .hdr, RGBEA) explicitly skip the
//  DisplayTransform wrap, mirroring the IsHDRFormat guard in
//  FileRasterizerOutput.cpp:94.
//
//  The 7 encoders share enough plumbing that a small base
//  class — FrameEncoderBase — owns the "construct, wrap, dump,
//  cleanup" sequence; concrete subclasses only need to override
//  CreateWriter() to invoke the right RISE_API_Create*Writer factory
//  for their format.
//
//  Author: design landing L2
//  License: see LICENSE.TXT
//
//////////////////////////////////////////////////////////////////////

#ifndef FRAMEENCODERS_H_
#define FRAMEENCODERS_H_

#include <cassert>
#include <mutex>
#include <string>
#include <vector>

#include "../Interfaces/IFrameEncoder.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		// ─────────────────────────────────────────────────────────────
		// Common base for all 7 concrete encoders.
		// ─────────────────────────────────────────────────────────────
		class FrameEncoderBase : public virtual IFrameEncoder,
		                         public virtual Reference
		{
		public:
			void Encode(
				const FrameStore&    store,
				IWriteBuffer&        dst,
				const EncodeOpts&    opts ) override;

		protected:
			virtual ~FrameEncoderBase() {}

			//! Subclass hook: instantiate the format-specific
			//! writer.  Caller addref on the returned writer is
			//! the writer-factory's responsibility (matching
			//! existing RISE_API_Create*Writer behaviour).  Return
			//! nullptr on failure.
			virtual IRasterImageWriter* CreateWriter(
				IWriteBuffer&     dst,
				const EncodeOpts& opts ) const = 0;

			//! True if this format should bypass the
			//! DisplayTransformWriter wrap regardless of opts.
			//! Mirrors FileRasterizerOutput::IsHDRFormat.
			virtual bool IsHDRFormat() const { return false; }
		};

		// ─────────────────────────────────────────────────────────────
		// Concrete encoders (declarations)
		// ─────────────────────────────────────────────────────────────

		class PNGFrameEncoder : public FrameEncoderBase
		{
		public:
			std::string FormatName() const override { return "PNG"; }
			std::vector<std::string> Extensions() const override { return { "png" }; }
			bool SupportsHDR() const override  { return false; }
			bool SupportsAOVs() const override { return false; }
		protected:
			virtual ~PNGFrameEncoder() {}
			IRasterImageWriter* CreateWriter( IWriteBuffer& dst, const EncodeOpts& opts ) const override;
		};

		class EXRFrameEncoder : public FrameEncoderBase
		{
		public:
			std::string FormatName() const override { return "EXR"; }
			std::vector<std::string> Extensions() const override { return { "exr" }; }
			bool SupportsHDR() const override  { return true; }
			bool SupportsAOVs() const override { return false; }  // L7 will flip this
		protected:
			virtual ~EXRFrameEncoder() {}
			IRasterImageWriter* CreateWriter( IWriteBuffer& dst, const EncodeOpts& opts ) const override;
			bool IsHDRFormat() const override { return true; }
		};

		class TIFFFrameEncoder : public FrameEncoderBase
		{
		public:
			std::string FormatName() const override { return "TIFF"; }
			std::vector<std::string> Extensions() const override { return { "tiff", "tif" }; }
			bool SupportsHDR() const override  { return false; }
			bool SupportsAOVs() const override { return false; }
		protected:
			virtual ~TIFFFrameEncoder() {}
			IRasterImageWriter* CreateWriter( IWriteBuffer& dst, const EncodeOpts& opts ) const override;
		};

		class HDRFrameEncoder : public FrameEncoderBase
		{
		public:
			std::string FormatName() const override { return "HDR"; }
			std::vector<std::string> Extensions() const override { return { "hdr" }; }
			bool SupportsHDR() const override  { return true; }
			bool SupportsAOVs() const override { return false; }
		protected:
			virtual ~HDRFrameEncoder() {}
			IRasterImageWriter* CreateWriter( IWriteBuffer& dst, const EncodeOpts& opts ) const override;
			bool IsHDRFormat() const override { return true; }
		};

		class RGBEAFrameEncoder : public FrameEncoderBase
		{
		public:
			std::string FormatName() const override { return "RGBEA"; }
			std::vector<std::string> Extensions() const override { return { "rgbea" }; }
			bool SupportsHDR() const override  { return true; }
			bool SupportsAOVs() const override { return false; }
		protected:
			virtual ~RGBEAFrameEncoder() {}
			IRasterImageWriter* CreateWriter( IWriteBuffer& dst, const EncodeOpts& opts ) const override;
			bool IsHDRFormat() const override { return true; }
		};

		class TGAFrameEncoder : public FrameEncoderBase
		{
		public:
			std::string FormatName() const override { return "TGA"; }
			std::vector<std::string> Extensions() const override { return { "tga" }; }
			bool SupportsHDR() const override  { return false; }
			bool SupportsAOVs() const override { return false; }
		protected:
			virtual ~TGAFrameEncoder() {}
			IRasterImageWriter* CreateWriter( IWriteBuffer& dst, const EncodeOpts& opts ) const override;
		};

		class PPMFrameEncoder : public FrameEncoderBase
		{
		public:
			std::string FormatName() const override { return "PPM"; }
			std::vector<std::string> Extensions() const override { return { "ppm" }; }
			bool SupportsHDR() const override  { return false; }
			bool SupportsAOVs() const override { return false; }
		protected:
			virtual ~PPMFrameEncoder() {}
			IRasterImageWriter* CreateWriter( IWriteBuffer& dst, const EncodeOpts& opts ) const override;
		};

		// L5c — HDR10 PNG encoder.  Renders the FrameStore directly
		// to RGB16_BT2020_PQ (16-bit fixed BT.2020 primaries +
		// SMPTE ST.2084 PQ transfer) and emits a 16-bit RGB PNG with
		// a `cICP` chunk declaring the HDR10 colour space (BT.2020
		// primaries / PQ transfer / RGB matrix / full range).  Modern
		// browsers and HDR-aware viewers (Chrome/Edge 100+, macOS
		// Preview on HDR displays) honor cICP and tone-map the
		// content to the display's HDR capabilities.  Older viewers
		// without cICP support display the raw PQ-encoded uint16
		// values, which look washed-out on SDR — the user-facing
		// expectation is "view in an HDR-aware app".
		//
		// This encoder OVERRIDES `Encode` (rather than
		// `CreateWriter`) because the HDR10 path bypasses the
		// legacy `IRasterImageWriter`-via-RISEColor-per-pixel
		// pipeline (which goes through the COLOR_SPACE legacy enum
		// that doesn't have BT.2020 + PQ entries) in favour of the
		// L1+ `FrameStore::Render` polymorphic readback into a
		// `uint16` buffer + libpng directly.
		//
		// File extension is "png" (overlaps with PNGFrameEncoder);
		// users select via FormatName "HDR10_PNG" rather than
		// extension lookup.  ByExtension("png") still returns
		// PNGFrameEncoder (the SDR default).  The bridges' Save-As
		// dropdown can list both.
		class HDR10PNGFrameEncoder : public FrameEncoderBase
		{
		public:
			std::string FormatName() const override { return "HDR10_PNG"; }
			std::vector<std::string> Extensions() const override { return { "png" }; }
			bool SupportsHDR() const override  { return true; }
			bool SupportsAOVs() const override { return false; }

			// Override Encode (rather than CreateWriter) — see class
			// doc for why the legacy IRasterImageWriter path doesn't
			// fit HDR10's BT.2020 + PQ requirements.
			void Encode(
				const FrameStore&    store,
				IWriteBuffer&        dst,
				const EncodeOpts&    opts ) override;

		protected:
			virtual ~HDR10PNGFrameEncoder() {}
			// L5c review P1 — `Encode` is overridden directly so the
			// base's `CreateWriter`-driven path is never reached.
			// Assert (rather than silently return null) so future
			// refactors that accidentally call back through the base
			// pipeline crash loudly in debug.
			IRasterImageWriter* CreateWriter( IWriteBuffer& /*dst*/, const EncodeOpts& /*opts*/ ) const override
			{
				assert( false && "HDR10PNGFrameEncoder::CreateWriter unreachable — "
				                  "L5c overrides Encode directly; do not call CreateWriter" );
				return nullptr;
			}
			bool IsHDRFormat() const override { return true; }
		};

		// ─────────────────────────────────────────────────────────────
		// Registry
		// ─────────────────────────────────────────────────────────────

		//! Singleton registry of IFrameEncoder instances.  The 7
		//! built-in encoders are registered lazily on first access.
		//! Lookup by format name ("PNG") or extension ("png") is
		//! case-insensitive on the input.
		class FrameEncoderRegistry
		{
		public:
			static FrameEncoderRegistry& Get();

			//! Register an encoder.  Caller transfers a reference
			//! (registry will release on shutdown / unregister).
			//! If an encoder with the same FormatName is already
			//! registered, the old one is released and replaced.
			void Register( IFrameEncoder* encoder );

			//! Lookup by format name.  Returns nullptr if not found.
			//! The returned pointer's lifetime is bounded by the
			//! registry; callers wishing to outlive the registry
			//! should addref.
			IFrameEncoder* ByFormatName( const std::string& name ) const;

			//! Lookup by file extension (with or without leading dot).
			IFrameEncoder* ByExtension( const std::string& ext ) const;

			//! Returns all registered encoders, in registration order.
			std::vector<IFrameEncoder*> All() const;

			// Non-copyable, non-movable singleton.
			FrameEncoderRegistry( const FrameEncoderRegistry& )            = delete;
			FrameEncoderRegistry& operator=( const FrameEncoderRegistry& ) = delete;

		private:
			FrameEncoderRegistry();
			~FrameEncoderRegistry();

			// Registers the 7 built-in encoders.  Called from
			// the constructor (Meyers-singleton timing).
			void RegisterBuiltins();

			mutable std::mutex             mutex_;
			std::vector<IFrameEncoder*>    encoders_;  // refcounted entries
		};
	}

	using Implementation::FrameEncoderRegistry;
}

#endif
