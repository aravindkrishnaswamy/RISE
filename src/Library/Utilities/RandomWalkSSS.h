//////////////////////////////////////////////////////////////////////
//
//  RandomWalkSSS.h - Volumetric random-walk subsurface scattering
//    sampler (Chiang, Burley et al., SIGGRAPH 2016).
//
//  Refracts the incoming ray across the boundary using the relative
//  index (ior / ri.ambientIOR), walks the interior via Beer-Lambert
//  free-flight sampling and Henyey-Greenstein scattering, handles
//  internal Fresnel/TIR and crossings into touching random-walk
//  bodies (IObjectManager::RandomWalkObjectContaining), and emits a
//  BSSRDFSampling::SampleResult matching disk-projection plumbing.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 7, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RANDOM_WALK_SSS_
#define RANDOM_WALK_SSS_

#include "BSSRDFSampling.h"
#include "Math3D/Math3D.h"
#include "../Interfaces/IObject.h"
#include "ISampler.h"

namespace RISE
{
	namespace RandomWalkSSS
	{
		//! Winding-independent entry admission for solids that certify containment.
		Scalar EntryFacing( const RayIntersectionGeometric& ri, const IObject* object, const Vector3& wo );

		/// Traces a random walk inside a mesh object and returns the exit point
		/// as a BSSRDFSampling::SampleResult.  Caller owns the entry Fresnel coin
		/// flip (enter vs reflect); this function handles the interior walk and
		/// boundary exit only.
		///
		/// \param ri         Surface hit (walk entry point after refraction;
		///                   ri.ambientIOR supplies the exterior IOR).
		/// \param pObject    Entry object (result.pExitObject names the exited
		///                   body if the walk crosses into a touching RW neighbour).
		/// \param sigma_a    Absorption coefficient per channel [1/m]
		/// \param sigma_s    Scattering coefficient per channel [1/m]
		/// \param sigma_t    Extinction coefficient per channel [1/m]
		/// \param g          Henyey-Greenstein asymmetry parameter
		/// \param ior        Absolute material IOR (boundary uses ior / ri.ambientIOR)
		/// \param maxBounces Maximum walk steps (safety cap)
		/// \param sampler    Random number source
		/// \param nm         Wavelength for spectral path (0 = RGB)
		/// \return SampleResult with valid=true on success
		BSSRDFSampling::SampleResult SampleExit(
			const RayIntersectionGeometric& ri,
			const IObject* pObject,
			const RISEPel& sigma_a,
			const RISEPel& sigma_s,
			const RISEPel& sigma_t,
			const Scalar g,
			const Scalar ior,
			const unsigned int maxBounces,
			ISampler& sampler,
			const Scalar nm,
			const Scalar maxDepth = 0,
			const IMaterial* pExitLeafMaterial = 0,
			const IRayIntersectionModifier* pExitLeafModifier = 0
			);
	}
}

#endif
