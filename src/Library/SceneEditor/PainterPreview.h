//////////////////////////////////////////////////////////////////////
//
//  PainterPreview.h - Headless painter-preview engine producing RGBA8
//  thumbnails of named painters, expression `def` stages, or ramp
//  strips by evaluating over a synthetic grid without a scene render.
//
//  Domain conventions (keep PainterPreviewTest in sync):
//  - Pixel centres: u = (x + 0.5) / w, v = (y + 0.5) / h (row 0 = low v).
//  - P = Po = (u - 0.5, v - 0.5, 0), N = (0, 0, 1), time = 0.
//  - fw = 0 with widthValid = true (honest point sample; a synthetic
//    patch pitch would erroneously fade high-frequency fbm octaves).
//
//  Encoding & normalization:
//  - Colour outputs (IPainter, vec3 `def` stages): linear Rec.709 clamped
//    to [0, 1], gamma-2.2 encoded (no auto-ranging).
//  - Scalar outputs (IScalarPainter, scalar `def` stages): auto-ranged
//    from [dataMin, dataMax] to [0, 1] (flat -> 0.5; range reported in
//    Result). HasPerChannelVariation() scalar painters render RGB using
//    ONE joint [dataMin, dataMax] across all 3 channels (`wasScalar = true`).
//
//  Concurrency & budget: callers hold the lock protecting `job`'s manager
//  graph. Distinct evaluations per call are capped at `kMaxSampleBudget`
//  (decimated and nearest-upscaled to exact w*h*4 output).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_PAINTERPREVIEW_
#define RISE_PAINTERPREVIEW_

#include "../Interfaces/IJobPriv.h"
#include "../Utilities/RString.h"
#include <cstdint>
#include <string>
#include <vector>

namespace RISE
{
	class PainterPreview
	{
	public:
		//! Hard cap on preview width/height (larger requests are refused).
		static const unsigned int kMaxDim = 2048;

		//! Cap on distinct per-pixel evaluations per RenderPainterPreview /
		//! RenderDefStagePreview call; larger w*h requests evaluate on a
		//! decimated grid and nearest-upscale to exact w*h*4 output.
		static const unsigned int kMaxSampleBudget = 96u * 96u;

		enum class Status { Ok, Refused };

		struct Result
		{
			Status      status = Status::Refused;
			std::string refusalReason;   // set iff status == Refused

			unsigned int width  = 0;
			unsigned int height = 0;
			//! Row-major, top row (y=0, the LOW end of v -- see the header's
			//! domain-convention note) first; RGBA8, 4 bytes/pixel,
			//! size == width*height*4 iff status == Ok.
			std::vector<unsigned char> rgba;

			//! Set iff status == Ok for a scalar stage (including per-channel
			//! IScalarPainters, where scalarRangeMin/Max is the joint range
			//! shared across all 3 RGB channels).
			bool   wasScalar     = false;
			double scalarRangeMin = 0.0;
			double scalarRangeMax = 0.0;
		};

		//! Previews `painterName` (colour pipe tried first, then scalar pipe;
		//! refuses if w/h is 0 or > kMaxDim). Requests over kMaxSampleBudget
		//! decimate and nearest-upscale to exact w*h*4 output.
		static Result RenderPainterPreview( IJobPriv& job, const String& painterName,
			unsigned int w = 64, unsigned int h = 64 );

		//! Previews expression `def` slot `defIndex` (0-based declaration order)
		//! for `painterName` (colour or scalar expression painter); refuses if
		//! not an expression painter or `defIndex` is out of range.
		static Result RenderDefStagePreview( IJobPriv& job, const String& painterName,
			int defIndex, unsigned int w = 64, unsigned int h = 64 );

		//! Previews a `ramp_painter`'s colour interpolation over its stop
		//! domain as a horizontal strip (`input` painter is not evaluated;
		//! rows are replicated when h > 1).
		static Result RenderRampStripPreview( IJobPriv& job, const String& painterName,
			unsigned int w = 64, unsigned int h = 1 );
	};
}

#endif
