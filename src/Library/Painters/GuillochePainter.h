//////////////////////////////////////////////////////////////////////
//
//  GuillochePainter.h - The guilloché oxide-dose IFunction2D painter
//  (the procedural replacement for the baked oxide_*.png maps) plus
//  the descriptor -> GuillocheParams converter.
//
//  GuillocheOxidePainter::Evaluate(u, v) interprets (u, v) as the
//  dial's LINEAR CARTESIAN UV in [0,1]^2 (u = (x+R)/2R, v = (y+R)/2R,
//  the same layout the baked PNGs used and a cartesian_disk_geometry
//  emits), converts to dial-space (x, y), and returns the normalized
//  oxide DOSE in [0,1]: the Arrhenius/parabolic radial profile of
//  thermal_oxide_sim.build_thickness_profile plus the signed
//  torch-pattern term of apply_torch_pattern.  Consume it through
//  `scalar_painter { function2d <name> scale S bias B }` exactly as
//  the PNG went through the texture form -- the scene's heat-tint
//  scale/bias semantics are unchanged.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GUILLOCHE_PAINTER_
#define GUILLOCHE_PAINTER_

#include "GuillocheField.h"
#include "../Interfaces/IFunction2D.h"
#include "../Interfaces/ProceduralDescriptors.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		//! Maps the public chunk-facing descriptor onto the field params.
		GuillocheParams GuillocheParamsFromDescriptor( const GuillocheDiskDescriptor& d );

		class GuillocheOxidePainter :
			public virtual IFunction2D,
			public virtual Reference
		{
		public:
			//! Output mode.  eDose = the normalized [0,1] heat-tint dose
			//! (scene scale/bias -> nm; the hero-watch path).  eThicknessNm
			//! and eSpallMask are the ABSOLUTE-temperature temper-comparison
			//! modes: an absolute radial ramp tempCenterC -> tempRimC drives
			//! the metal's calibrated thickness (nm, consumed directly) or its
			//! spall fraction [0,1] (the matte-scale blend mask).
			enum Mode { eDose, eThicknessNm, eSpallMask };

			//! Normalized-dose ctor.  falloffMode: 0 linear, 1 quadratic,
			//! 2 smooth (the radial heat falloff).  activationEa in J/mol
			//! (per-metal curvature; see GuillocheField::MetalEa).
			//! torchAmount: signed extra dwell along the pattern's torch mask
			//! (0 = uniform radial dose, the oxide_cart.png equivalent).
			GuillocheOxidePainter( const GuillocheParams& params,
			                       const int falloffMode,
			                       const Scalar activationEa,
			                       const Scalar torchAmount );

			//! Absolute-temperature temper ctor (eThicknessNm / eSpallMask):
			//! the radial falloff maps the disk onto the absolute ramp
			//! tempCenterC -> tempRimC and the metal's thermal model yields
			//! either absolute nm or the spall mask.
			GuillocheOxidePainter( const GuillocheParams& params,
			                       const int falloffMode,
			                       const Mode mode,
			                       const Scalar tempCenterC,
			                       const Scalar tempRimC,
			                       const GuillocheField::MetalThermal& thermal );

			Scalar Evaluate( const Scalar x, const Scalar y ) const;

		protected:
			virtual ~GuillocheOxidePainter();

			GuillocheField m_field;
			int            m_falloffMode;
			Mode           m_mode;
			Scalar         m_activationEa;
			Scalar         m_torchAmount;
			Scalar         m_g0;			//!< ArrheniusG(T0, Ea) -- Ea-only, hoisted from the per-query path
			Scalar         m_gSpan;			//!< ArrheniusG(T1, Ea) - m_g0
			Scalar         m_tempCenterC;	//!< absolute-temperature modes: ramp endpoints
			Scalar         m_tempRimC;
			GuillocheField::MetalThermal m_thermal;
		};
	}
}

#endif
