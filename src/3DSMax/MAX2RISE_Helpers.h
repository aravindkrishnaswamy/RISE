//////////////////////////////////////////////////////////////////////
//
//  MAX2RISE_Helpers.h - Helper functions for converting 3DSMAX
//    stuff to our stuff
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 13, 2005
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MAX2RISE_H
#define MAX2RISE_H

inline RISE::Point3 MAX2RISEPoint( const Point3& p )
{
	return RISE::Point3( p.x, p.y, p.z );
}

inline RISE::Vector3 MAX2RISEVector( const Point3& p )
{
	return RISE::Vector3( p.x, p.y, p.z );
}

inline RISE::Point2 MAX2RISEUV( const Point3& p )
{
	return RISE::Point2( p.x, p.y );
}

inline Point3 RISE2MAXPoint( const RISE::Point3& p )
{
	return Point3( p.x, p.y, p.z );
}

inline Point3 RISE2MAXVector( const RISE::Vector3& v )
{
	return Point3( v.x, v.y, v.z );
}

inline void getNormalsForFace( Instance& instance, unsigned int faceNum, Point3* vxnormal )
{
	RVertex* rv[3] = {0};
	Mesh& mesh = *instance.mesh;
	Face& f = mesh.faces[faceNum];
	DWORD smGroup = f.smGroup;
	int numNormals;
	int vxNum;

	// Get all three normals
	for( int i = 0, cc = 2; i < 3; i++, cc-- ) {

		// We need to get the vertices in counter clockwise order
		// if the object has negative scaling.
		if( !instance.TestFlag(INST_TM_NEGPARITY) ) {
			vxNum = i;
		} else {
			vxNum = cc;
		}

		rv[i] = mesh.getRVertPtr(f.getVert(vxNum));

		// Is normal specified
		// SPCIFIED is not currently used, but may be used in future versions.
		if (rv[i]->rFlags & SPECIFIED_NORMAL) {
			vxnormal[i] = rv[i]->rn.getNormal();
		}
		// If normal is not specified it's only available if the face belongs
		// to a smoothing group
		else if ((numNormals = rv[i]->rFlags & NORCT_MASK) && smGroup) {
			// If there is only one vertex is found in the rn member.
			if (numNormals == 1) {
				vxnormal[i] = rv[i]->rn.getNormal();
			}
			else {
				// If two or more vertices are there you need to step through them
				// and find the vertex with the same smoothing group as the current face.
				// You will find multiple normals in the ern member.
				for (int j = 0; j < numNormals; j++) {
					if (rv[i]->ern[j].getSmGroup() & smGroup) {
						vxnormal[i] = rv[i]->ern[j].getNormal();
					}
				}
			}
		} else {
			vxnormal[i] = mesh.getFaceNormal(faceNum);
		}
	}
}

#endif