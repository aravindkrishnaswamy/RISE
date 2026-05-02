//////////////////////////////////////////////////////////////////////
//
//  DisplayTransformWriter.h - IRasterImageWriter wrapper that
//  applies an exposure scale and a tone-curve display transform
//  to every WriteColor before forwarding to an inner writer.
//
//  Design rationale
//  ----------------
//  The inner writer (PNG / TGA / PPM / TIFF / etc.) already applies
//  the per-pixel primaries conversion + OETF + integerisation in
//  its own WriteColor.  Threading exposure / tone curve into every
//  inner writer would duplicate the chain across seven file
//  formats.  Wrapping the writer interposes the chain in one place;
//  inner writers stay unchanged and ignorant of tone-mapping.
//
//  Lifetime
//  --------
//  The wrapper holds an addref'd reference to the inner writer.
//  The caller is still responsible for releasing the reference
//  *they* hold on the inner writer (the addref count goes up by 1
//  when the wrapper is constructed).  This matches the existing
//  pattern used by HDRWriter / EXRWriter for their IWriteBuffer
//  references.
//
//  HDR formats
//  -----------
//  Never wrap an HDR writer (EXR / HDR / RGBEA) in this class.
//  Those formats are the radiometric ground truth; tone-mapping
//  them corrupts the archival radiance.  FileRasterizerOutput
//  enforces this by skipping the wrap when the format is HDR.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 2, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DISPLAY_TRANSFORM_WRITER_
#define DISPLAY_TRANSFORM_WRITER_

#include "../Interfaces/IRasterImageWriter.h"
#include "../Utilities/Reference.h"
#include "DisplayTransform.h"

namespace RISE
{
	namespace Implementation
	{
		class DisplayTransformWriter :
			public virtual IRasterImageWriter,
			public virtual Reference
		{
		protected:
			IRasterImageWriter&		inner;			///< Inner format writer; addref'd in ctor, released in dtor
			const Scalar			exposureMul;	///< Pre-computed pow(2, exposureEV)
			const DISPLAY_TRANSFORM	dt;				///< Tone curve to apply

			virtual ~DisplayTransformWriter();

		public:
			//! \param innerWriter   The format-specific writer to wrap.
			//!                      Addref'd by the constructor.
			//! \param exposureEV    Exposure offset in EV stops.  0 = no
			//!                      scaling; +1 doubles, -1 halves.
			//! \param displayXform  Tone curve to apply per channel.
			DisplayTransformWriter(
				IRasterImageWriter&  innerWriter,
				Scalar               exposureEV,
				DISPLAY_TRANSFORM    displayXform
				);

			void BeginWrite( const unsigned int width, const unsigned int height ) override;
			void WriteColor( const RISEColor& c, const unsigned int x, const unsigned int y ) override;
			void EndWrite() override;
		};
	}
}

#endif
