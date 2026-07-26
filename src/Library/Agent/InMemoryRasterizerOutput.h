//////////////////////////////////////////////////////////////////////
//
//  InMemoryRasterizerOutput.h - an IRasterizerOutput that captures the
//    final rasterized image into memory and serializes it to an 8-bit
//    PNG byte buffer (Facet 5, the agentic surface -- slice 0b).  sRGB
//    by default; SetOutputColorSpace (external review P2 fix) selects
//    any COLOR_SPACE the underlying PNGWriter supports, matching a
//    scene's declared `file_rasterizeroutput` `color_space`.
//
//    The agent's `render` / `read_image` verbs need the head's rendered
//    image WITHOUT touching the filesystem: no `file_rasterizeroutput`,
//    no RISE_MEDIA_PATH, no temp file to read back.  It captures the FINAL
//    image (the denoised final when OIDN is on: OutputDenoisedImage's
//    default forwards to OutputImage, so the denoised frame is what lands
//    here).  This sink holds the last full `OutputImage` in memory as
//    RISEColor pixels and, on
//    demand, encodes them to PNG bytes REUSING the tree's existing
//    encode path -- `PNGWriter` (which does the sRGB `Integerize` gamma
//    encode + libpng container) writing into an in-memory `MemoryBuffer`
//    instead of a file.  We deliberately do NOT hand-roll gamma or the
//    PNG container: `ToPng()` produces the SAME bytes a `png_painter` /
//    file PNG output would, just targeting memory.
//
//    Slice 0b is HEADLESS + single-threaded: no window, no thread-safety
//    (the rasterizer calls OutputImage serially on the driving thread and
//    the render has fully returned before ToPng() is called).  Region /
//    intermediate updates are ignored -- only the final full image is
//    retained, matching FileRasterizerOutput's "entire image is dumped"
//    contract.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_AGENT_INMEMORYRASTERIZEROUTPUT_
#define RISE_AGENT_INMEMORYRASTERIZEROUTPUT_

#include <vector>
#include <cstdint>

#include "../Interfaces/IRasterizerOutput.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Color/Color_Template.h"   // RISEColor

namespace RISE
{
	namespace Agent
	{
		//! Captures the final rasterized image into memory and serializes
		//! it to an 8-bit sRGB PNG byte buffer.  A reference-counted
		//! IRasterizerOutput so it can be handed to
		//! `IRasterizer::AddRasterizerOutput` like any other sink; the
		//! caller releases it after the render (see AgentSession::Render).
		class InMemoryRasterizerOutput
			: public virtual IRasterizerOutput
			, public virtual Implementation::Reference
		{
		public:
			struct PerceptionInfo
			{
				bool available = false;
				unsigned int sourceWidth = 0;
				unsigned int sourceHeight = 0;
				unsigned int validDepthPixels = 0;
				double depthMin = 0.0;
				double depthMax = 0.0;
				std::uint64_t persistentBytes = 0;
				std::uint64_t auxiliaryPeakBytes = 0;
				std::uint64_t encoderRowBytes = 0;
			};

			InMemoryRasterizerOutput();

			//! Captures the full final image (region ignored -- file-style
			//! "whole image" semantics).  Overwrites any previously-captured
			//! frame, so after a render `mPixels` holds the last frame.
			void OutputImage( const IRasterImage& pImage, const Rect* pRegion, const unsigned int frame ) override;

			//! Intermediate scanlines are not retained (headless final-image
			//! sink; matches FileRasterizerOutput ignoring intermediates).
			void OutputIntermediateImage( const IRasterImage& pImage, const Rect* pRegion ) override;

			//! Bind the canonical store whose optional AOVs accompany the
			//! next OutputImage callback.  Capture is compacted during that
			//! callback, before an agent render restores the display store.
			void OnRasterizerFrameStoreChanged( Implementation::FrameStore* framestore ) override;

			//! Toolkit slice 1 (read_viewport): adopt an ALREADY-COHERENT
			//! pixel buffer produced OUT-OF-BAND (the caller performed a
			//! tile-locked, complete-snapshot copy of the live interactive
			//! FrameStore via `SceneEditController::CopyInteractiveFrame`).
			//! This BYPASSES the `OutputImage` / `GetPEL` capture path
			//! entirely -- the caller already owns a row-major linear buffer,
			//! so there is nothing to re-read.  Post-state is IDENTICAL to a
			//! completed `OutputImage` (mPixels row-major linear, mWidth/
			//! mHeight set, mHasImage true), so the existing `ToPng()` /
			//! `ToPngDownscaled()` encode paths work unchanged with ZERO
			//! duplication of the box-filter / PNG-encode logic.  Moves the
			//! buffer in (no copy).  `pixels.size()` is expected to be
			//! `width*height`; a mismatch is defensively resized (padding with
			//! default-black) so the encode paths never index out of bounds.
			void AdoptCoherentSnapshot( std::vector<RISEColor>&& pixels,
			                            unsigned int width,
			                            unsigned int height );

			//! True once a full frame has been captured (OutputImage fired).
			bool HasImage() const { return mHasImage; }

			unsigned int Width()  const { return mWidth; }
			unsigned int Height() const { return mHeight; }

			//! The LINEAR per-channel means over all captured pixels (0 when
			//! no frame is captured).  A stable, thread-order-independent
			//! image signature: unlike the encoded PNG bytes (which diverge
			//! wholesale on a sub-LSB pixel change), the means differ only by
			//! the tiny MC noise floor between two renders of the same head
			//! and shift measurably under a visible edit.  Computed in linear
			//! space (before ToPng's sRGB Integerize) to avoid quantization.
			void MeanChannels( double& r, double& g, double& b ) const;

			//! Toolkit slice 3b (query_object_at): the captured pixel's LINEAR
			//! RISEColor at (x,y) -- the SAME per-pixel value ToPng()'s encode
			//! loop reads (row-major, pre-sRGB-Integerize; identical storage
			//! MeanChannels()/ToPng() already read). Returns false (leaving
			//! `out` unmodified) when no frame has been captured yet or (x,y)
			//! is outside [0,Width())x[0,Height()) -- no clamping, no
			//! wraparound. Const: reading a pixel does not mutate the
			//! captured buffer.
			bool GetPixelColor( unsigned int x, unsigned int y, RISEColor& out ) const;

			//! Set the DISPLAY TRANSFORM (exposure + tone curve) applied at
			//! encode time by ToPng()/ToPngDownscaled(), BEFORE the sRGB
			//! Integerize the PNGWriter performs.  This is the SAME stage the
			//! CLI `file_rasterizeroutput` PNG pipeline applies via
			//! `DisplayTransformWriter` (exposure multiply by 2^EV, then the
			//! selected `DISPLAY_TRANSFORM` tone curve) -- so a beauty sink
			//! configured with the scene's effective (exposure, tone-curve)
			//! encodes BYTE-IDENTICAL bytes to a CLI PNG render of the same
			//! scene.  Without this the sink encoded a raw linear->sRGB image
			//! (identity tone curve), which for RISE's default LDR output
			//! (ACES filmic) diverged systematically from every CLI / file /
			//! viewport render the agent's read_image / read_viewport is meant
			//! to mirror.
			//!
			//! `displayTransform` uses the `DISPLAY_TRANSFORM` enum values
			//! (0=none, 1=reinhard, 2=aces, 3=agx, 4=hable); it is a plain int
			//! here only so this header stays free of the DisplayTransform.h /
			//! Color.h dependency chain.  Default (unset) is IDENTITY
			//! (exposure 0, tone curve none) so any caller that does NOT set it
			//! -- e.g. the OBJECTMAP identity-render sink, whose per-pixel
			//! bytes must pass through un-tonemapped -- keeps the exact raw
			//! linear->sRGB behaviour this sink always had.  Affects ONLY the
			//! encoded PNG output; MeanChannels()/GetPixelColor() continue to
			//! report the stored LINEAR pixels (a display-independent image
			//! signature, per their own docs).
			void SetDisplayTransform( double exposureEV, int displayTransform );

			//! External review P2 fix: set the OUTPUT COLOUR SPACE ToPng()/
			//! ToPngDownscaled() pass to the underlying PNGWriter, BEFORE the
			//! sRGB Integerize/gamma stage -- the SAME `color_space` a
			//! `file_rasterizeroutput` PNG chunk applies (RISE_API_CreatePNGWriter's
			//! `color_space` argument).  Without this the sink always hardcoded
			//! `eColorSpace_sRGB` regardless of what the scene declared, so a
			//! scene authored with, say, `color_space Rec709RGB_Linear` diverged
			//! from a CLI PNG render of the same scene.  Default (unset) stays
			//! `eColorSpace_sRGB` -- today's pre-fix behaviour, and the
			//! descriptor's own default hint for an unspecified `color_space`.
			//!
			//! `colorSpace` uses the `COLOR_SPACE` enum values (0=sRGB,
			//! 1=Rec709RGB_Linear, 2=ROMMRGB_Linear, 3=ProPhotoRGB -- see
			//! Utilities/Color/Color.h); it is a plain int here only so this
			//! header stays free of the Color.h dependency, matching
			//! SetDisplayTransform's own `displayTransform` convention.  An
			//! out-of-range value degrades to `eColorSpace_sRGB` rather than
			//! indexing PNGWriter with a caller typo.
			void SetOutputColorSpace( int colorSpace );

			//! Account for a prior successful perception sidecar that remains
			//! live while this replacement render executes. AgentSession calls
			//! this before rendering so auxiliaryPeakBytes describes the actual
			//! session feature peak, not only the new frame in isolation.
			void SetConcurrentCachedPerceptionBytes( std::uint64_t bytes )
			{
				mConcurrentCachedPerceptionBytes = bytes;
			}

			//! Serialize the captured frame to 8-bit sRGB PNG bytes, reusing
			//! the tree's `PNGWriter` (sRGB Integerize + libpng) targeting a
			//! `MemoryBuffer` rather than a file.  When a non-identity display
			//! transform has been set (see SetDisplayTransform), the exposure +
			//! tone curve are applied first (same `DisplayTransformWriter`
			//! stage the CLI PNG pipeline uses).  Returns an EMPTY vector when
			//! no frame has been captured yet.  Const: encoding does not
			//! mutate the captured pixels.
			std::vector<unsigned char> ToPng() const;

			//! read_image maxEdge (F5 the cheap multi-angle observe loop):
			//! serialize a BOX-FILTER DOWNSCALED copy of the captured frame to
			//! 8-bit sRGB PNG bytes, reusing the same PNGWriter path as ToPng.
			//! The downscale happens on the LINEAR pixels (before ToPng's sRGB
			//! Integerize), same rationale as MeanChannels: averaging in linear
			//! space is the physically correct box filter, and it costs nothing
			//! extra since this method already owns the pre-encode buffer.
			//! `maxEdge` bounds the LONG edge (aspect-preserving; never
			//! upscales -- a frame already <= maxEdge on its long edge is
			//! encoded at its native size, same bytes as ToPng()).  Fills
			//! `outWidth`/`outHeight` with the dims actually encoded.  Returns
			//! an EMPTY vector when no frame has been captured yet (outWidth/
			//! outHeight left at 0).
			std::vector<unsigned char> ToPngDownscaled( unsigned int maxEdge,
			                                            unsigned int& outWidth,
			                                            unsigned int& outHeight ) const;

			//! Encode a conventional 2x2 diagnostic atlas from the compact
			//! sidecar captured with the last frame: beauty | albedo on the
			//! first row, world normal | log depth on the second.  maxEdge
			//! bounds the whole atlas and never upscales. Beauty honors
			//! SetOutputColorSpace; guide panels remain stable
			//! sRGB display fields independent of the beauty output setting.
			//! Encoding holds one RGBA scanline at a time;
			//! PerceptionInfo::encoderRowBytes reports
			//! that bounded uncompressed working set (the returned compressed
			//! PNG vector is the response payload and is necessarily retained).
			std::vector<unsigned char> ToPerceptionPng(
				unsigned int maxEdge,
				unsigned int& outWidth,
				unsigned int& outHeight,
				PerceptionInfo& outInfo ) const;
			bool HasPerception() const { return mPerceptionInfo.available; }
			PerceptionInfo GetPerceptionInfo() const { return mPerceptionInfo; }

		protected:
			~InMemoryRasterizerOutput() override;

		private:
			InMemoryRasterizerOutput( const InMemoryRasterizerOutput& );            // deleted
			InMemoryRasterizerOutput& operator=( const InMemoryRasterizerOutput& ); // deleted

			std::vector<RISEColor> mPixels;   //!< row-major, mWidth*mHeight, linear radiance (encode happens in ToPng)
			unsigned int           mWidth;
			unsigned int           mHeight;
			bool                   mHasImage;

			//! Encode-time display transform (see SetDisplayTransform).  0 EV
			//! + tone curve 0 (none) is the identity default -- ToPng() then
			//! emits the raw linear->sRGB bytes it always did.
			double                 mExposureEV;
			int                    mDisplayTransform;

			//! External review P2 fix: encode-time output colour space (see
			//! SetOutputColorSpace).  `COLOR_SPACE` enum value; defaults to
			//! eColorSpace_sRGB (0), matching this sink's pre-fix hardcoded
			//! behaviour.
			int                    mColorSpace;

			Implementation::FrameStore* mFrameStore;
			std::vector<unsigned char> mPerceptionAlbedo; //!< RGB8 sRGB display bytes
			std::vector<unsigned char> mPerceptionNormal; //!< RGB8 sRGB display bytes
			std::vector<unsigned char> mPerceptionDepth;  //!< 8-bit log-depth display bytes
			PerceptionInfo mPerceptionInfo;
			std::uint64_t mConcurrentCachedPerceptionBytes;
			std::uint64_t mObservedAuxiliaryPeakBytes;

			void CapturePerception_();
		};
	}
}

#endif
