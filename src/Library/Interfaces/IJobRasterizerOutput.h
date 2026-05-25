//////////////////////////////////////////////////////////////////////
//
//  IJobRasterizerOutput.h - This the interface that applications must
//    implement if they want their own custom render output call back
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 29, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IJOB_RASTERIZER_OUTPUT_
#define IJOB_RASTERIZER_OUTPUT_

namespace RISE
{
	//! Very basic interface that applications must implement if they want
	//! to get access to the rasterized output
	class IJobRasterizerOutput
	{
	public:
		virtual ~IJobRasterizerOutput(){}
	
		//! Asks if you want premultiplied alpha
		virtual bool PremultipliedAlpha() = 0;

		//! Asks what colorspace is requested
		virtual int GetColorSpace() = 0;

		//! Outputs an intermediate region of rasterized data
		//! The rasterized data is a 16 bpp image
		virtual void OutputImageRGBA16(
			const unsigned short* pImageData,			///< [in] Data pointer to rasterized image
			const unsigned int width,					///< [in] Width of the image
			const unsigned int height,					///< [in] Height of the image
			const unsigned int rc_top,					///< [in] Defines the precise rectangular area to update
			const unsigned int rc_left,					///< [in] Defines the precise rectangular area to update
			const unsigned int rc_bottom,				///< [in] Defines the precise rectangular area to update
			const unsigned int rc_right					///< [in] Defines the precise rectangular area to update
			) = 0;

		//! HDR-aware companion to `OutputImageRGBA16`.  Default returns
		//! false, meaning the caller should use the 16-bit path with
		//! its [0, 1] linear clamp — preserves bit-exact behaviour for
		//! every existing consumer that doesn't override.
		//!
		//! When an output returns `true`, Job.cpp's
		//! `OutputIntermediateImage` SKIPS the
		//! `Integerize<...>(65535.0)` quantize step (which lossily
		//! clamps any linear value > 1.0 to 1.0) and instead writes
		//! raw `RISEColor.base.r/g/b/.a` floats into a contiguous RGBA
		//! buffer, then dispatches `OutputImageRGBA32F`.  This
		//! preserves the full HDR dynamic range — essential for
		//! consumers that pipe the image into a downstream view
		//! transform (Blender's Filmic + exposure compensation, OCIO
		//! display configs, EXR archival writers).
		//!
		//! Outputs that want HDR MUST override BOTH `WantsFloat32`
		//! (return true) AND `OutputImageRGBA32F` (consume the data).
		//! The 16-bit `OutputImageRGBA16` callback then becomes dead
		//! code for that output but stays declared because the base
		//! class makes it pure-virtual.
		virtual bool WantsFloat32() { return false; }

		//! HDR-float region update.  Only called when `WantsFloat32`
		//! returns true.  `pImageData` is a contiguous row-major
		//! `width × height × 4` array of linear Rec.709 RGBA floats
		//! (matching the colour space declared via `GetColorSpace`).
		//! Values may exceed 1.0; consumers are responsible for any
		//! tonemapping or clamping needed downstream.
		virtual void OutputImageRGBA32F(
			const float* /*pImageData*/,
			const unsigned int /*width*/,
			const unsigned int /*height*/,
			const unsigned int /*rc_top*/,
			const unsigned int /*rc_left*/,
			const unsigned int /*rc_bottom*/,
			const unsigned int /*rc_right*/
			)
		{
		}
	};
}

#endif

