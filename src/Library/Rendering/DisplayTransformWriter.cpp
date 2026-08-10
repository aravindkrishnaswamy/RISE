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
#include "../Utilities/Color/ColorUtils.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	FrameStoreOutput::ViewTransform LegacyViewTransform(
		const Scalar exposureEV, const DISPLAY_TRANSFORM displayXform )
	{
		FrameStoreOutput::ViewTransform transform;
		transform.exposureEV = static_cast<float>(exposureEV);
		transform.toneCurve = displayXform;
		return transform;
	}

	FrameStoreOutput::FSColorSpace TargetSpace( const COLOR_SPACE colorSpace )
	{
		return colorSpace == eColorSpace_ROMMRGB_Linear ||
			colorSpace == eColorSpace_ProPhotoRGB ?
			FrameStoreOutput::FSColorSpace::ROMM_Linear :
			FrameStoreOutput::FSColorSpace::sRGB_Linear;
	}
}

DisplayTransformWriter::DisplayTransformWriter(
	IRasterImageWriter&  innerWriter,
	Scalar               exposureEV,
	DISPLAY_TRANSFORM    displayXform
	) :
	DisplayTransformWriter(innerWriter,
		LegacyViewTransform(exposureEV,displayXform),
		eColorSpace_Rec709RGB_Linear)

{
}

DisplayTransformWriter::DisplayTransformWriter(
	IRasterImageWriter& innerWriter,
	const FrameStoreOutput::ViewTransform& viewTransform_,
	COLOR_SPACE targetColorSpace_
	) :
	inner( innerWriter ),
	viewTransform( viewTransform_ ),
	targetColorSpace( targetColorSpace_ )
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
	// Alpha passes through untouched: it represents coverage /
	// transparency, not radiance, and tone-mapping it would
	// produce wrong compositing in downstream tools.
	double targetR = 0.0;
	double targetG = 0.0;
	double targetB = 0.0;
	FrameStoreOutput::ApplyViewTransformLinear(
		viewTransform,TargetSpace(targetColorSpace),true,
		c.base.r,c.base.g,c.base.b,targetR,targetG,targetB);

	RISEPel native;
	if( TargetSpace(targetColorSpace) == FrameStoreOutput::FSColorSpace::ROMM_Linear ) {
		ROMMRGBPel romm;
		romm.r = targetR;
		romm.g = targetG;
		romm.b = targetB;
		native = ColorUtils::ROMMRGBtoRec709RGB(romm);
	} else {
		native.r = targetR;
		native.g = targetG;
		native.b = targetB;
	}

	inner.WriteColor( RISEColor( native, c.a ), x, y );
}

void DisplayTransformWriter::EndWrite()
{
	inner.EndWrite();
}
