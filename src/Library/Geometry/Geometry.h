//////////////////////////////////////////////////////////////////////
//
//  Geometry.h - Declaration of the geometry class which is what
//  things that want to be classified as geometry must extend.
//  Some functions are pure virtual, others are only virtual
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 21, 2001
//  Tabs: 4
//  Comments:  All geometry objects *MUST* be representable as 
//			   triangular meshes, see meshes for mesh properties
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef GEOMETRY_
#define GEOMETRY_

#include "../Interfaces/IGeometry.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class Geometry : public virtual IGeometry, public virtual Reference
		{
		protected:
			// Can this geometry object only be represented as a 
			// mesh ?
			bool		bOnlyAsMesh;

		//	std::vector<Triangle>		triangles;

			Geometry();
			virtual ~Geometry();

		public:

			// Generates a mesh and stores it in the local class 
			// variables, all sub classes better implement this function
			virtual void GenerateMesh( );
		};
	}
}

#endif
