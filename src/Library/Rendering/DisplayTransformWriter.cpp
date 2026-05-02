//////////////////////////////////////////////////////////////////////
//
//  DisplayTransformWriter.cpp - Implementation of the
//  display-transform writer wrapper.  See DisplayTransformWriter.h
//  for the design.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 2, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "DisplayTransformWriter.h"

#include <cmath>

using namespace RISE;
using namespace RISE::Implementation;

DisplayTransformWriter::DisplayTransformWriter(
	IRasterImageWriter&  innerWriter,
	Scalar               exposureEV,
	DISPLAY_TRANSFORM    displayXform
	) :
  inner( innerWriter ),
  exposureMul( std::pow( Scalar( 2 ), exposureEV ) ),
  dt( displayXform )
{
	inner.addref();
}

DisplayTransformWriter::~DisplayTransformWriter()
{
	inner.release();
}

void DisplayTransformWriter::BeginWrite( const unsigned int width, const unsigned int height )
{
	inner.BeginWrite( width, height );
}

void DisplayTransformWriter::WriteColor( const RISEColor& c, const unsigned int x, const unsigned int y )
{
	// Apply exposure first (linear scale), then tone curve.  These
	// are orthogonal stages in the photographic pipeline (exposure
	// = sensor sensitivity, tone curve = display transform).
	//
	// Alpha passes through untouched: it represents coverage /
	// transparency, not radiance, and tone-mapping it would
	// produce wrong compositing in downstream tools.
	RISEPel scaled;
	scaled.r = c.base.r * exposureMul;
	scaled.g = c.base.g * exposureMul;
	scaled.b = c.base.b * exposureMul;

	const RISEPel mapped = DisplayTransforms::Apply( dt, scaled );

	inner.WriteColor( RISEColor( mapped, c.a ), x, y );
}

void DisplayTransformWriter::EndWrite()
{
	inner.EndWrite();
}
