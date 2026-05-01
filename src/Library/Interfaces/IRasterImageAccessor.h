//////////////////////////////////////////////////////////////////////
//
//  IRasterImageAccessor.h - Defines an interface for a raster image 
//  accessor.  A raster image accessor is capable of accessing 
//  raster images sub-pixelly.
//                
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 26, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IRASTERIMAGEACCESSOR_
#define IRASTERIMAGEACCESSOR_

#include "IFunction2D.h"
#include "IReference.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Color/Color_Template.h"

namespace RISE
{
	//! Sub-pixel address-wrap mode for a raster-image accessor.  Applied
	//! to the input UVs BEFORE pixel-coordinate scaling, so any subset of
	//! `(x, y)` outside [0, 1] maps to a stable in-bounds neighbour
	//! according to the chosen mode.  Char-encoded for the painter API
	//! (mirrors color_space / filter_type / lowmemory etc.):
	//!
	//!   0 = ClampToEdge — UVs outside [0,1] sample the boundary texel.
	//!       Matches the pre-2026-05-01 behaviour and stays the default
	//!       so legacy scenes render byte-identically.
	//!   1 = Repeat — UVs are reduced modulo 1 (tile/wrap).  glTF 2.0's
	//!       default sampler wrap; the right choice for tiled brick /
	//!       wood / plaster atlases (NewSponza floor).
	//!   2 = MirroredRepeat — UVs alternate between forward and reversed
	//!       on each integer crossing (no seams between tiles).
	//!
	//! Applied independently per axis (`wrap_s` for the U axis,
	//! `wrap_t` for the V axis) so an asset that wants a horizontally-
	//! tiling but vertically-clamped texture (rare but real) can declare
	//! that.
	enum eRasterWrapMode
	{
		eRasterWrap_ClampToEdge      = 0,
		eRasterWrap_Repeat           = 1,
		eRasterWrap_MirroredRepeat   = 2
	};

	//! Provides access to a raster image.  This is mainly used to
	//! provide sub-pixel access to raster images
	class IRasterImageAccessor : public virtual IFunction2D
	{
	protected:
		IRasterImageAccessor( ){};
		virtual ~IRasterImageAccessor( ){};

	public:
		//! Gets an RISEColor
		virtual void GetPEL( 
			const Scalar x,						///< [in] sub-pixel X value of pixel
			const Scalar y,						///< [in] sub-pixel Y of pixel
			RISEColor& p							///< [out] Color of pixel
			) const = 0;

		//! Gets an RISEColor
		virtual void SetPEL( 
			const Scalar x,						///< [in] sub-pixel X value of pixel
			const Scalar y,						///< [in] sub-pixel Y value of pixel
			RISEColor& p							///< [in] Color to set
			) const = 0;

		//! Function2D requirements
		virtual Scalar	Evaluate( 
			const Scalar x,						///< [in] X
			const Scalar y						///< [in] Y
			) const = 0;
	};
}

#endif
