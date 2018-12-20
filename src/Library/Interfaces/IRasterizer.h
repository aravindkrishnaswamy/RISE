//////////////////////////////////////////////////////////////////////
//
//  IRasterizer.h - Defines an interface to a rasterizer.  This is what 
//    converts a 3D scene into a raster image which is basically something
//    viewable
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 2, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IRASTERIZER_
#define IRASTERIZER_

#include "IReference.h"
#include "IRasterizerOutput.h"
#include "IRasterizeSequence.h"
#include "IScene.h"
#include "ISampling2D.h"
#include "IEnumCallback.h"
#include "IProgressCallback.h"

namespace RISE
{
	class IRasterizer : public virtual IReference 
	{
	protected:
		IRasterizer(){};
		virtual ~IRasterizer(){};

	public:
		//! Adds a new rasterizer output
		virtual void AddRasterizerOutput(
			IRasterizerOutput* ro							///< [in] The rasterizer output to add
			) = 0;

		//! Frees all rasterizer outputs
		virtual void FreeRasterizerOutputs() = 0;

		//! Enumerates all the rasterizer outputs
		virtual void EnumerateRasterizerOutputs(
			IEnumCallback<IRasterizerOutput>& pFunc			///< [in] Callback functor to call with each rasterizer output
			) const = 0;

		//! Attaches the a particular scene
		virtual void AttachToScene( 
			const IScene* pScene							///< [in] Scene to attach to
			) = 0;

		//! Detaches from a scene
		virtual void DetachFromScene(
			const IScene* pScene							///< [in] Scene to detach from
			) = 0;

		//! Predicts the amount of time it will take to rasterize
		/// \return The predicted time to rasterize the entire scene in milliseconds
		virtual unsigned int PredictTimeToRasterizeScene( 
			const IScene& pScene,							///< [in] Scene to perform prediction on
			const ISampling2D& pSampling,					///< [in] Sampling kernel to use for prediction
			unsigned int* pActualTime						///< [out] Actual amount of time it will to fully rasterize the scene
			) const = 0;

		//! Rasterizes a scene
		virtual void RasterizeScene(
			const IScene& pScene,							///< [in] Scene to rasterize
			const Rect* pRect,								///< [in] Region in the scene to rasterize, if its NULL, rasterizes the entire scene
			IRasterizeSequence* pRasterSequence				///< [in] The sequence in which to rasterize the scene
			) const = 0;

		//! Rasterizes a scene using the scene animator for keyframes
		virtual void RasterizeSceneAnimation(
			const IScene& pScene,							///< [in] Scene to rasterize
			const Scalar time_start,						///< [in] Scene time to start rasterizing at
			const Scalar time_end,							///< [in] Scene time to finish rasterizing
			const unsigned int num_frames,					///< [in] Number of frames to rasterize
			const bool do_fields,							///< [in] Should the rasterizer render to fields ?
			const bool invert_fields,						///< [in] Should the fields be temporally inverted?
			const Rect* pRect,								///< [in] Region in the scene to rasterize, if its NULL, rasterizes the entire scene
			const unsigned int* specificFrame,				///< [in] Asks to render only a specific frame
			IRasterizeSequence* pRasterSequence				///< [in] Raster sequence (only used when high temporal sampling is enabled)
			) const = 0;

		//! Sets the progress callback
		virtual void SetProgressCallback(
			IProgressCallback* pFunc						///< [in] Callback functor to call to report progress
			) = 0;
	};
}


#endif
