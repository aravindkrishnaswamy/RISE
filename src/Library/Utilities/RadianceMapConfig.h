//////////////////////////////////////////////////////////////////////
//
//  RadianceMapConfig.h - Parser-facing configuration for a radiance
//    map (HDRI / environment map / image-based light).
//
//    A radiance map is a painter applied as a spherical environment:
//    rays that exit the scene (or strike a configured background
//    object) sample radiance from that painter instead of the default
//    ambient/zero value.  It is the standard route to image-based
//    lighting and HDR sky domes.
//
//    The same four knobs appear in two distinct call paths:
//      - Per-object, via `AddObject` / `AddCSGObject`: the radiance
//        map decorates a specific object (useful for localised IBL).
//        `isBackground` is irrelevant for per-object maps and is
//        ignored by that call site.
//      - Per-rasterizer, via every `Set*Rasterizer`: the scene-wide
//        global radiance map.  `isBackground` controls whether
//        primary/camera rays that miss geometry display the map
//        (true = IBL visible; false = black background with IBL
//        still illuminating from indirect bounces).
//
//    The default-constructed configuration disables IBL (`name =
//    "none"`), matching the legacy parser behaviour where scenes
//    without a `radiance_map` parameter render with no environment.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 22, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RADIANCE_MAP_CONFIG_
#define RADIANCE_MAP_CONFIG_

#include "../Interfaces/IReference.h"
#include "RString.h"

namespace RISE
{
	struct RadianceMapConfig
	{
		String			name;				///< Painter name for the radiance map.  "none" (default) disables — no map is installed and rays that miss geometry return zero.
		Scalar			scale;				///< Scalar multiplier applied to radiance samples.  Default 1.0.
		Scalar			orientation[3];		///< Euler angles (radians) rotating the map in world space.  Parser converts the scene file's degrees via DEG_TO_RAD before writing here.  Default (0, 0, 0).
		bool			isBackground;		///< Primary-ray visibility: true (default) makes the map appear in the background of the render; false makes missed rays black while the map still contributes to indirect illumination.  Ignored by AddObject / AddCSGObject (per-object maps never act as the background).

		RadianceMapConfig() :
		  name( "none" ),
		  scale( 1.0 ),
		  isBackground( true )
		{
			orientation[0] = 0.0;
			orientation[1] = 0.0;
			orientation[2] = 0.0;
		}
	};
}

#endif
