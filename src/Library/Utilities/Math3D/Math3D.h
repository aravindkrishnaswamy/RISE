//////////////////////////////////////////////////////////////////////
//
//  Math3D.h - Includes most aspects of 3D math routines
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 13, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MATH3D_
#define MATH3D_

//
// Forward declarations of all types so that each can reference
// the other
//

#include <math.h>
#include "../math_utils.h"
namespace RISE
{
	typedef double Scalar;				// All scalars are doubles

	// Forward declarations
	struct Direction;
	struct Point2;
	struct Point3;
	struct Vector2;
	struct Vector3;
	struct Matrix3;
	struct Matrix4;
	struct Quaternion;
}

#include "Constants.h"
#include "Direction.h"
#include "Vectors.h"
#include "Points.h"
#include "Quaternion.h"
#include "Matrices.h"

#include "VectorsOps.h"
#include "PointsOps.h"
#include "MatricesOps.h"
#include "QuaternionOps.h"

#endif

