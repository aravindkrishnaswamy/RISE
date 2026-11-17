//////////////////////////////////////////////////////////////////////
//
//  ColorBeforeColorUtilsIncludeOrderTest.cpp - DL-80 regression: the
//    "Color.h" -before- "ColorUtils.h" order (the one every existing
//    production file happened to use, and which always compiled) must
//    keep compiling and behaving correctly after the fix.
//
//  See ColorUtilsBeforeColorIncludeOrderTest.cpp (this file's sibling)
//  for the full DL-80 background: SpectralPacket.h/
//  SpectralPacket_Template.h no longer `#include "ColorUtils.h"`
//  (forward-declaring the one function they call instead), which
//  removes the cycle entirely -- this file pins that the OTHER entry
//  order, which the fix must not regress, still works exactly as
//  before.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Color/ColorUtils.h"

#include <iostream>
#include <cmath>

using namespace RISE;

int main()
{
	int failures = 0;

	std::cout << "=== ColorBeforeColorUtilsIncludeOrderTest (DL-80) ===" << std::endl;

	const RISEPel white( 1, 1, 1 );
	if( white.r != 1.0 || white.g != 1.0 || white.b != 1.0 ) {
		std::cerr << "FAIL: RISEPel construction after Color.h-first inclusion is wrong" << std::endl;
		++failures;
	}

	const Scalar srgb = ColorUtils::SRGBTransferFunction( 0.5 );
	if( !std::isfinite( srgb ) || srgb <= 0.0 || srgb >= 1.0 ) {
		std::cerr << "FAIL: ColorUtils::SRGBTransferFunction(0.5) returned an unreasonable value ("
			<< srgb << ")" << std::endl;
		++failures;
	}

	SpectralPacket packet;
	const XYZPel xyz = packet.GetXYZ();
	if( !std::isfinite( xyz.X ) || !std::isfinite( xyz.Y ) || !std::isfinite( xyz.Z ) ) {
		std::cerr << "FAIL: SpectralPacket::GetXYZ() returned a non-finite value" << std::endl;
		++failures;
	}

	std::cout << "Failures: " << failures << std::endl;
	return failures == 0 ? 0 : 1;
}
