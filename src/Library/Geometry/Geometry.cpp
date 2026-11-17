//////////////////////////////////////////////////////////////////////
//
//  Geometry.cpp - Implementation of some methods in the Geometry
//  class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 31, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Geometry.h"

using namespace RISE;
using namespace RISE::Implementation;

Geometry::Geometry( ) :
  bOnlyAsMesh( false ),
  m_shapeRevision( 1 )
{
}

Geometry::~Geometry( )
{
}
