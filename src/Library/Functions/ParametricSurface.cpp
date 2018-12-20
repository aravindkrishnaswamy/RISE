//////////////////////////////////////////////////////////////////////
//
//  ParametricSurface.cpp - Implements Helper class for parametric surfaces
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 26, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ParametricSurface.h"

using namespace RISE;
using namespace RISE::Implementation;

ParametricSurface::ParametricSurface() : 
  u_start( 0 ),
  u_end( 0 ),
  v_start( 0 ),
  v_end( 0 )
{
}

ParametricSurface::~ParametricSurface()
{
}

//! Sets the evaluation range of u and v
void ParametricSurface::SetRange(
	const Scalar u_s,						///< [in] Where u starts
	const Scalar u_e,						///< [in] Where u ends
	const Scalar v_s,						///< [in] Where v starts
	const Scalar v_e						///< [in] Where v ends
	)
{
	u_start = u_s;
	u_end = u_e;
	v_start = v_s;
	v_end = v_e;
}

//! Returns the evaluation range of u and v
void ParametricSurface::GetRange(
	Scalar& u_s,							///< [in] Where u starts
	Scalar& u_e,							///< [in] Where u ends
	Scalar& v_s,							///< [in] Where v starts
	Scalar& v_e								///< [in] Where v ends
	)
{
	u_s = u_start;
	u_e = u_end;
	v_s = v_start;
	v_e = v_end;
}