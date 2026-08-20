//////////////////////////////////////////////////////////////////////
//
//  Voronoi3DPainter.cpp - Implenentation of the CheckPainter class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 14, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Voronoi3DPainter.h"

using namespace RISE;
using namespace RISE::Implementation;

Voronoi3DPainter::Voronoi3DPainter( const GeneratorsList& g, const IPainter& border_, const Scalar border_size_, const bool worldSpace_  ) :
  border( border_ ),
  border_size( border_size_ ),
  worldSpace( worldSpace_ )
{
	GeneratorsList::const_iterator i, e;

	for( i=g.begin(), e=g.end(); i!=e; i++ ) {
		if( i->second ) {
			i->second->addref();
			
			generators.push_back( std::make_pair(i->first,i->second) );

		} else {
			GlobalLog()->PrintEasyError( "Voronoi3DPainter:: Painter for one of the generators is NULL" );
		}
	}

	if( generators.size() < 2 ) {
		GlobalLog()->PrintEasyError( "Voronoi3DPainter:: Less than 2 generators, whats the point?" );
	}

	border.addref();
}

Voronoi3DPainter::~Voronoi3DPainter( )
{
	GeneratorsList::const_iterator i, e;

	for( i=generators.begin(), e=generators.end(); i!=e; i++ ) {
		i->second->release();
	}

	border.release();
}

inline const IPainter& Voronoi3DPainter::ComputeWhich( const RayIntersectionGeometric& ri ) const
{
	// Check the borders
	// @ there are no outer borders in the 3D version...
	/*
	if( ri.ptObjIntersec.x <= border_size ||
		ri.ptObjIntersec.y <= border_size ||
		ri.ptObjIntersec.y <= border_size ||
		ri.ptObjIntersec.x >= 1.0-border_size ||
		ri.ptObjIntersec.y >= 1.0-border_size ||
		ri.ptObjIntersec.z >= 1.0-border_size ) {
			return border;
		}
	*/

	const IPainter* pRet = 0;
	Scalar distance1 = RISE_INFINITY;
	Scalar distance2 = RISE_INFINITY;

	// P2.5 (doc 88): `space world` samples ptIntersection (matching the
	// other 3D painters); the historical default samples ptObjIntersec.
	const Point3& samplePt = worldSpace ? ri.ptIntersection : ri.ptObjIntersec;

	GeneratorsList::const_iterator i, e;

	for( i=generators.begin(), e=generators.end(); i!=e; i++ ) {
		const Generator& g = *i;

		const Scalar d = Point3Ops::Distance( g.first, samplePt );

		if( d < distance1 ) {
			distance2 = distance1;
			distance1 = d;
			pRet = g.second;
		}
	}

	if( distance2-distance1 <= border_size ) {
		return border;
	}

	return *pRet;
}

RISEPel Voronoi3DPainter::GetColor( const RayIntersectionGeometric& ri  ) const
{
	return ComputeWhich(ri).GetColor(ri);
}

SpectralPacket Voronoi3DPainter::GetSpectrum( const RayIntersectionGeometric& ri ) const
{
	return ComputeWhich(ri).GetSpectrum(ri);
}

Scalar Voronoi3DPainter::GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	return ComputeWhich(ri).GetColorNM(ri,nm);
}
