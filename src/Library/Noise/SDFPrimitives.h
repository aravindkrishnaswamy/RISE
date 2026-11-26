//////////////////////////////////////////////////////////////////////
//
//  SDFPrimitives.h - Defines 3D signed distance field primitives.
//  Evaluates SDF for geometric shapes (sphere, box, torus, cylinder)
//  with smooth boolean operations and optional noise displacement
//  for pyroclastic effects.
//
//  Reference: Inigo Quilez, "Distance Functions"
//  https://iquilezles.org/articles/distfunctions/
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SDF_PRIMITIVES_
#define SDF_PRIMITIVES_

#include "../Interfaces/IFunction3D.h"
#include "../Utilities/Reference.h"
#include "PerlinNoise.h"

namespace RISE
{
	namespace Implementation
	{
		/// SDF primitive type
		enum SDFPrimitiveType
		{
			eSDF_Sphere = 0,		///< Sphere (radius parameter)
			eSDF_Box = 1,			///< Box (half-extents)
			eSDF_Torus = 2,			///< Torus (major radius, minor radius)
			eSDF_Cylinder = 3		///< Cylinder (radius, half-height)
		};

		class SDFPrimitive3D : public virtual IFunction3D, public virtual Reference
		{
		protected:
			virtual ~SDFPrimitive3D();

			SDFPrimitiveType	eType;
			Scalar				dParam1;		///< Primary parameter (radius, etc.)
			Scalar				dParam2;		///< Secondary parameter (minor radius, half-height)
			Scalar				dParam3;		///< Tertiary parameter (box z-extent)
			Scalar				dShellThickness;///< Shell thickness (0 = solid)
			Scalar				dNoiseAmplitude;///< Noise displacement amplitude
			Scalar				dNoiseFrequency;///< Noise displacement frequency
			PerlinNoise3D*		pNoise;			///< Optional noise for displacement

		public:
			SDFPrimitive3D(
				const SDFPrimitiveType eType_,
				const Scalar dParam1_,
				const Scalar dParam2_,
				const Scalar dParam3_,
				const Scalar dShellThickness_,
				const Scalar dNoiseAmplitude_,
				const Scalar dNoiseFrequency_,
				const RealSimpleInterpolator& interp
			);

			/// Evaluates the SDF-based density at (x,y,z).
			/// Returns a value in [0, 1] where 1 = inside/on surface,
			/// 0 = far outside.
			virtual Scalar Evaluate( const Scalar x, const Scalar y, const Scalar z ) const;
		};
	}
}

#endif
