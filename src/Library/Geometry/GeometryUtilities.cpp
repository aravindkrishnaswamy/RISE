//////////////////////////////////////////////////////////////////////
//
//  GeometryUtilities.cpp - Implementation of the geometry
//  utility functions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 21, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include <vector>
#include "../Polygon.h"
#include "../Interfaces/ILog.h"
#include "../Interfaces/IFunction2D.h"

namespace RISE
{

	bool CalculateVertexNormals( 
		IndexTriangleListType& vFaces, 
		NormalsListType& vNormals, 
		VerticesListType& vVertices 
		)
	{
		// Now comes the really fun part of calculating vertex normals!
		// Lets setup a funky data structure for this...
		int			nNumVerts = vVertices.size();

		Vector3	*sum = new Vector3[nNumVerts];
		GlobalLog()->PrintNew( sum, __FILE__, __LINE__, "sum list" );

		int			*incident = new int[nNumVerts];
		GlobalLog()->PrintNew( incident, __FILE__, __LINE__, "incident list" );

		// Initialize the sum and incident arrays
		for( int x=0; x<nNumVerts; x++ ) {
			incident[x]=0;
			sum[x] = Vector3( 0.0, 0.0, 0.0 );
		}

		IndexTriangleListType::iterator		m, n;
		for( m=vFaces.begin(), n=vFaces.end(); m!=n; m++ )
		{
			// We have to calculate the face normals for each then..
			// Grab two vectors for the current face...
			const IndexedTriangle&	tri = *m;
			const Vector3 vEdgeA = Vector3Ops::mkVector3( vVertices[tri.iVertices[1]], vVertices[tri.iVertices[0]] );
			const Vector3 vEdgeB = Vector3Ops::mkVector3( vVertices[tri.iVertices[2]], vVertices[tri.iVertices[0]] );
		
			Vector3 vCross = Vector3Ops::Normalize(Vector3Ops::Cross( vEdgeA, vEdgeB ));

			// Now all the vertices that on this face, should have their normal totals added to this number
			// and nIncident++
			sum[tri.iVertices[0]] = sum[tri.iVertices[0]] + vCross;
			incident[tri.iVertices[0]]++;

			sum[tri.iVertices[1]] = sum[tri.iVertices[1]] + vCross;
			incident[tri.iVertices[1]]++;

			sum[tri.iVertices[2]] = sum[tri.iVertices[2]] + vCross;
			incident[tri.iVertices[2]]++;
		}

		// Now divide all the elements in vNormList list by incident and normalize!
		for( int i = 0; i < nNumVerts; i++ )
		{
			Scalar	OOIncident = 1.0f / Scalar( incident[i] );
			sum[i] = sum[i] * OOIncident;
			vNormals.push_back( Vector3Ops::Normalize(sum[i]) );
		}

		GlobalLog()->PrintDelete( incident, __FILE__, __LINE__ );
		delete [] incident;

		GlobalLog()->PrintDelete( sum, __FILE__, __LINE__ );
		delete [] sum;

		return true;
	}

	bool GenerateGrid(
		const int nWidthDetail,
		const int nHeightDetail, 
		const Scalar left,
		const Scalar top, 
		const Scalar right, 
		const Scalar bottom,
		VerticesListType& vVertices, 
		NormalsListType& vNormals,
		TexCoordsListType& vCoords,
		IndexTriangleListType& vFaces
		)
	{
		struct	surfRect {
			Scalar	top;
			Scalar	left;
			Scalar	right;
			Scalar	bottom;
		};

		surfRect		SurfDim;
	//	SurfDim.left	=	-fWidth/2;
	//	SurfDim.right	=	fWidth/2;
	//	SurfDim.top		=	fHeight/2;
	//	SurfDim.bottom	=	-fHeight/2;

		SurfDim.left	=	left;
		SurfDim.right	=	right;
		SurfDim.top		=	top;
		SurfDim.bottom	=	bottom;

		// The following variables are pulled from the class
		// detail, width and height

		// Create the vertex points
		int		nPts = (nWidthDetail+1) * (nHeightDetail+1);
		int		nFaces = 2 * nWidthDetail * nHeightDetail;
		int		nNormals = nPts;
		int		nCoords = nPts;

		Scalar nHeight = SurfDim.top - SurfDim.bottom;
		Scalar nWidth = SurfDim.right - SurfDim.left;

		Scalar nWidthPiece = nWidth / Scalar(nWidthDetail);
		Scalar nHeightPiece = nHeight / Scalar(nHeightDetail);

		vVertices.resize( nPts );
		vCoords.resize( nCoords );
		vNormals.resize( nNormals );
		vFaces.resize( nFaces );

		int	i=0, j=0;

		// Create the vertices...
		for( i = 0; i < nHeightDetail + 1; i++ ) {
			for( j = 0; j < nWidthDetail + 1; j++ ) {
				vVertices[i*(nWidthDetail+1)+j] = Point3(SurfDim.left + nWidthPiece*Scalar(j), SurfDim.bottom + nHeightPiece*Scalar(i), 0);
			}
		}

		// Create the texture mapping co-ordinates
		// We create the temporary surface rect aligned so that it starts at zero...
		surfRect	srTemp;
		Scalar		leftAdd;
		Scalar		topAdd;

		if( SurfDim.left <= 0.0f ) {
			leftAdd = fabs( SurfDim.left );
		} else {
			leftAdd = -1.0f * SurfDim.left;
		}
		
		if( SurfDim.top <= 0.0f ) {
			topAdd = fabs( SurfDim.top );
		} else {
			topAdd = -1.0f * SurfDim.top;
		}

		srTemp.top = SurfDim.top + topAdd;
		srTemp.bottom = SurfDim.bottom + topAdd;
		srTemp.left = SurfDim.left + leftAdd;
		srTemp.right = SurfDim.right + leftAdd;

		for( i = 0; i < nHeightDetail + 1; i++ ) {
			for( j = 0; j < nWidthDetail + 1; j++ ) {
				vCoords[i*(nWidthDetail+1)+j] = Point2( (vVertices[i*(nWidthDetail+1)+j].x + leftAdd) / srTemp.right, (vVertices[i*(nWidthDetail+1)+j].y + topAdd) / srTemp.bottom );
			}
		}

		// Create default normals
		for( i = 0; i < nNormals; i++ ) {
			vNormals[i] = Vector3( 0, 0, 1.0f );
		}

		// Now generate the vFaces... with the appopriate vertices
		for( int k=0; k < nHeightDetail; k++ )				// going up
		{
			for( int n=0; n < nWidthDetail*2; n += 2 )		// going right
			{
				vFaces[k*nWidthDetail*2+n].iVertices[0] = vFaces[k*nWidthDetail*2+n+1].iVertices[0] = (n/2 + (k+1)*(nWidthDetail+1));
				vFaces[k*nWidthDetail*2+n].iVertices[1] = (n/2 + k*(nWidthDetail+1));
				vFaces[k*nWidthDetail*2+n].iVertices[2] = vFaces[k*nWidthDetail*2+n+1].iVertices[1] = (n/2 + k*(nWidthDetail+1) + 1);
				vFaces[k*nWidthDetail*2+n+1].iVertices[2] = (n/2 + (k+1)*(nWidthDetail+1) + 1);

				vFaces[k*nWidthDetail*2+n].iNormals[0] = vFaces[k*nWidthDetail*2+n+1].iNormals[0] = vFaces[k*nWidthDetail*2+n].iVertices[0];
				vFaces[k*nWidthDetail*2+n].iNormals[1] = vFaces[k*nWidthDetail*2+n].iVertices[1];
				vFaces[k*nWidthDetail*2+n].iNormals[2] = vFaces[k*nWidthDetail*2+n+1].iNormals[1] = vFaces[k*nWidthDetail*2+n].iVertices[2];
				vFaces[k*nWidthDetail*2+n+1].iNormals[2] = vFaces[k*nWidthDetail*2+n+1].iVertices[2];

				vFaces[k*nWidthDetail*2+n].iCoords[0] = vFaces[k*nWidthDetail*2+n+1].iCoords[0] = vFaces[k*nWidthDetail*2+n].iVertices[0];
				vFaces[k*nWidthDetail*2+n].iCoords[1] = vFaces[k*nWidthDetail*2+n].iVertices[1];
				vFaces[k*nWidthDetail*2+n].iCoords[2] = vFaces[k*nWidthDetail*2+n+1].iCoords[1] = vFaces[k*nWidthDetail*2+n].iVertices[2];
				vFaces[k*nWidthDetail*2+n+1].iCoords[2] = vFaces[k*nWidthDetail*2+n+1].iVertices[2];
			}
		}

		return true;
	}

	void RemapTextureCoords(
		TexCoordsListType& vCoords 
		)
		
	{
		TexCoordsListType::iterator		q, r;
		for( q=vCoords.begin(), r=vCoords.end(); q!=r; q++ ) {
			if( q->x > 0.5 ) {
				// Map [0.5..1] to [0..1]
				q->x = (q->x-0.5) * 2.0;
			} else if( q->x < 0.5 ) {
				// Map [0..0.5] to [1..0]
				q->x = 1.0-(q->x*2.0);
			} else {
				q->x = 0.0;
			}

			if( q->y > 0.5 ) {
				q->y = (q->y-0.5) * 2.0;
			} else if( q->y < 0.5 ) {
				q->y = 1.0-(q->y*2.0);
			} else {
				q->y = 0.0;
			}
		}
	}

	bool InvertObject( 
		IndexTriangleListType& vFaces, 
		NormalsListType& vNormals, 
		TexCoordsListType& vCoords 
		)
	{
		// Change the face order
		IndexTriangleListType::iterator		m, n;
		for( m=vFaces.begin(), n=vFaces.end(); m!=n; m++ )
		{
			IndexedTriangle&	tri = *m;
			int x, y, z;
			x = tri.iVertices[2];
			y = tri.iNormals[2];
			z = tri.iCoords[2];

			tri.iVertices[2] = tri.iVertices[1];
			tri.iNormals[2] = tri.iNormals[1];
			tri.iCoords[2] = tri.iCoords[1];

			tri.iVertices[1] = x;
			tri.iNormals[1] = y;
			tri.iCoords[1] = z;
		}

		// Invert all the normals
		NormalsListType::iterator		o, p;
		for( o=vNormals.begin(), p=vNormals.end(); o!=p; o++ ) {
			*o = -(*o);
		}

		// Invert all the texture mapping co-ordinates as well
		TexCoordsListType::iterator		q, r;
		for( q=vCoords.begin(), r=vCoords.end(); q!=r; q++ ) {
			(*q).x = 1.0 - (*q).x;
		}

		return true;
	}

	bool CenterObject( 
		VerticesListType& vVertices 
		)
	{
		Vertex	vMin( INFINITY, INFINITY, INFINITY );
		Vertex	vMax( -INFINITY, -INFINITY, -INFINITY );

		VerticesListType::const_iterator	i, e;

		for( i=vVertices.begin(), e=vVertices.end(); i!=e; i++ )
		{
			// Go through all the points and calculate the minimum and maximum values from the
			// entire set.
			const Vertex&	vP = *i;
			if( vP.x < vMin.x ) vMin.x = vP.x;
			if( vP.y < vMin.y ) vMin.y = vP.y;
			if( vP.z < vMin.z ) vMin.z = vP.z;
			if( vP.x > vMax.x ) vMax.x = vP.x;
			if( vP.y > vMax.y ) vMax.y = vP.y;
			if( vP.z > vMax.z ) vMax.z = vP.z;
		}

		// The center is the center of the minimum and maximum values of the points
		const Vertex vCenter = Point3Ops::WeightedAverage2( vMax, vMin, 0.5 );
		Vector3 vAdjust = Vector3Ops::mkVector3(vCenter, Vertex(0,0,0));

		// Now recenter the object
		VerticesListType::iterator	m, n;
		for( m=vVertices.begin(), n=vVertices.end(); m!=n; m++ ) {
			*m = Point3Ops::mkPoint3( *m, -vAdjust );
		}

		return true;
	}


	#define NEAR_EQUAL_NEARZERO 1e-12
	#define NEAR_EQUAL( a, b ) ( ((a-b) <= NEAR_EQUAL_NEARZERO) && ((a-b) >= -NEAR_EQUAL_NEARZERO) )
	#define NEAR_EQUAL_V3( a, b ) ( NEAR_EQUAL(a.x,b.x) && NEAR_EQUAL(a.y,b.y) && NEAR_EQUAL(a.z,b.z) )

		struct st_pair
		{
			Vertex vPoint;
			unsigned int iVertex;
			unsigned int iNormal;
			unsigned int iCoord;
		};


	bool CombineSharedVertices(
		IndexTriangleListType& vFaces, 
		VerticesListType& vVertices 
		)
	{
		// We will achieve this by building a list of points and checking against that list 
		// for every face.  Each element in the list is made up of a point/face index pair
		std::vector<st_pair>		pts;

		IndexTriangleListType::iterator	i, e;
		for( i=vFaces.begin(), e=vFaces.end(); i!=e; i++ )
		{
			IndexedTriangle& poly = *i;
			for( int j=0; j<3; j++ )
			{
				const Vertex& PtToTest = vVertices[poly.iVertices[j]];

				bool bAlreadyInList = false;

				unsigned int k=0;
				for( ; k<pts.size(); k++ ) {
					if( NEAR_EQUAL_V3( pts[k].vPoint, PtToTest ) && ( pts[k].iVertex != poly.iVertices[j] ) )
					{
						// Already in the list
						bAlreadyInList = true;
						break;
					}
				}

				if( bAlreadyInList ) {
					poly.iVertices[j] = pts[k].iVertex;
					poly.iNormals[j] = pts[k].iNormal;
					poly.iCoords[j] = pts[k].iCoord;
				} else {
					// Add it to the list
					st_pair elem;
					elem.vPoint = PtToTest;
					elem.iVertex = poly.iVertices[j];
					elem.iNormal = poly.iNormals[j];
					elem.iCoord = poly.iCoords[j];
					pts.push_back( elem );
				}
			}
		}

		return true;
	}

	void CombineSharedVerticesFromGrids(
		IndexTriangleListType& vFaces, 
		VerticesListType& vVertices,
		const unsigned int numgrids,
		const unsigned int nWidthDetail,
		const unsigned int nHeightDetail
		)
	{
		// We will achieve this by building a list of points and checking against that list 
		// for every face.  Each element in the list is made up of a point/face index pair
		// In this specialized version, we know the points were all generated by a sequence of 
		// grids, where each grid is the same size and has the same number of polygons.  Thus
		// we only need to look at the top row, bottom row, left column and right column of the grids!
		std::vector<st_pair>		pts;
		pts.reserve(numgrids*(nWidthDetail*2+nHeightDetail*2));

		unsigned int ctr=0;
		for( unsigned int m=0; m<numgrids; m++ )
		{
			// For each grid
			for( unsigned int i = 0; i < nHeightDetail; i++ ) {
				for( unsigned int j = 0; j < nWidthDetail; j++, ctr+=2 ) {
					// Only bother to do the work if we have to
					if( i==0 || i==nWidthDetail-1 || j==0 || j==nHeightDetail-1 ) {

						// Remember there are two triangles / grid square
						for( int t=0; t<2; t++ ) {
							IndexedTriangle& poly = vFaces[ctr+t];

							// Check each vertex
							for( int l=0; l<3; l++ )
							{
								const Vertex& PtToTest = vVertices[poly.iVertices[l]];

								bool bAlreadyInList = false;

								unsigned int k=0;
								for( ; k<pts.size(); k++ ) {
									if( NEAR_EQUAL_V3( pts[k].vPoint, PtToTest ) && ( pts[k].iVertex != poly.iVertices[l] ) )
									{
										// Already in the list
										bAlreadyInList = true;
										break;
									}
								}

								if( bAlreadyInList ) {
									poly.iVertices[l] = pts[k].iVertex;
									poly.iNormals[l] = pts[k].iNormal;
									poly.iCoords[l] = pts[k].iCoord;
								} else {
									// Add it to the list
									st_pair elem;
									elem.vPoint = PtToTest;
									elem.iVertex = poly.iVertices[l];
									elem.iNormal = poly.iNormals[l];
									elem.iCoord = poly.iCoords[l];
									pts.push_back( elem );
								}
							}
						}
					}
				}
			}
		}
	}

	#include "../Utilities/GeometricUtilities.h"

	void ApplyDisplacementMapToObject( 
		IndexTriangleListType& vFaces, 
		VerticesListType& vVertices, 
		NormalsListType& vNormals, 
		TexCoordsListType& vCoords,
		const IFunction2D& displacement,
		const Scalar scale
		)
	{
		// The vector will track whether we have already displaced a vertex
		std::vector<bool> done_list( vVertices.size(), false );

		IndexTriangleListType::iterator	i, e;
		for( i=vFaces.begin(), e=vFaces.end(); i!=e; i++ )
		{
			IndexedTriangle& poly = *i;
			for( int j=0; j<3; j++ )
			{
				const unsigned int idx = poly.iVertices[j];
				if( !done_list[idx] ) {
					// Displace the vertex
					Vertex& v = vVertices[idx];

					const Scalar disp = displacement.Evaluate( vCoords[idx].x, vCoords[idx].y ) * scale;
					v = Point3Ops::mkPoint3( v, vNormals[idx] * disp );

					done_list[idx] = true;
				}
			}
		}
	}

}
