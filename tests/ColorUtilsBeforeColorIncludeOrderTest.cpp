//////////////////////////////////////////////////////////////////////
//
//  ColorUtilsBeforeColorIncludeOrderTest.cpp - DL-80 red-proof/
//    regression: a translation unit that includes "ColorUtils.h"
//    BEFORE "Color.h" must compile and behave correctly.
//
//  THE BUG THIS GUARDS AGAINST
//
//    ColorUtils.h, Color.h and SpectralPacket.h formed a genuine
//    include cycle: ColorUtils.h included Color.h (for `Scalar` and a
//    forward-declared `XYZPel`); Color.h included SpectralPacket.h;
//    and SpectralPacket.h included ColorUtils.h back (for
//    `ColorUtils::XYZFromNM`, used inside its own non-template
//    `GetXYZ()`).  Whichever of ColorUtils.h / Color.h a TU named
//    FIRST won the include-guard race: entering via Color.h happened
//    to work (SpectralPacket.h's nested `#include "ColorUtils.h"` ran
//    to completion before anything needed its declarations), but
//    entering via ColorUtils.h did not -- by the time control reached
//    SpectralPacket.h (nested inside Color.h, nested inside
//    ColorUtils.h's OWN first line), ColorUtils.h's include guard was
//    already set from the outermost entry, so SpectralPacket.h's
//    `#include "ColorUtils.h"` silently no-op'd and `GetXYZ()`'s call
//    to `ColorUtils::XYZFromNM` referenced a name nothing had declared
//    yet: `error: no member named 'XYZFromNM' in namespace
//    'RISE::ColorUtils'`.  Every production file happened to spell
//    `#include "Color.h"` before `#include "ColorUtils.h"` (or never
//    included ColorUtils.h directly at all), so the landmine was never
//    tripped in-tree -- until a NEW translation unit
//    (`PiecewiseLinearScalarPainter.cpp`, `tests/IScalarPainterTest.cpp`
//    in the sibling `precision` slice) spelled `#include
//    "ColorUtils.h"` alone, with no prior `Color.h`, and hit it.
//
//  THE FIX
//
//    SpectralPacket.h and SpectralPacket_Template.h no longer
//    `#include "ColorUtils.h"` at all -- they forward-declare the one
//    function they call (`ColorUtils::XYZFromNM`) directly, which is
//    all a call needs.  ColorUtils.h's own `#include "Color.h"` is
//    unchanged (many existing files rely on it transitively), but the
//    back-edge that completed the cycle is gone, so entry order no
//    longer matters.
//
//  WHAT THIS FILE DOES
//
//    Includes "ColorUtils.h" FIRST -- the order that used to fail --
//    then "Color.h", then exercises both a ColorUtils function and the
//    exact cross-header call this file's header comment describes
//    (SpectralPacket::GetXYZ() calling ColorUtils::XYZFromNM), proving
//    the fix is not merely "compiles" but functionally correct too.
//    Its sibling, ColorBeforeColorUtilsIncludeOrderTest.cpp, proves the
//    other (always-worked) order remains correct.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Utilities/Color/ColorUtils.h"
#include "../src/Library/Utilities/Color/Color.h"

#include <iostream>
#include <cmath>

using namespace RISE;

int main()
{
	int failures = 0;

	std::cout << "=== ColorUtilsBeforeColorIncludeOrderTest (DL-80) ===" << std::endl;

	// A plain ColorUtils function, declared directly by ColorUtils.h.
	const Scalar srgb = ColorUtils::SRGBTransferFunction( 0.5 );
	if( !std::isfinite( srgb ) || srgb <= 0.0 || srgb >= 1.0 ) {
		std::cerr << "FAIL: ColorUtils::SRGBTransferFunction(0.5) returned an unreasonable value ("
			<< srgb << ")" << std::endl;
		++failures;
	}

	// A Color.h type, only visible because ColorUtils.h's own
	// #include "Color.h" (unchanged by the fix) still supplies it.
	const RISEPel white( 1, 1, 1 );
	if( white.r != 1.0 || white.g != 1.0 || white.b != 1.0 ) {
		std::cerr << "FAIL: RISEPel construction after ColorUtils.h-first inclusion is wrong"
			<< std::endl;
		++failures;
	}

	// THE exact cross-header call this test exists to pin: SpectralPacket
	// (declared by Color.h, via its own SpectralPacket.h include) calling
	// ColorUtils::XYZFromNM from its non-template GetXYZ() -- the precise
	// site that failed to compile pre-fix when entered via ColorUtils.h
	// first.
	SpectralPacket packet;
	const XYZPel xyz = packet.GetXYZ();
	if( !std::isfinite( xyz.X ) || !std::isfinite( xyz.Y ) || !std::isfinite( xyz.Z ) ) {
		std::cerr << "FAIL: SpectralPacket::GetXYZ() (which calls ColorUtils::XYZFromNM) "
			"returned a non-finite value" << std::endl;
		++failures;
	}

	// A direct call to the exact function name the DL-80 error message
	// named as missing, confirming it is genuinely declared and linkable
	// from this translation unit's entry order.
	XYZPel direct( 0, 0, 0 );
	const bool ok = ColorUtils::XYZFromNM( direct, 555.0 );
	if( !ok || !std::isfinite( direct.X ) || !std::isfinite( direct.Y ) || !std::isfinite( direct.Z ) ) {
		std::cerr << "FAIL: ColorUtils::XYZFromNM(555nm) failed or returned a non-finite value"
			<< std::endl;
		++failures;
	}

	std::cout << "Failures: " << failures << std::endl;
	return failures == 0 ? 0 : 1;
}
