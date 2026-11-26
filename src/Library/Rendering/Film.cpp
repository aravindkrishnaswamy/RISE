//////////////////////////////////////////////////////////////////////
//
//  Film.cpp - Concrete implementation of IFilm.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 10, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Film.h"

using namespace RISE;
using namespace RISE::Implementation;

Film::Film(
	const unsigned int width_,
	const unsigned int height_,
	const Scalar pixelAR_
	) :
	width( width_ ),
	height( height_ ),
	pixelAR( pixelAR_ )
{
}
