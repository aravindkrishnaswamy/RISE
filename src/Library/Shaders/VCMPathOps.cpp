//////////////////////////////////////////////////////////////////////
//
//  VCMPathOps.cpp - Free-function wrappers around BDPT helpers.
//
//    Step 0 ships a safe-default stub so the build wiring compiles.
//    Step 8 pastes the copied helper bodies.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 14, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "VCMPathOps.h"

namespace RISE
{
	namespace Implementation
	{
		namespace VCMPathOps
		{
			bool IsVisible(
				const IScene&,
				const IRayCaster&,
				const Point3&,
				const Point3&
				)
			{
				// Step 8 populates this by porting the same shadow-ray
				// logic BDPTIntegrator uses when it connects two
				// subpath vertices.  For Step 0 we return false so the
				// stubs compile and the VCM integrator never thinks a
				// connection is free to take.
				return false;
			}
		}
	}
}
