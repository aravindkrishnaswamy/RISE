//////////////////////////////////////////////////////////////////////
//
//  IRayIntersectionModifier.h - An interface to an object
//  that is capable of taking a ray intersection and screwing
//  around with it
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 17, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IRAYINTERSECTION_MODIFIER_
#define IRAYINTERSECTION_MODIFIER_
#define RISE_SMS_MODIFIER_DIFFERENTIAL 1
#define RISE_SMS_COMPOSED_DIFFERENTIAL 1

#include "IReference.h"
#include "../Intersection/RayIntersectionGeometric.h"

namespace RISE
{
    // Optional extended-SMS capability. Deliberately separate from the
    // legacy modifier vtable. Raster state and discrete hit identities are
    // fixed; unsupported input dependencies cannot claim this contract.
    struct SMSIntersectionDifferential
    {
        Vector3 worldPoint{0,0,0}, objectPoint{0,0,0};
        Vector3 normal{0,0,0}, geometricNormal{0,0,0};
        Vector3 frameU{0,0,0}, frameV{0,0,0}, frameW{0,0,0};
        Vector3 rayOrigin{0,0,0}, rayDirection{0,0,0};
        Point2 uv{0,0};
    };
    class ISMSModifierDifferential
    {
    public:
        virtual ~ISMSModifierDifferential() = default;
        // A static declaration, valid for every supported input of this
        // provider. Query failure is uncertainty, never a zero derivative.
        virtual bool HasSMSDifferentialContract() const = 0;
        // Derivatives of the fields AFTER Modify(raw), along the supplied
        // raw native-hit differential. Must preserve physical surface and
        // geometric normal. Return false at unaudited dependencies/boundaries.
        virtual bool SMSFrameDifferential(const RayIntersectionGeometric& raw,
            const SMSIntersectionDifferential& input,
            Vector3& normal, Vector3& frameW) const = 0;
        // Conservative default: undeclared dependencies include raw UVs.
        // False certifies that output normal/frame derivatives do not consume
        // raw UV values or derivatives, for every supported provider input.
        virtual bool SMSFrameDependsOnUV() const { return true; }
    };

	//! Has the ability to modify intersection details
	class IRayIntersectionModifier : public virtual IReference
	{
	protected:
		IRayIntersectionModifier(){};
		virtual ~IRayIntersectionModifier(){};

	public:
		//! Modifies a ray intersection
		virtual void Modify( 
			RayIntersectionGeometric& ri				///< [in/out] The geometric intersection information to modify
			) const = 0;
	};
}

#endif
