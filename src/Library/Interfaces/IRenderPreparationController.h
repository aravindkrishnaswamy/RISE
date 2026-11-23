//////////////////////////////////////////////////////////////////////
//
//  IRenderPreparationController.h - Immutable render-input preparation seam
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
#ifndef IRENDER_PREPARATION_CONTROLLER_
#define IRENDER_PREPARATION_CONTROLLER_

#include <functional>
#include <string>

namespace RISE
{
	struct RenderTimeSupport
	{
		double nominal = 0.0;
		double open = 0.0;
		double close = 0.0;
	};

	//! The rasterizer's per-frame seam (PixelBasedRasterizerHelper's
	//! PrepareAndFreeze): between frames, `PrepareMediaForRender` installs
	//! every time-varying input for the frame's time support -- fire sequence
	//! media and, since simulation M3b, every ITimeVaryingAssetProvider (the
	//! name is historical) -- and the freeze pair brackets the render.
	class IRenderPreparationController
	{
	protected:
		virtual ~IRenderPreparationController() = default;
	public:
		virtual bool PrepareMediaForRender(
			const RenderTimeSupport& support, std::string& error ) = 0;
		virtual bool AcquireRenderFreeze( std::string& error ) = 0;
		virtual bool ReleaseRenderFreeze( std::string& error ) = 0;
	};

	//! Simulation M3b: a time-varying scene asset other than a fire sequence
	//! medium -- today a simulated deforming mesh -- that the preparation
	//! controller walks between frames, in the same transaction as the fire
	//! media.  PrepareAssetForRender installs the asset's state for the
	//! support (a no-op when that state is already installed) and, when
	//! `rollback` is non-null, hands back an undo that restores the previous
	//! state if a later asset or check fails.  Failure leaves the previous
	//! state intact.  While frozen (Acquire..Release, the render) a prepare is
	//! refused; Release reports whether the installed state stayed put.
	class ITimeVaryingAssetProvider
	{
	protected:
		virtual ~ITimeVaryingAssetProvider() = default;
	public:
		virtual bool PrepareAssetForRender( const RenderTimeSupport& support,
			std::string& error, std::function<void()>* rollback ) = 0;
		virtual bool AcquireAssetFreeze( std::string& error ) = 0;
		virtual bool ReleaseAssetFreeze( std::string& error ) = 0;
	};
}
#endif
