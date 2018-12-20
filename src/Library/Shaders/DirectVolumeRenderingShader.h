//////////////////////////////////////////////////////////////////////
//
//  DirectVolumeRenderingShader.h - A shader that employs the Kajiya path
//    tracing algorithm to compute shade
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DIRECT_VOLUME_RENDERING_SHADER_
#define DIRECT_VOLUME_RENDERING_SHADER_

#include "../Interfaces/IShader.h"
#include "../Interfaces/ISampling1D.h"
#include "../Interfaces/IFunction1D.h"
#include "../Interfaces/IVolumeOperation.h"
#include "../Interfaces/IVolumeAccessor.h"
#include "../Interfaces/IGradientEstimator.h"
#include "../Utilities/Reference.h"
#include "../Volume/TransferFunctions.h"

namespace RISE
{
	namespace Implementation
	{
		class DirectVolumeRenderingShader : 
			public virtual IShader, 
			public virtual Reference
		{
		protected:
			virtual ~DirectVolumeRenderingShader();

			IVolumeAccessor*			pVolume;						///< [in] The volume interpolator and its attached volume
			ISampling1D&				pSampler;						///< [in] The volume sampler to use
			IGradientEstimator*			pGradientEstimator;				///< [in] The gradient estimator
			IVolumeOperation*			pComposite;						///< [in] The type of composite operator
			TransferFunctions*			pTransferFunctions;				///< [in] The transfer functions
			SpectralTransferFunctions*	pSpectralTransferFunctions;		///< [in] The spectral transfer functions
			const IShader*				pISOShader;						///< [in] Shader for ISO surfaces


		public:
			DirectVolumeRenderingShader(
				const char* szVolumeFilePattern,
				const unsigned int width,
				const unsigned int height, 
				const unsigned int startz,
				const unsigned int endz,
				const char accessor,
				const char gradient,
				const char composite,
				const Scalar dThresholdStart,
				const Scalar dThresholdEnd,
				ISampling1D& sampler,
				const IFunction1D& red,
				const IFunction1D& green,
				const IFunction1D& blue,
				const IFunction1D& alpha,
				const IShader* pISOShader
				);

			DirectVolumeRenderingShader(
				const char* szVolumeFilePattern,
				const unsigned int width,
				const unsigned int height, 
				const unsigned int startz,
				const unsigned int endz,
				const char accessor,
				const char gradient,
				const char composite,
				const Scalar dThresholdStart,
				const Scalar dThresholdEnd,
				ISampling1D& sampler,
				const IFunction1D& red,
				const IFunction2D& spectral,
				const IShader* pISOShader_
				);

			//! Tells the shader to apply shade to the given intersection point
			void Shade(
				const RuntimeContext& rc,					///< [in] The runtime context
				const RayIntersection& ri,					///< [in] Intersection information 
				const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
				const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
				RISEPel& c,									///< [out] RISEPel value at the point
				const IORStack* const ior_stack				///< [in/out] Index of refraction stack
				) const;

			//! Tells the shader to apply shade to the given intersection point for the given wavelength
			/// \return Amplitude of spectral function 
			Scalar ShadeNM(
				const RuntimeContext& rc,					///< [in] The runtime context
				const RayIntersection& ri,					///< [in] Intersection information 
				const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
				const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
				const Scalar nm,							///< [in] Wavelength to shade
				const IORStack* const ior_stack				///< [in/out] Index of refraction stack
				) const;

			//! Tells the shader to reset itself
			void ResetRuntimeData() const;
		};
	}
}

#endif

