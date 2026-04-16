//////////////////////////////////////////////////////////////////////
//
//  VCMPathOps.h - Free-function wrappers around the private BDPT
//    helpers that the VCM integrator needs.
//
//    VCM wants to evaluate the same primitives as BDPT:
//      - Shadow-ray visibility between two vertices
//      - Connection transmittance through participating media
//      - BSDF evaluation at a surface/medium vertex for an arbitrary
//        (wi, wo) pair
//      - Forward / reverse solid-angle PDF at a vertex for an
//        arbitrary (wi, wo) pair
//
//    BDPTIntegrator's existing helpers are private and the plan is
//    to leave BDPTIntegrator strictly untouched apart from the two
//    new data fields on BDPTVertex.  So VCM gets its own tiny copy
//    of those helpers via this header.  The implementation will
//    largely mirror the BDPT originals but lives in a namespace
//    that VCMIntegrator can call freely.
//
//    Step 0 ships the interface only so the build wiring compiles.
//    Step 8 populates the bodies with the copied helper code.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 14, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef VCM_PATH_OPS_
#define VCM_PATH_OPS_

#include "BDPTVertex.h"

namespace RISE
{
	class IScene;
	class IRayCaster;

	namespace Implementation
	{
		namespace VCMPathOps
		{
			/// Unoccluded ray from a -> b.  Mirrors
			/// BDPTIntegrator::IsVisible.  Step 8 fills this in.
			bool IsVisible(
				const IScene& scene,
				const IRayCaster& caster,
				const Point3& a,
				const Point3& b
				);
		}
	}
}

#endif
