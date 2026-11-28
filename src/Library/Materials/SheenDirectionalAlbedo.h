//////////////////////////////////////////////////////////////////////
//
//  SheenDirectionalAlbedo.h - Baked directional- and bihemispherical-
//  albedo tables for the bare achromatic Charlie sheen lobe
//  (CharlieSheen.h; docs/CLOTH_FABRIC_DESIGN.md §9.2, §9.4).
//
//  Generated deterministically (adaptive quadrature, no MC) by
//  tools/SheenDirectionalAlbedoGen.cpp into SheenDirectionalAlbedo_LUTData.cpp.
//
//  Tables & load-bearing invariants:
//  - E(alpha, cosTheta): directional-hemispherical albedo of bare Charlie
//    D*V*(n.l). Returned UNCLAMPED: the Estevez & Kulla fit exceeds 1 near
//    grazing (~1.195 at alpha = 0.04, mu ~ 0.004), so the consumer
//    (FabricBRDF) bounds it via kMinSheenAlpha = 0.04, Ehat = min(E, 1),
//    and SheenNormaliser.
//  - EHatMean(alpha): hemispherical mean 2 * int_0^1 min(E, 1) * mu dmu
//    of the CLAMPED lobe (including constant extrapolation over [0, mu1]),
//    matching FabricBRDF::BaseScaling's denominator.
//  - Axes: alpha is log-spaced on [1e-3, 1.0] (matching CharlieSheen::D's
//    internal floor); cosTheta is grazing-warped mu_j = (j/(N-1))^2 so
//    node 1 sits at mu1 = 1/3969 (resolving the grazing peak).
//  - Domain [mu1, 1] with constant extrapolation below mu1: E() floors
//    cosTheta at mu1 because interpolating to the exact E(0)=0 node
//    severely under-reads the grazing peak. Blending within a cell is
//    log-alpha x log(mu). Note: for alpha <~ 0.05 and mu <~ 1e-6,
//    CharlieSheen::V's n.l*n.v < 1e-6 cutoff drops the lobe to 0 while
//    floored E(mu1) >= 1 suppresses the base (black step, energy loss).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SHEEN_DIRECTIONAL_ALBEDO_
#define SHEEN_DIRECTIONAL_ALBEDO_

#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	namespace SheenDirectionalAlbedo
	{
		//! Table extents shared by this header and SheenDirectionalAlbedoGen.cpp;
		//! checked at compile time by static_asserts below.
		static const unsigned int kNumAlphaBins    = 64;
		static const unsigned int kNumCosThetaBins = 64;

		//! Alpha axis endpoints.  Defined in SheenDirectionalAlbedo_LUTData.cpp
		//! alongside the tables themselves.
		extern const float kAlphaMin;	//!< 1e-3f -- matches CharlieSheen::D's internal floor
		extern const float kAlphaMax;	//!< 1.0f

		//! E(alpha, cosTheta), row-major [alphaIdx][cosThetaIdx].
		extern const float kETable[ kNumAlphaBins ][ kNumCosThetaBins ];

		//! EHatMean(alpha) -- the mean of min(E, 1), not of E.
		extern const float kEHatMeanTable[ kNumAlphaBins ];

		//! Compile-time checks keeping regenerated table extents in sync.
		static_assert( sizeof( kETable ) / sizeof( kETable[0] ) == kNumAlphaBins,
		               "kETable alpha extent must match kNumAlphaBins" );
		static_assert( sizeof( kETable[0] ) / sizeof( float ) == kNumCosThetaBins,
		               "kETable cosTheta extent must match kNumCosThetaBins" );
		static_assert( sizeof( kEHatMeanTable ) / sizeof( float ) == kNumAlphaBins,
		               "kEHatMeanTable extent must match kNumAlphaBins" );

		//! Directional-hemispherical albedo of bare Charlie at (alpha, cosTheta).
		//! Floors cosTheta at mu1 = 1/(N-1)^2, interpolates log-alpha x log(mu),
		//! and returns UNCLAMPED values (may exceed 1 near grazing; bounded by
		//! FabricBRDF).
		Scalar E( Scalar alpha, Scalar cosTheta );

		//! Hemispherical mean of Ehat = min(E, 1) at `alpha`.  Linear
		//! interpolation (log-alpha), clamped at the table edges.  See
		//! the file header for why the mean is of the CLAMPED lobe.
		Scalar EHatMean( Scalar alpha );
	}
}

#endif
