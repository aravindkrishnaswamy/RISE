//////////////////////////////////////////////////////////////////////
//
//  CylindricalUVGenerator.h - Generates cylinderical texture mapping
//                           co-ordinates
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 14, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CYLINDRICALUVGENERATOR_
#define CYLINDRICALUVGENERATOR_

#include "../Interfaces/IUVGenerator.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class CylindricalUVGenerator : public virtual IUVGenerator, public virtual Reference
		{
		protected:
			Scalar		m_OVRadius;
			char		m_chAxis;
			Scalar		m_dAxisMin;
			Scalar		m_dAxisMax;

			virtual ~CylindricalUVGenerator( );

		public:
			CylindricalUVGenerator( const Scalar radius, const char axis, const Scalar size );
			virtual void GenerateUV( const Point3& ptIntersection, const Vector3& vNormal, Point2& uv ) const;
		};
	}
}

#endif

