//////////////////////////////////////////////////////////////////////
//
//  RandomWalkSSS.h - Random-walk subsurface scattering sampler
//
//  Implements the volumetric random walk algorithm for subsurface
//  light transport (Chiang, Burley et al., SIGGRAPH 2016).  At a
//  surface entry point on a translucent object, the algorithm traces
//  a random walk inside the mesh geometry using Beer-Lambert
//  free-flight distance sampling and Henyey-Greenstein phase function
//  scattering.  When the walk exits the mesh, the exit point becomes
//  the re-emission vertex.
//
//  ALGORITHM OVERVIEW:
//    1. Refract the incoming ray into the surface using Snell's law.
//    2. Walk loop (up to maxBounces):
//       a. Trace ray from current position against the object with
//          back-face-only hits to find the exit distance.
//       b. Sample a spectral channel uniformly (RGB) or use the
//          single channel (NM mode).
//       c. Sample free-flight distance: t = -log(1-xi) / sigma_t[ch].
//       d. If t < exitDist: scatter inside.  Advance position, update
//          throughput, sample new direction from HG phase function.
//       e. If t >= exitDist: exit the mesh.  Compute Fresnel at exit.
//          If total internal reflection: reflect back inside and
//          continue.  Otherwise refract out and return the exit point.
//    3. At exit: generate a cosine-weighted scattered direction from
//       the exit normal and compute the BSSRDF weight.
//
//  ADVANTAGES OVER DISK PROJECTION:
//    - Correctly handles thin geometry (ears, noses, fingers) where
//      disk projection probe rays miss the surface.
//    - Naturally captures high-albedo diffusion without profile
//      approximation artifacts.
//    - Models geometry-dependent subsurface transport (light leaking
//      through thin features).
//
//  The output is a BSSRDFSampling::SampleResult, identical to the
//  disk-projection method, so integrator vertex plumbing is reused.
//
//  Reference:
//    Chiang, M. J., Burley, B., et al. (2016).
//    "A Practical and Controllable Hair and Fur Model for Production
//    Path Tracing." SIGGRAPH 2016.
//    (Section on Subsurface Scattering via Random Walk)
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
		/// Traces a random walk inside a mesh object and returns the
		/// exit point as a BSSRDFSampling::SampleResult.
		///
		/// The caller is responsible for the Fresnel coin flip at the
		/// surface (deciding to enter the medium vs reflect).  This
		/// function handles the interior walk and boundary exit only.
		///
		/// \param ri         Exit point intersection (where the camera
		///                   ray hit the surface).  Used as the walk
		///                   entry point after refraction.
		/// \param pObject    The translucent object to walk inside.
		/// \param sigma_a    Absorption coefficient per channel [1/m]
		/// \param sigma_s    Scattering coefficient per channel [1/m]
		/// \param sigma_t    Extinction coefficient per channel [1/m]
		/// \param g          Henyey-Greenstein asymmetry parameter
		/// \param ior        Index of refraction at the surface boundary
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
			const Scalar nm
			);
	}
}

#endif
