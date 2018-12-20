//////////////////////////////////////////////////////////////////////
//
//  IPhotonTracer.h - Interface to a photon tracer
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 29, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IPHOTON_TRACER_
#define IPHOTON_TRACER_

#include "IReference.h"
#include "IScenePriv.h"
#include "IProgressCallback.h"

namespace RISE
{
	//! A photon tracer is able to shoot photons into a scene and store
	//! them in the scene's photon map
	class IPhotonTracer : public virtual IReference
	{
	protected:
		IPhotonTracer(){};
		virtual ~IPhotonTracer(){};

	public:
		//! Attached a scene to this tracer
		virtual void AttachScene( 
			IScenePriv* pScene_					///< [in] Scene to attach
			) = 0;

		//! Traces photons
		/// \return TRUE if photons were traced, FALSE otherwise
		virtual bool TracePhotons( 
			const unsigned int numPhotons,		///< [in] Number of photons to acquire in the photon map
			const Scalar time,					///< [in] The time to trace these photons at
			const bool bAtTime,					///< [in] Should we be tracing photons at a particular time?
			IProgressCallback* pFunc			///< [in] Callback functor for reporting progress
			) const = 0;
	};
}

#endif

