//////////////////////////////////////////////////////////////////////
//
//  BezierTeseslation.h - Functions for tesselating beziers into
//  polygons
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 7, 2002
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BEZIER_TESSELATION_
#define BEZIER_TESSELATION_

#include "../Utilities/Math3D/Math3D.h"
#include "../Polygon.h"
#include <vector>

namespace RISE
{
	void GeneratePointsFromBezierCurve( VerticesListType& dest, const BezierCurve& curve, const unsigned int nDetail );
	void GeneratePointsFromBezierPatch( VerticesListType& dest, const BezierPatch& patch, const unsigned int nDetail );
	void GeneratePolygonsFromBezierPatch( IndexTriangleListType& tris, VerticesListType& points, NormalsListType& normals, TexCoordsListType& coords, const BezierPatch& patch, const unsigned int nDetail );
	void GeneratePolygonsFromBezierPatch( TriangleListType& tris, const BezierPatch& patch, const unsigned int nDetail );

	void GeneratePolygonsFromBezierPatches( IndexTriangleListType& tris, VerticesListType& points, NormalsListType& normals, TexCoordsListType& coords, BezierPatchesListType& patches, const unsigned int nDetail );
}

#endif //BEZIER_TESSELATION_

