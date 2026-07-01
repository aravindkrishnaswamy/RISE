//////////////////////////////////////////////////////////////////////
//
//  InMemoryRasterizerOutput.h - an IRasterizerOutput that captures the
//    final rasterized image into memory and serializes it to an sRGB
//    PNG byte buffer (Facet 5, the agentic surface -- slice 0b).
//
//    The agent's `render` / `read_image` verbs need the head's rendered
//    image WITHOUT touching the filesystem: no `file_rasterizeroutput`,
//    no RISE_MEDIA_PATH, no temp file to read back.  This sink holds the
//    last full `OutputImage` in memory as RISEColor pixels and, on
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

			//! True once a full frame has been captured (OutputImage fired).
			bool HasImage() const { return mHasImage; }

			unsigned int Width()  const { return mWidth; }
			unsigned int Height() const { return mHeight; }

			//! Serialize the captured frame to 8-bit sRGB PNG bytes, reusing
			//! the tree's `PNGWriter` (sRGB Integerize + libpng) targeting a
			//! `MemoryBuffer` rather than a file.  Returns an EMPTY vector
			//! when no frame has been captured yet.  Const: encoding does not
			//! mutate the captured pixels.
			std::vector<unsigned char> ToPng() const;

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
