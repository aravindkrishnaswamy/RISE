//////////////////////////////////////////////////////////////////////
//
//  ControlledSmoothness2DPainter.h - Test painter that emits a single
//  radially-symmetric bump with controllable boundary smoothness order.
//  Designed for the SMS-investigation experiment that disentangles
//  per-vertex height delta from per-edge normal-angle delta in
//  Newton-fail rate on displaced meshes.
//
//  Same `(center, radius, amplitude)` across all variants → matched
//  vertex value at the bump apex.  What differs is the FALLOFF
//  function's continuity order:
//
//      heaviside  : value jumps from `amplitude` to 0 at r = radius
//                   (C⁻¹ in value).
//      tent       : amplitude · (1 − r/radius)
//                   (C⁰; kink at r=0 and r=radius).
//      quadratic  : amplitude · (1 − r/radius)²
//                   (C¹; second derivative jumps at r=radius).
//      cubic      : amplitude · smoothstep_cubic(1 − r/radius)
//                   (C¹; same C¹ continuity as quadratic but smoother
//                   shape inside).
//      quintic    : amplitude · smoothstep_quintic(1 − r/radius)
//                   (C³; standard Perlin-style "smootherstep").
//      gaussian   : amplitude · exp(−(r/radius)²/falloff_sigma²)
//                   (C^∞; never reaches exactly zero).
//
//  Implements both `IPainter` (interpolates between colora and colorb
//  based on the bump value, normalised to [0,1]) and `IFunction2D`
//  (the hook `displaced_geometry { displacement <name> }` calls per
//  vertex).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-05-04
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CONTROLLED_SMOOTHNESS_2D_PAINTER_
#define CONTROLLED_SMOOTHNESS_2D_PAINTER_

#include "Painter.h"

namespace RISE
{
	namespace Implementation
	{
		class ControlledSmoothness2DPainter : public Painter
		{
		public:
			enum SmoothnessMode
			{
				eHeaviside = 0,	///< C⁻¹: value step at r=radius
				eTent      = 1,	///< C⁰: linear falloff
				eQuadratic = 2,	///< C¹ at r=radius via (1−r)²
				eCubic     = 3,	///< C¹ via Hermite smoothstep
				eQuintic   = 5,	///< C³ via Hermite smootherstep
				eGaussian  = 99	///< C^∞ via exp(−r²)
			};

		protected:
			virtual ~ControlledSmoothness2DPainter();

			const IPainter&	a;
			const IPainter&	b;

			Scalar			centerU;
			Scalar			centerV;
			Scalar			radius;
			Scalar			amplitude;
			SmoothnessMode	mode;

		public:
			ControlledSmoothness2DPainter(
				const IPainter&		cA_,
				const IPainter&		cB_,
				const Scalar		centerU_,
				const Scalar		centerV_,
				const Scalar		radius_,
				const Scalar		amplitude_,
				const SmoothnessMode mode_ );

			ControlledSmoothness2DPainter( const ControlledSmoothness2DPainter& ) = delete;
			ControlledSmoothness2DPainter& operator=( const ControlledSmoothness2DPainter& ) = delete;

			RISEPel	GetColor( const RayIntersectionGeometric& ri ) const;
			Scalar	GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			//! IFunction2D — `displaced_geometry` calls this per mesh vertex.
			Scalar	Evaluate( const Scalar x, const Scalar y ) const;

			//! Keyframable interface (no animated parameters in v1).
			IKeyframeParameter*	KeyframeFromParameters( const String& name, const String& value );
			void				SetIntermediateValue( const IKeyframeParameter& val );
			void				RegenerateData();
		};
	}
}

#endif
