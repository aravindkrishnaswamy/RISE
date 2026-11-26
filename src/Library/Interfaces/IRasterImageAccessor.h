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

		//! LOD-aware sample (Landing 2 of the PB pipeline plan).
		//! `lod` is the texture-space pixel-footprint level-of-detail:
		//! lod = 0 means sample at base resolution; lod = N means
		//! sample at the Nth level of the mip pyramid (each level has
		//! half the linear resolution of the previous).  Fractional
		//! LODs are supported by stochastic single-mip selection
		//! (Olano-Baker style — caller-driven RNG OR a deterministic
		//! coordinate hash).
		//!
		//! Default implementation forwards to GetPEL (no LOD support);
		//! accessors that build mip pyramids override.  Painters use
		//! this entry point ONLY when they have a footprint to drive
		//! the LOD; absence of footprint falls back to GetPEL.
		virtual void GetPELwithLOD(
			const Scalar x, const Scalar y,
			const Scalar lod,
			RISEColor& p ) const
		{
			(void)lod;
			GetPEL( x, y, p );
		}

		//! Footprint-aware sample for lowmem mode (Landing 2).
		//! Instead of building a mip pyramid, callers pass the FULL
		//! 2x2 footprint Jacobian in texture-space normalized UV
		//!   ( ∂u/∂x  ∂u/∂y )
		//!   ( ∂v/∂x  ∂v/∂y )
		//! and the accessor takes one jittered sample inside the
		//! screen-pixel parallelogram those columns span.  Earlier
		//! revisions collapsed the Jacobian to two scalar magnitudes
		//! and sampled an axis-aligned bounding box; that is biased
		//! on rotated / sheared mappings (e.g. rotated UV charts on
		//! Sponza pillars) — the AABB enlarges the integration region
		//! and over-blurs.  Passing the full Jacobian preserves the
		//! true footprint shape, so converges to the same integral as
		//! a proper mip lookup at infinite spp.
		//! Default implementation forwards to GetPEL (no footprint
		//! support); accessors that opt in to lowmem mode override.
		virtual void GetPELwithFootprint(
			const Scalar x, const Scalar y,
			const Scalar dudx, const Scalar dudy,
			const Scalar dvdx, const Scalar dvdy,
			const Scalar jitterU, const Scalar jitterV,
			RISEColor& p ) const
		{
			(void)dudx; (void)dudy; (void)dvdx; (void)dvdy;
			(void)jitterU; (void)jitterV;
			GetPEL( x, y, p );
		}

		//! Returns true if this accessor supports mip-LOD sampling
		//! (i.e., GetPELwithLOD does something other than forward to
		//! GetPEL).  Painter uses this to decide whether to compute
		//! and pass a LOD; accessors that don't support it skip the
		//! footprint computation.
		virtual bool SupportsLOD() const { return false; }

		//! Returns true if this accessor supports footprint-driven
		//! stochastic supersampling (GetPELwithFootprint does
		//! something other than forward to GetPEL).  Used when LOD
		//! pyramid storage is undesirable (lowmem mode) but the
		//! pixel-footprint anti-aliasing is still wanted: the painter
		//! jitters sample positions within the footprint at base
		//! resolution.  At infinite spp converges to the same integral
		//! as a proper mip lookup; zero pyramid memory.
		virtual bool SupportsFootprint() const { return false; }

		//! Underlying texture's pixel dimensions, used by painters to
		//! convert normalized-UV-space footprints to texel-space LODs.
		//! Default 1×1 for accessors that don't wrap a fixed-size
		//! image (procedural function accessors etc.).  Concrete
		//! image-backed accessors override.
		virtual unsigned int GetWidth() const  { return 1; }
		virtual unsigned int GetHeight() const { return 1; }

		//! Function2D requirements
		virtual Scalar	Evaluate(
			const Scalar x,						///< [in] X
			const Scalar y						///< [in] Y
			) const = 0;
	};
}

#endif
