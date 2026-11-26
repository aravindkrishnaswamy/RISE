//////////////////////////////////////////////////////////////////////
//
//  EXRCompression.h - Standalone EXR_COMPRESSION enum.
//
//  Lives in its own header so EXRWriter.h (which pulls in the
//  OpenEXR headers when EXR support is enabled) does not need to
//  be included by every consumer that just needs to spell the
//  enum value.  RISE_API.h consumers in particular benefit — the
//  OpenEXR header tree is heavyweight to parse.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 2, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef EXR_COMPRESSION_
#define EXR_COMPRESSION_

namespace RISE
{
	//! Compression mode for EXR output.  Maps to OpenEXR's
	//! Imf::Compression values inside the writer; the enum is
	//! exposed here without OpenEXR headers so consumers can
	//! select a mode without pulling in the OpenEXR dependency.
	enum EXR_COMPRESSION
	{
		eExrCompression_None = 0,	///< No compression; largest files, fastest write
		eExrCompression_Zip  = 1,	///< Lossless ZIP (deflate) over 16-line blocks
		eExrCompression_Piz  = 2,	///< Lossless wavelet (PIZ); good HDR ratio (DEFAULT)
		eExrCompression_Dwaa = 3	///< Lossy DWAA (JPEG-like); smallest, lossy
	};
}

#endif
