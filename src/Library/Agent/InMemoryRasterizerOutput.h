//////////////////////////////////////////////////////////////////////
//
//  InMemoryRasterizerOutput.h - an IRasterizerOutput that captures the
//    final rasterized image into memory and serializes it to an sRGB
//    PNG byte buffer (Facet 5, the agentic surface -- slice 0b).
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
			InMemoryRasterizerOutput();

			//! Captures the full final image (region ignored -- file-style
			//! "whole image" semantics).  Overwrites any previously-captured
			//! frame, so after a render `mPixels` holds the last frame.
			void OutputImage( const IRasterImage& pImage, const Rect* pRegion, const unsigned int frame ) override;

			//! Intermediate scanlines are not retained (headless final-image
			//! sink; matches FileRasterizerOutput ignoring intermediates).
			void OutputIntermediateImage( const IRasterImage& pImage, const Rect* pRegion ) override;

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

			//! Serialize the captured frame to 8-bit sRGB PNG bytes, reusing
			//! the tree's `PNGWriter` (sRGB Integerize + libpng) targeting a
			//! `MemoryBuffer` rather than a file.  Returns an EMPTY vector
			//! when no frame has been captured yet.  Const: encoding does not
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

		protected:
			~InMemoryRasterizerOutput() override;

		private:
			InMemoryRasterizerOutput( const InMemoryRasterizerOutput& );            // deleted
			InMemoryRasterizerOutput& operator=( const InMemoryRasterizerOutput& ); // deleted

			std::vector<RISEColor> mPixels;   //!< row-major, mWidth*mHeight, linear radiance (encode happens in ToPng)
			unsigned int           mWidth;
			unsigned int           mHeight;
			bool                   mHasImage;
		};
	}
}

#endif
