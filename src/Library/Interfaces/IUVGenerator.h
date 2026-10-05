//////////////////////////////////////////////////////////////////////
//
//  IUVGenerator.h - Interface to UV generators, which are classes
//                   that generate texture mapping co-ordinates
//					 based on custom parameters and the point of
//                   intersection
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 13, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IUVGENERATOR_
#define IUVGENERATOR_

#include "IReference.h"
#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
    // Optional internal capability; the legacy UV-generator vtable is unchanged.
    // Covers every supported object-space point/normal (the same arguments
    // Object passes to GenerateUV) and declines unaudited boundaries.
    class ISMSUVDifferential
    {
    public:
        virtual ~ISMSUVDifferential() = default;
        virtual bool HasSMSUVDifferentialContract() const = 0;
        virtual bool SMSUVDifferential(const Point3&,const Vector3&,
            const Vector3& pointDerivative,const Vector3& normalDerivative,
            Point2& uvDerivative) const = 0;
    };
	//! Generates UV co-ordinates
	class IUVGenerator : public virtual IReference
	{
	protected:
		IUVGenerator( ){};
		virtual ~IUVGenerator( ){};

	public:
		// Asks to generate the texture co-ordinates
		virtual void GenerateUV( 
			const Point3& ptIntersection,					///< [in] Point of intersection
			const Vector3& vNormal,						///< [in] Normal at point of intersection
			Point2& uv										///< [out] Texture mapping co-ordinates
			) const = 0;
	};
}

#endif
