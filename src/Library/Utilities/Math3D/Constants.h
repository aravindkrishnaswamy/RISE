//////////////////////////////////////////////////////////////////////
//
//  Constants.h - Contains math constants
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 15, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MATH_CONSTANTS_
#define MATH_CONSTANTS_

#ifdef INFINITY
#undef INFINITY
#endif

namespace RISE
{
	static const Scalar		PI			=			Scalar(3.1415926535897932384626433832795028841971);
	static const Scalar		INV_PI		=			Scalar(1.0 / PI);
	static const Scalar		TWO_PI		=			Scalar(2.0 * PI);
	static const Scalar		FOUR_PI		=			Scalar(4.0 * PI);
	static const Scalar		PI_OV_TWO	=			Scalar(PI * 0.5);
	static const Scalar		PI_OV_FOUR	=			Scalar(PI * 0.25);
	static const Scalar		PI_OV_THREE	=			Scalar(PI / 3.0);
	static const Scalar		E_			=			Scalar(2.718281828459045235360287471352662497);
	static const Scalar		THIRD		=			Scalar(1.0 / 3.0);
	static const Scalar		HALF		=			Scalar(1.0 / 2.0);
	static const Scalar		THREE_PI_OV_TWO =		Scalar(3.0 * PI_OV_TWO);
	static const Scalar		THREE_OV_TWO_PI =		Scalar(1.5 / PI);

	static const Scalar		INFINITY	=			Scalar(1.7976931348623158e+308);
	static const Scalar		NEARZERO	=			Scalar(1e-12);

	static const Scalar		DEG_TO_RAD	=			Scalar(PI / 180.0);
	static const Scalar		RAD_TO_DEG	=			Scalar(180.0 / PI);
}

#endif
