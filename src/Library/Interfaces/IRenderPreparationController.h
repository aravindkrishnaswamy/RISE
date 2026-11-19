//////////////////////////////////////////////////////////////////////
//
//  IRenderPreparationController.h - Immutable render-input preparation seam
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
#ifndef IRENDER_PREPARATION_CONTROLLER_
#define IRENDER_PREPARATION_CONTROLLER_

#include <string>

namespace RISE
{
	struct RenderTimeSupport
	{
		double nominal = 0.0;
		double open = 0.0;
		double close = 0.0;
	};

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
}
#endif
