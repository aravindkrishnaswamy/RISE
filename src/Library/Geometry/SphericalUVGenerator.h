//////////////////////////////////////////////////////////////////////
//
//  SphericalUVGenerator.h - Generates spherical texture mapping
//                           co-ordinates
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 13, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SPHERICALUVGENERATOR_
#define SPHERICALUVGENERATOR_

#include "../Interfaces/IUVGenerator.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class SphericalUVGenerator : public virtual IUVGenerator, public virtual Reference
		{
		protected:
			Scalar		m_OVRadius;

			virtual ~SphericalUVGenerator( );

		public:
			SphericalUVGenerator( const Scalar radius );
			virtual void GenerateUV( const Point3& ptIntersection, const Vector3& vNormal, Point2& uv ) const;
		};
	}
}

#endif

