//////////////////////////////////////////////////////////////////////
//
//  BoxUVGenerator.h - Generates box texture mapping
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

#ifndef BOXUVGENERATOR_
#define BOXUVGENERATOR_

#include "../Interfaces/IUVGenerator.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class BoxUVGenerator : public virtual IUVGenerator, public virtual Reference
		{
		protected:
			Scalar	dOVWidth;
			Scalar	dOVHeight;
			Scalar	dOVDepth;

			Scalar	dWidthOV2;
			Scalar	dHeightOV2;
			Scalar	dDepthOV2;

			virtual ~BoxUVGenerator( );

		public:
			BoxUVGenerator( const Scalar width, const Scalar height, const Scalar depth );
			virtual void GenerateUV( const Point3& ptIntersection, const Vector3& vNormal, Point2& uv ) const;
		};
	}
}

#endif

