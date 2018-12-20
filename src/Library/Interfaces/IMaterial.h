//////////////////////////////////////////////////////////////////////
//
//  IMaterial.h - Defines an interface to a material.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 28, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IMATERIAL_
#define IMATERIAL_

#include "IReference.h"

namespace RISE
{
	class ISPF;
	class IBSDF;
	class IEmitter;

	//! The IMaterial interface is basically an aggregate of other interfaces.  Though we don't actually
	//! aggregate the interfaces, in essense what is all it does
	/// \sa IBSDF
	/// \sa ISPF
	/// \sa IEmitter
	class IMaterial : public virtual IReference
	{
	protected:
		IMaterial(){};
		virtual ~IMaterial( ){};

	public:

		/// \return The BRDF for this material.  NULL If there is no BRDF
		virtual IBSDF* GetBSDF() const = 0;

		/// \return The SPF for this material.  NULL If there is no SPF
		virtual ISPF* GetSPF() const  = 0;

		/// \return The emission properties for this material.  NULL If there is not an emitter
		virtual IEmitter* GetEmitter() const  = 0;
	};
}

#include "IBSDF.h"
#include "ISPF.h"
#include "IEmitter.h"

#endif
