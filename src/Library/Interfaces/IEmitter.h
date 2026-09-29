//////////////////////////////////////////////////////////////////////
//
//  IEmitter.h - Defines an interface to an emitter
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 21, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IEMITTER_
#define IEMITTER_

#include "IReference.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/OrthonormalBasis3D.h"
#include "../Utilities/Color/Color.h"
#include "../Intersection/RayIntersectionGeometric.h"

namespace RISE
{
	//! This is the interface for luminaries
	/// \sa IMaterial
	class IEmitter : public virtual IReference
	{
	protected:
		IEmitter(){};
		virtual ~IEmitter(){};

	public:
		//! Flux per solid angle per unit projected area (Watt/m^2sr)
		//! So this function would, given the outgoing vector out and the normal N
		/// \return The emitted radiance for each color component as an IFXPel
		virtual RISEPel emittedRadiance(
			const RayIntersectionGeometric& ri,							///< [in] Geometric intersection information
			const Vector3& out,											///< [in] Outgoing vector from the surface of the luminary
			const Vector3& N											///< [in] Normal of the luminary
			) const = 0;

		//! Returns the radiance for a particular wavelength
		/// \return The emitted radiance for the parituclar wavelength as a scalar
		virtual Scalar emittedRadianceNM( 
			const RayIntersectionGeometric& ri,							///< [in] Geometric intersection information
			const Vector3& out,											///< [in] Outgoing vector from the surface of the luminary
			const Vector3& N,											///< [in] Normal of the luminary
			const Scalar nm												///< [in] Wavelength to process
			) const = 0;

		//! Average amount of radiative flux leaving any point on the surface into all
		//! directions above the surface.  Flux is the radiant energy flowing through a surface per unit time
		//! ie Watt = Joules / sec )
		/// \return The flux for each component as an IFXPel
		virtual RISEPel averageRadiantExitance(
			) const	= 0;

		//! Same as above except returns the radiant exitance for a particular wavelength
		/// \return The flux for the wavelength as a scalar
		virtual Scalar averageRadiantExitanceNM(
			const Scalar nm												///< [in] Wavelength to process
			) const = 0;

		//! Returns a random emmited photon direction for this material, assuming
		//! the material has emmisive properties
		/// \return Vector which represents the direction of photon emmision
		virtual Vector3 getEmmittedPhotonDir( 
			const RayIntersectionGeometric& ri,							///< [in] Geometric intersection information
			const Point2& random										///< [in] Two random variables which determine the perturbation of the photon emmision vector
			) const	= 0;
	};

	//! DL-320: the SIDEDNESS of an area emitter, shared by every strategy
	//! that samples or prices one.  docs/DL320_DOUBLE_SIDED_EMITTER.md.
	//!
	//! An `IEmitter` is one-sided about the normal it is HANDED
	//! (`LambertianEmitter::emittedRadiance` returns 0 when
	//! `Dot(out, N) <= 0`).  On geometry whose `IGeometry::IsDoubleSided()`
	//! is true a HIT hands it the RAY-FACING normal, so the emitter shines
	//! from both faces for every strategy that hits it.  Every strategy
	//! that instead SAMPLES a point on it (NEE, a light-subpath root, a
	//! photon) only has the winding normal `UniformRandomPoint` returned,
	//! and must turn it into the same two-faced source with these helpers:
	//!
	//!   * FaceToward -- the face an outgoing direction leaves from.  Hand
	//!     it to `emittedRadiance{,NM}` and to any per-face record.
	//!   * CosineEmissionPdf -- the solid-angle density of the light-
	//!     subpath emission sampler: a cosine lobe about the winding normal
	//!     for a one-sided emitter; for a two-sided one, a face chosen with
	//!     probability 1/2 and a cosine lobe about it, i.e. |cos|/(2 pi).
	//!     The hit-side MIS partners (BDPT s = 0, s = 1, t = 1; VCM S0 and
	//!     NEE) must use the SAME function the sampler draws from.
	//!   * FaceCount -- how many faces radiate: the emitter's total power
	//!     (selection PMFs, photon budgets) is `M * A * FaceCount`.
	//!
	//! With `twoSided == false` every helper is the pre-DL-320 one-sided
	//! expression, bit for bit.
	namespace EmitterSides
	{
		inline Vector3 FaceToward( const bool twoSided, const Vector3& n, const Vector3& out )
		{
			return ( twoSided && Vector3Ops::Dot( out, n ) < 0 ) ? -n : n;
		}

		inline Scalar CosineEmissionPdf( const bool twoSided, const Vector3& n, const Vector3& out )
		{
			const Scalar c = Vector3Ops::Dot( n, out );
			if( twoSided ) {
				return ( c < 0 ? -c : c ) * INV_PI * Scalar( 0.5 );
			}
			return ( c > 0 ) ? ( c * INV_PI ) : Scalar( 0 );
		}

		inline Scalar FaceCount( const bool twoSided )
		{
			return twoSided ? Scalar( 2 ) : Scalar( 1 );
		}
	}
}

#endif
