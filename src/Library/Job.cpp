//////////////////////////////////////////////////////////////////////
//
//  Job.cpp - Implementation of a job
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 6, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Job.h"
#include "RISE_API.h"
#include "Utilities/RString.h"
#include <stdio.h>
#include "Utilities/MediaPathLocator.h"
#include "Interfaces/IOptions.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	//! Creates a new empty job
	bool RISE_CreateJob(
			IJob** ppi										///< [out] Pointer to recieve the job
			)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new Job();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "job" );

		return true;
	}

	bool RISE_CreateJobPriv(
			IJobPriv** ppi										///< [out] Pointer to recieve the job
			)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new Job();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "job" );

		return true;
	}
}


Job::Job( )
{
	InitializeContainers();
}

Job::~Job( )
{
	DestroyContainers();
}

void Job::InitializeContainers()
{
	// Create empty internal objects
	pRasterizer = 0;
	pGlobalProgress = 0;

	RISE_API_CreateScene( &pScene );
	RISE_API_CreateGeometryManager( &pGeomManager );
	RISE_API_CreateMaterialManager( &pMatManager );
	RISE_API_CreateShaderManager( &pShaderManager );
	RISE_API_CreateShaderOpManager( &pShaderOpManager );
	RISE_API_CreatePainterManager( &pPntManager );
	RISE_API_CreateFunction1DManager( &pFunc1DManager );
	RISE_API_CreateFunction2DManager( &pFunc2DManager );
	RISE_API_CreateObjectManager( &pObjectManager, false, false, 0, 0 );
	RISE_API_CreateLightManager( &pLightManager );
	RISE_API_CreateModifierManager( &pModManager );

	pScene->SetObjectManager( pObjectManager );
	pScene->SetLightManager( pLightManager );

	// Adding null painter and null material
	{
		IMaterial* pNullMaterial = 0;
		RISE_API_CreateNullMaterial( &pNullMaterial );
		pMatManager->AddItem( pNullMaterial, "none" );
		safe_release( pNullMaterial );

		IPainter* pNullPainter = 0;
		RISE_API_CreateUniformColorPainter( &pNullPainter, RISEPel(0,0,0) );
		pPntManager->AddItem( pNullPainter, "none" );
		safe_release( pNullPainter );
	}

	// Adding some nice default shader ops
	{
		this->AddReflectionShaderOp( "DefaultReflection" );
		this->AddRefractionShaderOp( "DefaultRefraction" );
		this->AddEmissionShaderOp( "DefaultEmission" );
		this->AddDirectLightingShaderOp( "DefaultDirectLighting", 0, true, true, false );
		this->AddCausticPelPhotonMapShaderOp( "DefaultCausticPelPhotonMap" );
		this->AddCausticSpectralPhotonMapShaderOp( "DefaultCausticSpectralPhotonMap" );
		this->AddGlobalPelPhotonMapShaderOp( "DefaultGlobalPelPhotonMap" );
		this->AddGlobalSpectralPhotonMapShaderOp( "DefaultGlobalSpectralPhotonMap" );
		this->AddTranslucentPelPhotonMapShaderOp( "DefaultTranslucentPelPhotonMap" );
		this->AddShadowPhotonMapShaderOp( "DefaultShadowPhotonMap" );
	}
}

void Job::DestroyContainers()
{
	safe_shutdown_and_release( pScene );
	safe_release( pRasterizer );

	safe_shutdown_and_release( pGeomManager );
	safe_shutdown_and_release( pPntManager );
	safe_shutdown_and_release( pFunc1DManager );
	safe_shutdown_and_release( pFunc2DManager );
	safe_shutdown_and_release( pLightManager );
	safe_shutdown_and_release( pMatManager );
	safe_shutdown_and_release( pModManager );
	safe_shutdown_and_release( pShaderManager );
	safe_shutdown_and_release( pShaderOpManager );
	safe_shutdown_and_release( pObjectManager );
}

//
// Core settings
//

//! Resets the acceleration structure
//! WARNING!  Call this before adding objects, otherwise you will LOSE them!
//! \return TRUE if successful, FALSE otherwise
bool Job::SetPrimaryAcceleration(
	const bool bUseBSPtree,									///< [in] Use BSP trees for spatial partitioning
	const bool bUseOctree,									///< [in] Use Octrees for spatial partitioning
	const unsigned int nMaxObjectsPerNode,					///< [in] Maximum number of elements / node
	const unsigned int nMaxTreeDepth						///< [in] Maximum tree depth
	)
{
	if( pObjectManager ) {
		pObjectManager->Shutdown();
		pObjectManager->release();
		pObjectManager = 0;
	}

	RISE_API_CreateObjectManager( &pObjectManager, bUseBSPtree, bUseOctree, nMaxObjectsPerNode, nMaxTreeDepth );
	pScene->SetObjectManager( pObjectManager );

	return true;
}


//
// Sets the camera
//

//! Sets a pinhole camera
/// \return TRUE if successful, FALSE otherwise
bool Job::SetPinholeCamera(
	const double ptLocation[3],								///< [in] Absolute location of where the camera is located
	const double ptLookAt[3], 								///< [in] Absolute point the camera is looking at
	const double vUp[3],									///< [in] Up vector of the camera
	const double fov,										///< [in] Field of view in radians
	const unsigned int xres,								///< [in] X resolution of virtual screen
	const unsigned int yres,								///< [in] Y resolution of virtual screen
	const double pixelAR,									///< [in] Pixel aspect ratio
	const double exposure,									///< [in] Exposure time of the camera
	const double scanningRate,								///< [in] Rate at which each scanline is recorded
	const double pixelRate,									///< [in] Rate at which each pixel is recorded
	const double orientation[3],							///< [in] Orientation (Pitch,Roll,Yaw)
	const double target_orientation[2]						///< [in] Orientation relative to a target
	)
{
	ICamera* pCamera = 0;
	RISE_API_CreatePinholeCamera( &pCamera, Point3(ptLocation), Point3(ptLookAt), Vector3(vUp), fov, xres, yres, pixelAR, exposure, scanningRate, pixelRate, Vector3(orientation), Vector2(target_orientation) );
	pScene->SetCamera( pCamera );
	safe_release( pCamera );
	return true;
}

//! Sets a pinhole camera
/// \return TRUE if successful, FALSE otherwise
bool Job::SetPinholeCameraONB(
	const double ONB_U[3],									///< [in] U vector of orthonormal basis
	const double ONB_V[3],									///< [in] V vector of orthonormal basis
	const double ONB_W[3],									///< [in] W vector of orthonormal basis
	const double ptLocation[3],								///< [in] Absolute location of where the camera is located
	const double fov,										///< [in] Field of view in radians
	const unsigned int xres,								///< [in] X resolution of virtual screen
	const unsigned int yres,								///< [in] Y resolution of virtual screen
	const double pixelAR,									///< [in] Pixel aspect ratio
	const double exposure,									///< [in] Exposure time of the camera
	const double scanningRate,								///< [in] Rate at which each scanline is recorded
	const double pixelRate									///< [in] Rate at which each pixel is recorded
	)
{
	OrthonormalBasis3D onb = OrthonormalBasis3D( Vector3(ONB_U), Vector3(ONB_V), Vector3(ONB_W) );
	ICamera* pCamera = 0;
	RISE_API_CreatePinholeCameraONB( &pCamera, onb, Point3(ptLocation), fov, xres, yres, pixelAR, exposure, scanningRate, pixelRate );
	pScene->SetCamera( pCamera );
	safe_release( pCamera );
	return true;
}

//! Sets a camera based on thin lens model
/// \return TRUE if successful, FALSE otherwise
bool Job::SetThinlensCamera(
	const double ptLocation[3],								///< [in] Absolute location of where the camera is located
	const double ptLookAt[3], 								///< [in] Absolute point the camera is looking at
	const double vUp[3],									///< [in] Up vector of the camera
	const double fov,										///< [in] Field of view in radians
	const unsigned int xres,								///< [in] X resolution of virtual screen
	const unsigned int yres,								///< [in] Y resolution of virtual screen
	const double pixelAR,									///< [in] Pixel aspect ratio
	const double exposure,									///< [in] Exposure time of the camera
	const double scanningRate,								///< [in] Rate at which each scanline is recorded
	const double pixelRate,									///< [in] Rate at which each pixel is recorded
	const double orientation[3],							///< [in] Orientation (Pitch,Roll,Yaw)
	const double target_orientation[2],						///< [in] Orientation relative to a target
	const double aperture,									///< [in] Size of the aperture
	const double focalLength,								///< [in] Focal length
	const double focusDistance								///< [in] Focus distance
	)
{
	ICamera* pCamera = 0;
	RISE_API_CreateThinlensCamera( &pCamera, Point3(ptLocation), Point3(ptLookAt), Vector3(vUp), fov, xres, yres, pixelAR, exposure, scanningRate, pixelRate, Vector3(orientation), Vector2(target_orientation), aperture, focalLength, focusDistance );
	pScene->SetCamera( pCamera );
	safe_release( pCamera );
	return true;
}

//! Sets a fisheye camera
/// \return TRUE if successful, FALSE otherwise
bool Job::SetFisheyeCamera(
	const double ptLocation[3],								///< [in] Absolute location of where the camera is located
	const double ptLookAt[3], 								///< [in] Absolute point the camera is looking at
	const double vUp[3],									///< [in] Up vector of the camera
	const unsigned int xres,								///< [in] X resolution of virtual screen
	const unsigned int yres,								///< [in] Y resolution of virtual screen
	const double pixelAR,									///< [in] Pixel aspect ratio
	const double exposure,									///< [in] Exposure time of the camera
	const double scanningRate,								///< [in] Rate at which each scanline is recorded
	const double pixelRate,									///< [in] Rate at which each pixel is recorded
	const double orientation[3],							///< [in] Orientation (Pitch,Roll,Yaw)
	const double target_orientation[2],						///< [in] Orientation relative to a target
	const double scale										///< [in] Scale factor to exagerrate the effects
	)
{
	ICamera* pCamera = 0;
	RISE_API_CreateFisheyeCamera( &pCamera, Point3(ptLocation), Point3(ptLookAt), Vector3(vUp), xres, yres, pixelAR, exposure, scanningRate, pixelRate, Vector3(orientation), Vector2(target_orientation), scale );
	pScene->SetCamera( pCamera );
	safe_release( pCamera );
	return true;
}

//! Sets an orthographic camera
/// \return TRUE if successful, FALSE otherwise
bool Job::SetOrthographicCamera(
	const double ptLocation[3],								///< [in] Absolute location of where the camera is located
	const double ptLookAt[3], 								///< [in] Absolute point the camera is looking at
	const double vUp[3],									///< [in] Up vector of the camera
	const unsigned int xres,								///< [in] X resolution of virtual screen
	const unsigned int yres,								///< [in] Y resolution of virtual screen
	const double vpScale[2],								///< [in] Viewport scale factor
	const double pixelAR,									///< [in] Pixel aspect ratio
	const double exposure,									///< [in] Exposure time of the camera
	const double scanningRate,								///< [in] Rate at which each scanline is recorded
	const double pixelRate,									///< [in] Rate at which each pixel is recorded
	const double orientation[3],							///< [in] Orientation (Pitch,Roll,Yaw)
	const double target_orientation[2]						///< [in] Orientation relative to a target
	)
{
	ICamera* pCamera = 0;
	RISE_API_CreateOrthographicCamera( &pCamera, Point3(ptLocation), Point3(ptLookAt), Vector3(vUp), xres, yres, Vector2(vpScale), pixelAR, exposure, scanningRate, pixelRate, Vector3(orientation), Vector2(target_orientation) );
	pScene->SetCamera( pCamera );
	safe_release( pCamera );
	return true;
}

//
// Adding painters
//


//! Adds a simple checker painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddCheckerPainter(
							const char* name,				///< [in] Name of the painter
							const double size,				///< [in] Size of the checkers in texture mapping units
							const char* pa,					///< [in] First painter
							const char* pb					///< [in] Second painter
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateCheckerPainter( &pPainter, size, *pA, *pB );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}



//! Adds a lines painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddLinesPainter(
							const char* name,				///< [in] Name of the painter
							const double size,				///< [in] Size of the lines in texture mapping units
							const char* pa,					///< [in] First painter
							const char* pb,					///< [in] Second painter
							const bool bvert				///< [in] Are the lines vertical?
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateLinesPainter( &pPainter, size, *pA, *pB, bvert );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Adds a mandelbrot fractal painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddMandelbrotFractalPainter(
							const char* name,				///< [in] Name of the painter
							const char* pa,					///< [in] First painter
							const char* pb,					///< [in] Second painter
							const double lower_x,
							const double upper_x,
							const double lower_y,
							const double upper_y,
							const double exp
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateMandelbrotFractalPainter( &pPainter, *pA, *pB, lower_x, upper_x, lower_y, upper_y, exp );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Adds a 2D perlin noise painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPerlin2DPainter(
							const char* name,				///< [in] Name of the painter
							const double dPersistence,		///< [in] Persistence
							const unsigned int nOctaves,	///< [in] Number of octaves to use in noise generation
							const char* pa,					///< [in] First painter
							const char* pb,					///< [in] Second painter
							const double vScale[2],			///< [in] How much to scale the function by
							const double vShift[2]			///< [in] How much to shift the function by
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}


	IPainter* pPainter = 0;
	RISE_API_CreatePerlin2DPainter( &pPainter, dPersistence, nOctaves, *pA, *pB, Vector2(vScale), Vector2(vShift) );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Adds a 2D perlin noise painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPerlin3DPainter(
								const char* name,				///< [in] Name of the painter
								const double dPersistence,		///< [in] Persistence
								const unsigned int nOctaves,	///< [in] Number of octaves to use in noise generation
								const char* pa,					///< [in] First painter
								const char* pb,					///< [in] Second painter
								const double vScale[3],			///< [in] How much to scale the function by
								const double vShift[3]			///< [in] How much to shift the function by
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreatePerlin3DPainter( &pPainter, dPersistence, nOctaves, *pA, *pB, Vector3(vScale), Vector3(vShift) );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Adds a spectral color painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddSpectralColorPainter(
							const char* name,				///< [in] Name of the painter
							const double* amplitudes,		///< [in] Array that contains the amplitudes
							const double* frequencies,		///< [in] Array that contains the frequencies for the amplitudes
							const double lambda_begin,		///< [in] Begining of the spectral packet
							const double lambda_end,		///< [in] End of the spectral packet
							const unsigned int numfreq,		///< [in] Number of frequencies in the array
							const double scale				///< [in] How much to scale the amplitudes by
							)
{
	IPiecewiseFunction1D* pFunc = 0;
	RISE_API_CreatePiecewiseLinearFunction1D( &pFunc );

	for( unsigned int i=0; i<numfreq; i++ ) {
		pFunc->addControlPoint( std::make_pair( frequencies[i], amplitudes[i] ) );
	}

	SpectralPacket spectrum =	SpectralPacket( lambda_begin, lambda_end, numfreq, pFunc );

	IPainter* pPainter = 0;
	RISE_API_CreateSpectralColorPainter( &pPainter, spectrum, scale );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	safe_release( pFunc );
	return true;
}

static IRasterImageAccessor* RasterImageAccessorFromChar( const char filter_type, IRasterImage& image )
{
	IRasterImageAccessor* pRIA = 0;

	switch( filter_type )
	{
	case 0:
		RISE_API_CreateNNBRasterImageAccessor( &pRIA, image );
		break;
	default:
		GlobalLog()->PrintEasyWarning( "Unknown texture filter type, using bilinear" );
	case 1:
		RISE_API_CreateBiLinRasterImageAccessor( &pRIA, image );
		break;
	case 2:
		RISE_API_CreateCatmullRomBicubicRasterImageAccessor( &pRIA, image );
		break;
	case 3:
		RISE_API_CreateUniformBSplineBicubicRasterImageAccessor( &pRIA, image );
		break;

	};

	return pRIA;
}

//! Adds a texture painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPNGTexturePainter(
							const char* name,				///< [in] Name of the painter
							const char* filename,			///< [in] Name of the file that contains the texture
							const char color_space,			///< [in] Color space in the file
															///		0 - Rec709 RGB linear
															///		1 - sRGB profile
															///		2 - ROMM RGB (ProPhotoRGB) linear
															///		3 - ROMM RGB (ProPhotoRGB) non-linear
							const char filter_type,			///< [in] Type of texture filtering
																///     0 - Nearest neighbour
																///     1 - Bilinear
																///     2 - Catmull Rom Bicubic
																///     3 - Uniform BSpline Bicubic
							const bool lowmemory,			///< [in] low memory mode doesn't do an image convert
							const double scale[3],			///< [in] Scale factor for color values
							const double shift[3]			///< [in] Shift factor for color values
							)
{
	IRasterImage* pImage = 0;
	if( lowmemory ) {
		RISE_API_CreateReadOnlyRISEColorRasterImage( &pImage );
	} else {
		RISE_API_CreateRISEColorRasterImage( &pImage, 0, 0, RISEColor(0,0,0,0) );
	}

	IReadBuffer* pReadBuffer = 0;
	RISE_API_CreateDiskFileReadBuffer( &pReadBuffer, filename );

	COLOR_SPACE gc = eColorSpace_sRGB;
	switch( color_space )
	{
	case 1:
		gc = eColorSpace_sRGB;
		break;
	case 0:
		gc = eColorSpace_Rec709RGB_Linear;
		break;
	case 2:
		gc = eColorSpace_ROMMRGB_Linear;
		break;
	case 3:
		gc = eColorSpace_ProPhotoRGB;
		break;
	};

	IRasterImageReader* pImageReader = 0;
	RISE_API_CreatePNGReader( &pImageReader, *pReadBuffer, gc );

	pImage->LoadImage( pImageReader );

	// Apply the scale and/or shift operators
	{
		if( scale[0] != 1 || scale[1] != 1 || scale[2] != 1 ) {
			IOneColorOperator* pOp = 0;
			RISE_API_CreateScaleColorOperatorRasterImage( &pOp, RISEColor( RISEPel(scale), 1.0 ) );
			Apply1ColorOperator( *pImage, *pOp );
			safe_release( pOp );
		}

		if( shift[0] != 0 || shift[1] != 0 || shift[2] != 0 ) {
			IOneColorOperator* pOp = 0;
			RISE_API_CreateShiftColorOperatorRasterImage( &pOp, RISEColor( RISEPel(shift), 0 ) );
			Apply1ColorOperator( *pImage, *pOp );
			safe_release( pOp );
		}
	}

	IRasterImageAccessor* pRIA = RasterImageAccessorFromChar( filter_type, *pImage );

	IPainter* pPainter = 0;
	RISE_API_CreateTexturePainter( &pPainter, pRIA );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );

	safe_release( pRIA );
	safe_release( pImageReader );
	safe_release( pReadBuffer );
	safe_release( pImage );

	return true;
}

//! Adds a texture painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddHDRTexturePainter(
							const char* name,				///< [in] Name of the painter
							const char* filename,			///< [in] Name of the file that contains the texture
							const char filter_type,			///< [in] Type of texture filtering
																///     0 - Nearest neighbour
																///     1 - Bilinear
																///     2 - Catmull Rom Bicubic
																///     3 - Uniform BSpline Bicubic
							const bool lowmemory,			///< [in] low memory mode doesn't do an image convert
							const double scale[3],			///< [in] Scale factor for color values
							const double shift[3]			///< [in] Shift factor for color values
							)
{
	IRasterImage* pImage = 0;
	if( lowmemory ) {
		RISE_API_CreateReadOnlyRISEColorRasterImage( &pImage );
	} else {
		RISE_API_CreateRISEColorRasterImage( &pImage, 0, 0, RISEColor(0,0,0,0) );
	}

	IMemoryBuffer* pReadBuffer = 0;
	RISE_API_CreateMemoryBufferFromFile( &pReadBuffer, filename );

	IRasterImageReader* pImageReader = 0;
	RISE_API_CreateHDRReader( &pImageReader, *pReadBuffer );

	pImage->LoadImage( pImageReader );

	// Apply the scale and/or shift operators
	{
		if( scale[0] != 1 || scale[1] != 1 || scale[2] != 1 ) {
			IOneColorOperator* pOp = 0;
			RISE_API_CreateScaleColorOperatorRasterImage( &pOp, RISEColor( RISEPel(scale), 1.0 ) );
			Apply1ColorOperator( *pImage, *pOp );
			safe_release( pOp );
		}

		if( shift[0] != 0 || shift[1] != 0 || shift[2] != 0 ) {
			IOneColorOperator* pOp = 0;
			RISE_API_CreateShiftColorOperatorRasterImage( &pOp, RISEColor( RISEPel(shift), 0 ) );
			Apply1ColorOperator( *pImage, *pOp );
			safe_release( pOp );
		}
	}

	IRasterImageAccessor* pRIA = RasterImageAccessorFromChar( filter_type, *pImage );

	IPainter* pPainter = 0;
	RISE_API_CreateTexturePainter( &pPainter, pRIA );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );

	safe_release( pRIA );
	safe_release( pImageReader );
	safe_release( pReadBuffer );
	safe_release( pImage );

	return true;
}

//! Adds an EXR texture painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddEXRTexturePainter(
							const char* name,				///< [in] Name of the painter
							const char* filename,			///< [in] Name of the file that contains the texture
							const char color_space,			///< [in] Color space in the file
															///		0 - Rec709 RGB Linear
															///		1 - sRGB profile
															///		2 - ROMM RGB (ProPhotoRGB) linear
															///		3 - ROMM RGB (ProPhotoRGB) non-linear
							const char filter_type,			///< [in] Type of texture filtering
															///     0 - Nearest neighbour
															///     1 - Bilinear
															///     2 - Catmull Rom Bicubic
															///     3 - Uniform BSpline Bicubic
							const bool lowmemory,			///< [in] low memory mode doesn't do an image convert
							const double scale[3],			///< [in] Scale factor for color values
							const double shift[3]			///< [in] Shift factor for color values
							)
{
	IRasterImage* pImage = 0;
	if( lowmemory ) {
		RISE_API_CreateReadOnlyRISEColorRasterImage( &pImage );
	} else {
		RISE_API_CreateRISEColorRasterImage( &pImage, 0, 0, RISEColor(0,0,0,0) );
	}

	IMemoryBuffer* pReadBuffer = 0;
	RISE_API_CreateMemoryBufferFromFile( &pReadBuffer, filename );

	COLOR_SPACE gc = eColorSpace_sRGB;
	switch( color_space )
	{
	case 1:
		gc = eColorSpace_sRGB;
		break;
	case 0:
		gc = eColorSpace_Rec709RGB_Linear;
		break;
	case 2:
		gc = eColorSpace_ROMMRGB_Linear;
		break;
	case 3:
		gc = eColorSpace_ProPhotoRGB;
		break;
	};

	IRasterImageReader* pImageReader = 0;
	RISE_API_CreateEXRReader( &pImageReader, *pReadBuffer, gc );

	pImage->LoadImage( pImageReader );

	// Apply the scale and/or shift operators
	{
		if( scale[0] != 1 || scale[1] != 1 || scale[2] != 1 ) {
			IOneColorOperator* pOp = 0;
			RISE_API_CreateScaleColorOperatorRasterImage( &pOp, RISEColor( RISEPel(scale), 1.0 ) );
			Apply1ColorOperator( *pImage, *pOp );
			safe_release( pOp );
		}

		if( shift[0] != 0 || shift[1] != 0 || shift[2] != 0 ) {
			IOneColorOperator* pOp = 0;
			RISE_API_CreateShiftColorOperatorRasterImage( &pOp, RISEColor( RISEPel(shift), 0 ) );
			Apply1ColorOperator( *pImage, *pOp );
			safe_release( pOp );
		}
	}

	IRasterImageAccessor* pRIA = RasterImageAccessorFromChar( filter_type, *pImage );

	IPainter* pPainter = 0;
	RISE_API_CreateTexturePainter( &pPainter, pRIA );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );

	safe_release( pRIA );
	safe_release( pImageReader );
	safe_release( pReadBuffer );
	safe_release( pImage );

	return true;
}

//! Adds a texture painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddTIFFTexturePainter(
							const char* name,				///< [in] Name of the painter
							const char* filename,			///< [in] Name of the file that contains the texture
							const char color_space,			///< [in] Color space in the file
															///		0 - Rec709 RGB Linear
															///		1 - sRGB profile
															///		2 - ROMM RGB (ProPhotoRGB) linear
															///		3 - ROMM RGB (ProPhotoRGB) non-linear
							const char filter_type,			///< [in] Type of texture filtering
																///     0 - Nearest neighbour
																///     1 - Bilinear
																///     2 - Catmull Rom Bicubic
																///     3 - Uniform BSpline Bicubic
							const bool lowmemory,			///< [in] low memory mode doesn't do an image convert
							const double scale[3],			///< [in] Scale factor for color values
							const double shift[3]			///< [in] Shift factor for color values
							)
{
	IRasterImage* pImage = 0;
	if( lowmemory ) {
		RISE_API_CreateReadOnlyRISEColorRasterImage( &pImage );
	} else {
		RISE_API_CreateRISEColorRasterImage( &pImage, 0, 0, RISEColor(0,0,0,0) );
	}

	IReadBuffer* pReadBuffer = 0;
	RISE_API_CreateDiskFileReadBuffer( &pReadBuffer, filename );

	COLOR_SPACE gc = eColorSpace_sRGB;
	switch( color_space )
	{
	case 1:
		gc = eColorSpace_sRGB;
		break;
	case 0:
		gc = eColorSpace_Rec709RGB_Linear;
		break;
	case 2:
		gc = eColorSpace_ROMMRGB_Linear;
		break;
	case 3:
		gc = eColorSpace_ProPhotoRGB;
		break;
	};

	IRasterImageReader* pImageReader = 0;
	RISE_API_CreateTIFFReader( &pImageReader, *pReadBuffer, gc );

	pImage->LoadImage( pImageReader );

	// Apply the scale and/or shift operators
	{
		if( scale[0] != 1 || scale[1] != 1 || scale[2] != 1 ) {
			IOneColorOperator* pOp = 0;
			RISE_API_CreateScaleColorOperatorRasterImage( &pOp, RISEColor( RISEPel(scale), 1.0 ) );
			Apply1ColorOperator( *pImage, *pOp );
			safe_release( pOp );
		}

		if( shift[0] != 0 || shift[1] != 0 || shift[2] != 0 ) {
			IOneColorOperator* pOp = 0;
			RISE_API_CreateShiftColorOperatorRasterImage( &pOp, RISEColor( RISEPel(shift), 0 ) );
			Apply1ColorOperator( *pImage, *pOp );
			safe_release( pOp );
		}
	}

	IRasterImageAccessor* pRIA = RasterImageAccessorFromChar( filter_type, *pImage );

	IPainter* pPainter = 0;
	RISE_API_CreateTexturePainter( &pPainter, pRIA );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );

	safe_release( pRIA );
	safe_release( pImageReader );
	safe_release( pReadBuffer );
	safe_release( pImage );

	return true;
}

//! Adds a painter that paints a uniform color
/// \return TRUE if successful, FALSE otherwise
bool Job::AddUniformColorPainter(
							const char* name,				///< [in] Name of the painter
							const double pel[3],			///< [in] Color to paint
							const char* cspace				///< [in] Color space of the given color
							)
{
	IPainter* pPainter = 0;
	if( cspace )
	{
		// Then a type of color is specified
		if( strcmp( cspace, "Rec709RGB_Linear" ) == 0 ) {
			RISE_API_CreateUniformColorPainter( &pPainter, Rec709RGBPel(pel) );
		} else if ( strcmp( cspace, "sRGB" ) == 0  ) {
			RISE_API_CreateUniformColorPainter( &pPainter, sRGBPel(pel) );
		} else if ( strcmp( cspace, "ROMMRGB_Linear" ) == 0  ) {
			RISE_API_CreateUniformColorPainter( &pPainter, ROMMRGBPel(pel) );
		} else if ( strcmp( cspace, "ProPhotoRGB" ) == 0  ) {
			RISE_API_CreateUniformColorPainter( &pPainter, ProPhotoRGBPel(pel) );
		} else if ( strcmp( cspace, "RISERGB" ) == 0  ) {
			RISE_API_CreateUniformColorPainter( &pPainter, RISEPel(pel) );
		} else {
			GlobalLog()->PrintEx( eLog_Error, "Unknown color space: %s", cspace );
			return false;
		}
	} else {	// we assume SRGB values by default
		RISE_API_CreateUniformColorPainter( &pPainter, sRGBPel(pel) );
	}

	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Adds a painter that paints a voronoi diagram
/// \return TRUE if successful, FALSE otherwise
bool Job::AddVoronoi2DPainter(
							const char* name,				///< [in] Name of the painter
							const double pt_x[],			///< [in] X co-ordinates of generators
							const double pt_y[],			///< [in] Y co-ordinates of generators
							const char** painters,			///< [in] The painters for each generator
							const unsigned int count,		///< [in] Number of the generators
							const char* border,				///< [in] Name of the painter for the border
							const double bsize				///< [in] Size of the border
							)
{
	if( count < 2 ) {
		return false;
	}

	IPainter* pBorder = pPntManager->GetItem( border );

	if( !pBorder ) {
		return false;
	}

	std::vector<Point2> pts;
	std::vector<IPainter*> ptrs;

	for( unsigned int i=0; i<count; i++ ) {
		pts.push_back( Point2( pt_x[i], pt_y[i] ) );
		ptrs.push_back( pPntManager->GetItem( painters[i] ) );
	}

	IPainter* pPainter = 0;
	RISE_API_CreateVoronoi2DPainter( &pPainter, pts, ptrs, *pBorder, bsize );

	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );

	return true;
}

//! Adds a painter that paints a voronoi diagram in 3D
/// \return TRUE if successful, FALSE otherwise
bool Job::AddVoronoi3DPainter(
							const char* name,				///< [in] Name of the painter
							const double pt_x[],			///< [in] X co-ordinates of generators
							const double pt_y[],			///< [in] Y co-ordinates of generators
							const double pt_z[],			///< [in] Z co-ordinates of generators
							const char** painters,			///< [in] The painters for each generator
							const unsigned int count,		///< [in] Number of the generators
							const char* border,				///< [in] Name of the painter for the border
							const double bsize				///< [in] Size of the border
							)
{
	if( count < 2 ) {
		return false;
	}

	IPainter* pBorder = pPntManager->GetItem( border );

	if( !pBorder ) {
		return false;
	}

	std::vector<Point3> pts;
	std::vector<IPainter*> ptrs;

	for( unsigned int i=0; i<count; i++ ) {
		pts.push_back( Point3( pt_x[i], pt_y[i], pt_z[i] ) );
		ptrs.push_back( pPntManager->GetItem( painters[i] ) );
	}

	IPainter* pPainter = 0;
	RISE_API_CreateVoronoi3DPainter( &pPainter, pts, ptrs, *pBorder, bsize );

	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );

	return true;
}

//! Adds a iridescent painter (a painter whose color changes as viewing angle changes)
/// \return TRUE if successful, FALSE otherwise
bool Job::AddIridescentPainter(
							const char* name,				///< [in] Name of the painter
							const char* pa,					///< [in] First painter
							const char* pb,					///< [in] Second painter
							const double bias				///< [in] Biases the iridescence to one color or another
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateIridescentPainter( &pPainter, *pA, *pB, bias );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Creates a black body radiator painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddBlackBodyPainter(
							const char* name,				///< [in] Name of the painter
							const double temperature,		///< [in] Temperature of the radiator in Kelvins
							const double lambda_begin,		///< [in] Where in the spectrum to start creating the spectral packet
							const double lambda_end,		///< [in] Where in the spectrum to end creating the spectral packet
							const unsigned int num_freq,	///< [in] Number of frequencies to use in the spectral packet
							const bool normalize,			///< [in] Should the values be normalized to peak intensity?
							const double scale				///< [in] Value to scale radiant exitance by
							)
{
	IPainter* pPainter = 0;
	RISE_API_CreateBlackBodyPainter( &pPainter, temperature, lambda_begin, lambda_end, num_freq, normalize, scale );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Adds a blend painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddBlendPainter(
							const char* name,				///< [in] Name of the painter
							const char* pa,					///< [in] First painter
							const char* pb,					///< [in] Second painter
							const char* mask				///< [in] Mask painter
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );
	IPainter* pMask = pPntManager->GetItem( mask );

	if( !pA || !pB || !pMask ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateBlendPainter( &pPainter, *pA, *pB, *pMask );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//
// Adding materials
//


//! Creates Lambertian material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddLambertianMaterial(
							const char* name,				///< [in] Name of the material
							const char* ref					///< [in] Reflectance Painter
							)
{
	IPainter* pRef = pPntManager->GetItem( ref );
	if( !pRef ) {
		return false;
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateLambertianMaterial( &pMaterial, *pRef );

	pMatManager->AddItem( pMaterial, name );
	safe_release( pMaterial );

	return true;
}

//! Creates a Polished material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPolishedMaterial(
							const char* name,				///< [in] Name of the material
							const char* ref,				///< [in] Reflectance of diffuse substrate
							const char* tau,				///< [in] Transmittance of dielectric top
							const char* Nt,					///< [in] Index of refraction of dielectric coating
							const char* scat,				///< [in] Scattering function for dielectric coating (either Phong or HG)
							const bool hg					///< [in] Use Henyey-Greenstein phase function scattering
							)
{
	IPainter* pRef = pPntManager->GetItem( ref );
	IPainter* pTau = pPntManager->GetItem( tau );
	if( !pRef || !pTau ) {
		return false;
	}

	IPainter*		sc = pPntManager->GetItem( scat );
	IPainter*		refract = pPntManager->GetItem( Nt );

	if( !sc )
	{
		double fScat = atof(scat);
		RISE_API_CreateUniformColorPainter( &sc, RISEPel(fScat,fScat,fScat) );
	} else {
		sc->addref();
	}

	if( !refract )
	{
		double fRefract = atof(Nt);
		RISE_API_CreateUniformColorPainter( &refract, RISEPel(fRefract,fRefract,fRefract) );
	} else {
		refract->addref();
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreatePolishedMaterial( &pMaterial, *pRef, *pTau, *refract, *sc, hg );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( sc );
	safe_release( refract );

	return true;
}

//! Creates a Dielectric material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddDielectricMaterial(
							const char* name,				///< [in] Name of the material
							const char* tau,				///< [in] Transmittance painter
							const char* rIndex,				///< [in] Index of refraction
							const char* scat,				///< [in] Scattering function (either Phong or HG)
							const bool hg					///< [in] Use Henyey-Greenstein phase function scattering
							)
{
	IPainter* pTau = pPntManager->GetItem( tau );
	if( !pTau ) {
		return false;
	}

	IPainter*		rindex = pPntManager->GetItem( rIndex );
	IPainter*		sc = pPntManager->GetItem( scat );

	if( !sc )
	{
		double fScat = atof(scat);
		RISE_API_CreateUniformColorPainter( &sc, RISEPel(fScat,fScat,fScat) );
	} else {
		sc->addref();
	}

	if( !rindex )
	{
		double frindex = atof(rIndex);
		RISE_API_CreateUniformColorPainter( &rindex, RISEPel(frindex,frindex,frindex) );
	} else {
		rindex->addref();
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateDielectricMaterial( &pMaterial, *pTau, *rindex, *sc, hg );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( sc );
	safe_release( rindex );

	return true;
}

//! Creates an isotropic phong material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddIsotropicPhongMaterial(
							const char* name,				///< [in] Name of the material
							const char* rd,					///< [in] Diffuse reflectance painter
							const char* rs,					///< [in] Specular reflectance painter
							const char* exponent			///< [in] Phong exponent
							)
{
	IPainter* pRd = pPntManager->GetItem(rd);
	IPainter* pRs = pPntManager->GetItem(rs);

	if( !pRd || !pRs ) {
		return false;
	}

	IPainter*		pExp = pPntManager->GetItem( exponent );

	if( !pExp )
	{
		double fexp = atof(exponent);
		RISE_API_CreateUniformColorPainter( &pExp, RISEPel(fexp,fexp,fexp) );
	} else {
		pExp->addref();
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateIsotropicPhongMaterial( &pMaterial, *pRd, *pRs, *pExp );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( pExp );

	return true;
}

//! Creates the anisotropic phong material of Ashikmin and Shirley
/// \return TRUE if successful, FALSE otherwise
bool Job::AddAshikminShirleyAnisotropicPhongMaterial(
							const char* name,				///< [in] Name of the material
							const char* rd,					///< [in] Diffuse reflectance painter
							const char* rs,					///< [in] Specular reflectance painter
							const char* Nu,					///< [in] Phong exponent in U
							const char* Nv					///< [in] Phong exponent in V
							)
{
	IPainter* pRd = pPntManager->GetItem(rd);
	IPainter* pRs = pPntManager->GetItem(rs);

	if( !pRd || !pRs ) {
		return false;
	}

	IPainter*		pNu = pPntManager->GetItem( Nu );
	IPainter*		pNv = pPntManager->GetItem( Nv );

	if( !pNu )
	{
		double fnu = atof(Nu);
		RISE_API_CreateUniformColorPainter( &pNu, RISEPel(fnu,fnu,fnu) );
	} else {
		pNu->addref();
	}

	if( !pNv )
	{
		double fnv = atof(Nv);
		RISE_API_CreateUniformColorPainter( &pNv, RISEPel(fnv,fnv,fnv) );
	} else {
		pNv->addref();
	}


	IMaterial* pMaterial = 0;
	RISE_API_CreateAshikminShirleyAnisotropicPhongMaterial( &pMaterial, *pRd, *pRs, *pNu, *pNv );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( pNu );
	safe_release( pNv );

	return true;
}

//! Creates a perfect reflector
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPerfectReflectorMaterial(
							const char* name,				///< [in] Name of the material
							const char* ref					///< [in] Reflectance painter
							)
{
	IPainter* pRd = pPntManager->GetItem(ref);

	if( !pRd ) {
		return false;
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreatePerfectReflectorMaterial( &pMaterial, *pRd );

	pMatManager->AddItem( pMaterial, name );
	safe_release( pMaterial );

	return true;
}

//! Creates a perfect refractor
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPerfectRefractorMaterial(
							const char* name,				///< [in] Name of the material
							const char* ref,				///< [in] Amount of refraction painter
							const char* ior					///< [in] Index of refraction
							)
{
	IPainter* pRd = pPntManager->GetItem(ref);

	if( !pRd ) {
		return false;
	}

	IPainter*		pIOR = pPntManager->GetItem( ior );

	if( !pIOR )
	{
		double fior = atof(ior);
		RISE_API_CreateUniformColorPainter( &pIOR, RISEPel(fior,fior,fior) );
	} else {
		pIOR->addref();
	}


	IMaterial* pMaterial = 0;
	RISE_API_CreatePerfectRefractorMaterial( &pMaterial, *pRd, *pIOR );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pIOR );
	safe_release( pMaterial );

	return true;
}

//! Creates a translucent material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddTranslucentMaterial(
							const char* name,				///< [in] Name of the material
							const char* rF,					///< [in] Reflectance painter
							const char* T,					///< [in] Transmittance painter
							const char* ext,				///< [in] Extinction painter
							const char* N,					///< [in] Phong scattering function
							const char* scat				///< [in] Multiple scattering component
							)
{
	IPainter* pRf = pPntManager->GetItem(rF);
	IPainter* pTau = pPntManager->GetItem(T);
	IPainter* pExt = pPntManager->GetItem(ext);

	if( !pRf || !pTau || !pExt ) {
		return false;
	}

	IPainter*		pN = pPntManager->GetItem( N );

	if( !pN )
	{
		double fn = atof(N);
		RISE_API_CreateUniformColorPainter( &pN, RISEPel(fn,fn,fn) );
	} else {
		pN->addref();
	}

	IPainter*		pScat = pPntManager->GetItem( scat );

	if( !pScat )
	{
		double fscat = atof(scat);
		RISE_API_CreateUniformColorPainter( &pScat, RISEPel(fscat,fscat,fscat) );
	} else {
		pScat->addref();
	}


	IMaterial* pMaterial = 0;
	RISE_API_CreateTranslucentMaterial( &pMaterial, *pRf, *pTau, *pExt, *pN, *pScat );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pN );
	safe_release( pScat );
	safe_release( pMaterial );

	return true;
}

bool Job::AddBioSpecSkinMaterial(
	const char* name,
	const char* thickness_SC_,									///< Thickness of the stratum corneum (in cm)
	const char* thickness_epidermis_,							///< Thickness of the epidermis (in cm)
	const char* thickness_papillary_dermis_,					///< Thickness of the papillary dermis (in cm)
	const char* thickness_reticular_dermis_,					///< Thickness of the reticular dermis (in cm)
	const char* ior_SC_,										///< Index of refraction of the stratum corneum
	const char* ior_epidermis_,									///< Index of refraction of the epidermis
	const char* ior_papillary_dermis_,							///< Index of refraction of the papillary dermis
	const char* ior_reticular_dermis_,							///< Index of refraction of the reticular dermis
	const char* concentration_eumelanin_,						///< Average Concentration of eumelanin in the melanosomes
	const char* concentration_pheomelanin_,						///< Average Concentration of pheomelanin in the melanosomes
	const char* melanosomes_in_epidermis_,						///< Percentage of the epidermis made up of melanosomes
	const char* hb_ratio_,										///< Ratio of oxyhemoglobin to deoxyhemoglobin in blood
	const char* whole_blood_in_papillary_dermis_,				///< Percentage of the papillary dermis made up of whole blood
	const char* whole_blood_in_reticular_dermis_,				///< Percentage of the reticular dermis made up of whole blood
	const char* bilirubin_concentration_,						///< Concentration of Bilirubin in whole blood
	const char* betacarotene_concentration_SC_,					///< Concentration of Beta-Carotene in the stratum corneum
	const char* betacarotene_concentration_epidermis_,			///< Concentration of Beta-Carotene in the epidermis
	const char* betacarotene_concentration_dermis_,				///< Concentration of Beta-Carotene in the dermis
	const char* folds_aspect_ratio_,							///< Aspect ratio of the little folds and wrinkles on the skin surface
	const bool bSubdermalLayer									///< Should the model simulate a perfectly reflecting subdermal layer?
	)
{

	IPainter* pnt_thickness_SC_ = pPntManager->GetItem( thickness_SC_ );
	IPainter* pnt_thickness_epidermis_ = pPntManager->GetItem( thickness_epidermis_ );
	IPainter* pnt_thickness_papillary_dermis_ = pPntManager->GetItem( thickness_papillary_dermis_ );
	IPainter* pnt_thickness_reticular_dermis_ = pPntManager->GetItem( thickness_reticular_dermis_ );
	IPainter* pnt_ior_SC_ = pPntManager->GetItem( ior_SC_ );
	IPainter* pnt_ior_epidermis_ = pPntManager->GetItem( ior_epidermis_ );
	IPainter* pnt_ior_papillary_dermis_ = pPntManager->GetItem( ior_papillary_dermis_ );
	IPainter* pnt_ior_reticular_dermis_ = pPntManager->GetItem( ior_reticular_dermis_ );
	IPainter* pnt_concentration_eumelanin_ = pPntManager->GetItem( concentration_eumelanin_ );
	IPainter* pnt_concentration_pheomelanin_ = pPntManager->GetItem( concentration_pheomelanin_ );
	IPainter* pnt_melanosomes_in_epidermis_ = pPntManager->GetItem( melanosomes_in_epidermis_ );
	IPainter* pnt_hb_ratio_ = pPntManager->GetItem( hb_ratio_ );
	IPainter* pnt_whole_blood_in_papillary_dermis_ = pPntManager->GetItem( whole_blood_in_papillary_dermis_ );
	IPainter* pnt_whole_blood_in_reticular_dermis_ = pPntManager->GetItem( whole_blood_in_reticular_dermis_ );
	IPainter* pnt_bilirubin_concentration_ = pPntManager->GetItem( bilirubin_concentration_ );
	IPainter* pnt_betacarotene_concentration_SC_ = pPntManager->GetItem( betacarotene_concentration_SC_ );
	IPainter* pnt_betacarotene_concentration_epidermis_ = pPntManager->GetItem( betacarotene_concentration_epidermis_ );
	IPainter* pnt_betacarotene_concentration_dermis_ = pPntManager->GetItem( betacarotene_concentration_dermis_ );
	IPainter* pnt_folds_aspect_ratio_ = pPntManager->GetItem( folds_aspect_ratio_ );

	{
#define CHECK_FOR_VALUE( x )\
		{\
			if( !pnt_##x ) {\
				double d = atof( x );\
				RISE_API_CreateUniformColorPainter( &pnt_##x, RISEPel(d,d,d) );\
			} else {\
				pnt_##x->addref();\
			}\
		}

	CHECK_FOR_VALUE(thickness_SC_);
	CHECK_FOR_VALUE(thickness_epidermis_);
	CHECK_FOR_VALUE(thickness_papillary_dermis_);
	CHECK_FOR_VALUE(thickness_reticular_dermis_);
	CHECK_FOR_VALUE(ior_SC_);
	CHECK_FOR_VALUE(ior_epidermis_);
	CHECK_FOR_VALUE(ior_papillary_dermis_);
	CHECK_FOR_VALUE(ior_reticular_dermis_);
	CHECK_FOR_VALUE(concentration_eumelanin_);
	CHECK_FOR_VALUE(concentration_pheomelanin_);
	CHECK_FOR_VALUE(melanosomes_in_epidermis_);
	CHECK_FOR_VALUE(hb_ratio_);
	CHECK_FOR_VALUE(whole_blood_in_papillary_dermis_);
	CHECK_FOR_VALUE(whole_blood_in_reticular_dermis_);
	CHECK_FOR_VALUE(bilirubin_concentration_);
	CHECK_FOR_VALUE(betacarotene_concentration_SC_);
	CHECK_FOR_VALUE(betacarotene_concentration_epidermis_);
	CHECK_FOR_VALUE(betacarotene_concentration_dermis_);
	CHECK_FOR_VALUE(folds_aspect_ratio_);

#undef CHECK_FOR_VALUE
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateBioSpecSkinMaterial( &pMaterial,
		*pnt_thickness_SC_,
		*pnt_thickness_epidermis_,
		*pnt_thickness_papillary_dermis_,
		*pnt_thickness_reticular_dermis_,
		*pnt_ior_SC_,
		*pnt_ior_epidermis_,
		*pnt_ior_papillary_dermis_,
		*pnt_ior_reticular_dermis_,
		*pnt_concentration_eumelanin_,
		*pnt_concentration_pheomelanin_,
		*pnt_melanosomes_in_epidermis_,
		*pnt_hb_ratio_,
		*pnt_whole_blood_in_papillary_dermis_,
		*pnt_whole_blood_in_reticular_dermis_,
		*pnt_bilirubin_concentration_,
		*pnt_betacarotene_concentration_SC_,
		*pnt_betacarotene_concentration_epidermis_,
		*pnt_betacarotene_concentration_dermis_,
		*pnt_folds_aspect_ratio_,
		bSubdermalLayer
		);

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );

	return true;
}

//! Adds a generic human tissue material based on BioSpec
/// \return TRUE if successful, FALSE otherwise
bool Job::AddGenericHumanTissueMaterial(
	const char* name,
	const char* sca,											///< [in] Scattering co-efficient
	const char* g,												///< [in] The g factor in the HG phase function
	const double whole_blood_,									///< Percentage of the tissue made up of whole blood
	const double hb_ratio_,										///< Ratio of oxyhemoglobin to deoxyhemoglobin in blood
	const double bilirubin_concentration_,						///< Concentration of Bilirubin in whole blood
	const double betacarotene_concentration_,					///< Concentration of Beta-Carotene in whole blood
	const bool diffuse											///< Is the tissue just completely diffuse?
	)
{
	GlobalLog()->PrintEasyWarning( "Job::AddGenericHumanTissueMaterial:: This is an experiment and has not bee tested or verfied, its use is not recommended" );

	IPainter* pG = pPntManager->GetItem(g);

	if( !pG ) {
		return false;
	}

	IPainter* pSca = pPntManager->GetItem(sca);

	if( !pSca ) {
		double d = atof( sca );
		RISE_API_CreateUniformColorPainter( &pSca, RISEPel(d,d,d) );
	} else {
		pSca->addref();
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateGenericHumanTissueMaterial( &pMaterial,
		*pSca,
		*pG,
		whole_blood_,
		hb_ratio_,
		bilirubin_concentration_,
		betacarotene_concentration_,
		diffuse );

	pMatManager->AddItem( pMaterial, name );
	safe_release( pMaterial );

	return true;
}

//! Adds Composite material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddCompositeMaterial(
							const char* name,											///< [in] Name of the material
							const char* top,											///< [in] Name of material on top
							const char* bottom,											///< [in] Name of material on bottom
							const unsigned int max_recur,								///< [in] Maximum recursion level in the random walk process
							const unsigned int max_reflection_recursion,				///< [in] Maximum level of reflection recursion
							const unsigned int max_refraction_recursion,				///< [in] Maximum level of refraction recursion
							const unsigned int max_diffuse_recursion,					///< [in] Maximum level of diffuse recursion
							const unsigned int max_translucent_recursion,				///< [in] Maximum level of translucent recursion
							const double thickness										///< [in] Thickness between the materials
							)
{
	GlobalLog()->PrintEasyWarning( "Job::AddCompositeMaterial:: This is an experiment and has not bee tested or verfied, its use is not recommended" );

	IMaterial* pTop = pMatManager->GetItem( top );
	IMaterial* pBottom = pMatManager->GetItem( bottom );

	if( !pTop || !pBottom ) {
		return false;
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateCompositeMaterial( &pMaterial, *pTop, *pBottom, max_recur, max_reflection_recursion, max_refraction_recursion, max_diffuse_recursion, max_translucent_recursion, thickness );

	pMatManager->AddItem( pMaterial, name );
	safe_release( pMaterial );

	return true;
}

//! Adds Composite material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddWardIsotropicGaussianMaterial(
	const char* name,											///< [in] Name of the material
	const char* diffuse,										///< [in] Diffuse reflectance
	const char* specular,										///< [in] Specular reflectance
	const char* alpha											///< [in] Standard deviation (RMS) of surface slope
	)

{
	IPainter* pRd = pPntManager->GetItem(diffuse);
	IPainter* pRs = pPntManager->GetItem(specular);

	if( !pRd || !pRs ) {
		return false;
	}

	IPainter*		pAlpha = pPntManager->GetItem( alpha );

	if( !pAlpha )
	{
		double fa = atof(alpha);
		RISE_API_CreateUniformColorPainter( &pAlpha, RISEPel(fa,fa,fa) );
	} else {
		pAlpha->addref();
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateWardIsotropicGaussianMaterial( &pMaterial, *pRd, *pRs, *pAlpha );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( pAlpha );

	return true;
}

//! Adds Ward's anisotropic elliptical gaussian material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddWardAnisotropicEllipticalGaussianMaterial(
	const char* name,											///< [in] Name of the material
	const char* diffuse,										///< [in] Diffuse reflectance
	const char* specular,										///< [in] Specular reflectance
	const char* alphax,											///< [in] Standard deviation (RMS) of surface slope in x
	const char* alphay											///< [in] Standard deviation (RMS) of surface slope in y
	)
{
	IPainter* pRd = pPntManager->GetItem(diffuse);
	IPainter* pRs = pPntManager->GetItem(specular);

	if( !pRd || !pRs ) {
		return false;
	}

	IPainter*		pAlphaX = pPntManager->GetItem( alphax );
	IPainter*		pAlphaY = pPntManager->GetItem( alphay );

	if( !pAlphaX )
	{
		double fa = atof(alphax);
		RISE_API_CreateUniformColorPainter( &pAlphaX, RISEPel(fa,fa,fa) );
	} else {
		pAlphaX->addref();
	}

	if( !pAlphaY )
	{
		double fa = atof(alphay);
		RISE_API_CreateUniformColorPainter( &pAlphaY, RISEPel(fa,fa,fa) );
	} else {
		pAlphaY->addref();
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateWardAnisotropicEllipticalGaussianMaterial( &pMaterial, *pRd, *pRs, *pAlphaX, *pAlphaY );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( pAlphaX );
	safe_release( pAlphaY );

	return true;
}

//! Adds Cook Torrance material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddCookTorranceMaterial(
	const char* name,											///< [in] Name of the material
	const char* diffuse,										///< [in] Diffuse reflectance
	const char* specular,										///< [in] Specular reflectance
	const char* facet,											///< [in] Facet distribution
	const char* ior,											///< [in] IOR delta
	const char* ext												///< [in] Extinction factor
	)
{
	IPainter* pRd = pPntManager->GetItem(diffuse);
	IPainter* pRs = pPntManager->GetItem(specular);

	if( !pRd || !pRs ) {
		return false;
	}

	IPainter*		pFacet = pPntManager->GetItem( facet );
	IPainter*		pIOR = pPntManager->GetItem( ior );
	IPainter*		pExt = pPntManager->GetItem( ext );

	if( !pFacet )
	{
		double fa = atof(facet);
		RISE_API_CreateUniformColorPainter( &pFacet, RISEPel(fa,fa,fa) );
	} else {
		pFacet->addref();
	}

	if( !pIOR )
	{
		double fa = atof(ior);
		RISE_API_CreateUniformColorPainter( &pIOR, RISEPel(fa,fa,fa) );
	} else {
		pIOR->addref();
	}

	if( !pExt )
	{
		double fa = atof(ext);
		RISE_API_CreateUniformColorPainter( &pExt, RISEPel(fa,fa,fa) );
	} else {
		pExt->addref();
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateCookTorranceMaterial( &pMaterial, *pRd, *pRs, *pFacet, *pIOR, *pExt );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( pFacet );
	safe_release( pIOR );
	safe_release( pExt );

	return true;
}

//! Adds Oren-Nayar material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddOrenNayarMaterial(
	const char* name,											///< [in] Name of the material
	const char* reflectance,									///< [in] Reflectance
	const char* roughness										///< [in] Roughness factor
	)
{
	IPainter* pRef = pPntManager->GetItem(reflectance);

	if( !pRef ) {
		return false;
	}

	IPainter*		pRoughness = pPntManager->GetItem( roughness );

	if( !pRoughness )
	{
		double fa = atof(roughness);
		RISE_API_CreateUniformColorPainter( &pRoughness, RISEPel(fa,fa,fa) );
	} else {
		pRoughness->addref();
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateOrenNayarMaterial( &pMaterial, *pRef, *pRoughness );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( pRoughness );

	return true;
}

//! Adds Schlick material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddSchlickMaterial(
	const char* name,											///< [in] Name of the material
	const char* diffuse,										///< [in] Diffuse reflectance
	const char* specular,										///< [in] Specular reflectance
	const char* roughness,										///< [in] Roughness factor
	const char* isotropy										///< [in] Isotropy factor
	)
{
	IPainter* pRd = pPntManager->GetItem(diffuse);
	IPainter* pRs = pPntManager->GetItem(specular);

	if( !pRd || !pRs ) {
		return false;
	}

	IPainter*		pRoughness = pPntManager->GetItem( roughness );

	if( !pRoughness )
	{
		double fa = atof(roughness);
		RISE_API_CreateUniformColorPainter( &pRoughness, RISEPel(fa,fa,fa) );
	} else {
		pRoughness->addref();
	}

	IPainter*		pIsotropy = pPntManager->GetItem( isotropy );

	if( !pIsotropy )
	{
		double fa = atof(isotropy);
		RISE_API_CreateUniformColorPainter( &pIsotropy, RISEPel(fa,fa,fa) );
	} else {
		pIsotropy->addref();
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateSchlickMaterial( &pMaterial, *pRd, *pRs, *pRoughness, *pIsotropy );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( pRoughness );
	safe_release( pIsotropy );

	return true;
}

//! Adds a data driven material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddDataDrivenMaterial(
	const char* name,											///< [in] Name of the material
	const char* filename										///< [in] Filename to load data from
	)
{
	IMaterial* pMaterial = 0;
	RISE_API_CreateDataDrivenMaterial( &pMaterial, filename );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
    return true;
}

//! Creates a lambertian luminaire material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddLambertianLuminaireMaterial(
							const char* name,				///< [in] Name of the material
							const char* radEx,				///< [in] Radiant exitance painter
							const char* mat,				///< [in] Material to use for all non emmission properties
							const double scale				///< [in] Value to scale radiant exitance by
							)
{
	IPainter* pRadEx = pPntManager->GetItem(radEx);
	IMaterial* pMaterial = pMatManager->GetItem(mat);

	if( !pRadEx || !pMaterial ) {
		return false;
	}

	IMaterial* pLumMaterial = 0;
	RISE_API_CreateLambertianLuminaireMaterial( &pLumMaterial, *pRadEx, *pMaterial, scale );

	pMatManager->AddItem( pLumMaterial, name );
	safe_release( pLumMaterial );

	return true;
}

//! Creates a phong luminaire material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPhongLuminaireMaterial(
							const char* name,				///< [in] Name of the material
							const char* radEx,				///< [in] Radiance exitance painter
							const char* mat,				///< [in] Material to use for all non emmission properties
							const char* N,					///< [in] Phong exponent function
							const double scale				///< [in] Value to scale radiant exitance by
							)
{
	IPainter* pRadEx = pPntManager->GetItem(radEx);
	IMaterial* pMaterial = pMatManager->GetItem(mat);

	if( !pRadEx || !pMaterial ) {
		return false;
	}

	IPainter*	pN = pPntManager->GetItem( N );

	if( !pN )
	{
		double fn = atof(N);
		RISE_API_CreateUniformColorPainter( &pN, RISEPel(fn,fn,fn) );
	} else {
		pN->addref();
	}

	IMaterial* pLumMaterial = 0;
	RISE_API_CreatePhongLuminaireMaterial( &pLumMaterial, *pRadEx, *pMaterial, *pN, scale );

	pMatManager->AddItem( pLumMaterial, name );

	safe_release( pN );
	safe_release( pLumMaterial );

	return true;
}


//
// Adds geometry
//

//! Creates a box located at the origin
/// \return TRUE if successful, FALSE otherwise
bool Job::AddBoxGeometry(
						const char* name,					///< [in] Name of the geometry
						const double width,					///< [in] Width of the box
						const double height,				///< [in] Height of the box
						const double depth					///< [in] Depth of the box
						)
{
	IGeometry* pGeometry = 0;
	RISE_API_CreateBoxGeometry( &pGeometry, width, height, depth );

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//! Creates a circular disk at the origin
/// \return TRUE if successful, FALSE otherwise
bool Job::AddCircularDiskGeometry(
								const char* name,			///< [in] Name of the geometry
								const double radius,		///< [in] Radius of the disk
								const char axis				///< [in] (x|y|z) Which axis the disk sits on
								)
{
	IGeometry* pGeometry = 0;
	RISE_API_CreateCircularDiskGeometry( &pGeometry, radius, axis );

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//! Creates a clipped plane, defined by four points
/// \return TRUE if successful, FALSE otherwise
bool Job::AddClippedPlaneGeometry(
								 const char* name,			///< [in] Name of the geometry
								 const double ptA[4],		///< [in] Point A of the clipped plane
								 const double ptB[4],		///< [in] Point B of the clipped plane
								 const double ptC[4],		///< [in] Point C of the clipped plane
								 const double ptD[4],		///< [in] Point D of the clipped plane
								 const bool doublesided		///< [in] Is it doublesided?
								 )
{
	IGeometry* pGeometry = 0;
	Point3 pts[4];
	pts[0] = Point3( ptA );
	pts[1] = Point3( ptB );
	pts[2] = Point3( ptC );
	pts[3] = Point3( ptD );
	RISE_API_CreateClippedPlaneGeometry( &pGeometry, pts, doublesided );

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//! Creates a Cylinder at the origin
/// \return TRUE if successful, FALSE otherwise
bool Job::AddCylinderGeometry(
							 const char* name,				///< [in] Name of the geometry
							 const char axis,				///< [in] (x|y|z) Which axis the cylinder is sitting on
							 const double radius,			///< [in] Radius of the cylinder
							 const double height			///< [in] Height of the cylinder
							 )
{
	IGeometry* pGeometry = 0;
	RISE_API_CreateCylinderGeometry( &pGeometry, axis, radius, height );

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//! Creates an infinite plane that passes through the origin
/// \return TRUE if successful, FALSE otherwise
/// \todo This needs to be seriously re-evaluated
bool Job::AddInfinitePlaneGeometry(
										  const char* name,	///< [in] Name of the geometry
										  const double xt,	///< [in] How often to tile in X
										  const double yt	///< [in] How often to tile in Y
										  )
{
	IGeometry* pGeometry = 0;
	RISE_API_CreateInfinitePlaneGeometry( &pGeometry, xt, yt );

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//! Creates a sphere at the origin
/// \return TRUE if successful, FALSE otherwise
bool Job::AddSphereGeometry(
								   const char* name,		///< [in] Name of the geometry
								   const double radius		///< [in] Radius of the sphere
								   )
{
	IGeometry* pGeometry = 0;
	RISE_API_CreateSphereGeometry( &pGeometry, radius );

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//! Creates an ellipsoid at the origin
/// \return TRUE if successful, FALSE otherwise
bool Job::AddEllipsoidGeometry(
									const char* name,		///< [in] Name of the geometry
									const double radii[3]	///< [in] Radii of the ellipse
									)
{
	IGeometry* pGeometry = 0;
	RISE_API_CreateEllipsoidGeometry( &pGeometry, Vector3(radii) );

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//! Creates a torus at the origin
/// \return TRUE if successful, FALSE otherwise
bool Job::AddTorusGeometry(
								  const char* name,			///< [in] Name of the geometry
								  const double majorRad,	///< [in] Major radius
								  const double minorRad		///< [in] Minor radius (as a percentage of the major radius)
								  )
{
	IGeometry* pGeometry = 0;
	RISE_API_CreateTorusGeometry( &pGeometry, majorRad, minorRad );

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//! Adds a triangle mesh geometry from the pointers passed it
/// \return TRUE if successful, FALSE otherwise
bool Job::AddIndexedTriangleMeshGeometry(
					const char* name,						///< [in] Name of the geometry
					const float* vertices,					///< [in] List of vertices
					const float* normals,					///< [in] List of normals
					const float* coords,					///< [in] Texture co-ordinates
					const unsigned int* vertexface,			///< [in] List of the vertex faces
					const unsigned int* uvwface,			///< [in] List of the texture coord faces
					const unsigned int* normalface,			///< [in] List of normal faces
					const unsigned int numpts,				///< [in] Number of points, normals and texture coords
					const unsigned int numnormals,			///< [in] Number of normals
					const unsigned int numcoords,			///< [in] Number of texture co-ordinate points
					const unsigned int numfaces,			///< [in] Number of faces
					const unsigned int max_polys,			///< [in] Maximum number of polygons in one octant node
					const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
					const bool double_sided,				///< [in] Are the triangles double sided ?
					const bool use_bsp,						///< [in] Use a BSP tree rather than an Octree
					const bool face_normals					///< [in] Use face normals rather than vertex normals
					)
{
	if( !name || !vertices || !vertexface ) {
		return false;
	}

	ITriangleMeshGeometryIndexed* pGeometry = 0;
	RISE_API_CreateTriangleMeshGeometryIndexed( &pGeometry, max_polys, max_recur, double_sided, use_bsp, face_normals );

	pGeometry->BeginIndexedTriangles();

	unsigned int i=0;
	for( i=0; i<numpts; i++ ) {
		pGeometry->AddVertex( Vertex( vertices[i*3], vertices[i*3+1], vertices[i*3+2] ) );
	}

	if( normals && !face_normals ) {
		for( i=0; i<numnormals; i++ ) {
			pGeometry->AddNormal( Normal( normals[i*3], normals[i*3+1], normals[i*3+2] ) );
		}
	}

	if( coords ) {
		for( i=0; i<numcoords; i++ ) {
			pGeometry->AddTexCoord( TexCoord( coords[i*3], coords[i*3+1] ) );
		}
	} else {
		pGeometry->AddTexCoord( TexCoord( 0, 0 ) );
	}

	for( i=0; i<numfaces; i++ ) {
		IndexedTriangle tri;
		for( int j=0; j<3; j++ ) {
			tri.iVertices[j] = vertexface[i*3+j];
			if( coords && uvwface ) {
				tri.iCoords[j] = uvwface[i*3+j];
			} else {
				tri.iCoords[j] = 0;
			}

			if( normals && !face_normals && normalface ) {
				tri.iNormals[j] = normalface[i*3+j];
			} else {
				tri.iNormals[j] = tri.iVertices[j];
			}
		}

		pGeometry->AddIndexedTriangle( tri );
	}

	if( !normals && !face_normals ) {
		pGeometry->ComputeVertexNormals();
	}

	pGeometry->DoneIndexedTriangles();

	bool bRet = pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );

	return bRet;
}

//! Creates a triangle mesh geometry from a 3DS file
/// \return TRUE if successful, FALSE otherwise
bool Job::Add3DSTriangleMeshGeometry(
					const char* name,						///< [in] Name of the geometry
					const char* filename,					///< [in] The 3DS file to load
					const unsigned int max_polys,			///< [in] Maximum number of polygons in one octant node
					const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
					const bool double_sided,				///< [in] Are the triangles double sided ?
					const bool use_bsp,						///< [in] Use a BSP tree rather than an Octree
					const bool face_normals					///< [in] Use face normals rather than vertex normals
					)
{
	ITriangleMeshGeometryIndexed* pGeometry = 0;
	RISE_API_CreateTriangleMeshGeometryIndexed( &pGeometry, max_polys, max_recur, double_sided, use_bsp, face_normals );

	IReadBuffer* pBuffer = 0;
	RISE_API_CreateDiskFileReadBuffer( &pBuffer, filename );

	ITriangleMeshLoaderIndexed* pLoader = 0;
	RISE_API_Create3DSTriangleMeshLoader( &pLoader, pBuffer );

	bool bRet = pLoader->LoadTriangleMesh( pGeometry );
	if( bRet ) {
		pGeomManager->AddItem( pGeometry, name );
	}

	safe_release( pGeometry );

	safe_release( pLoader );
	safe_release( pBuffer );
	return bRet;
}

//! Creates a triangle mesh geometry from a raw file
/// \return TRUE if successful, FALSE otherwise
bool Job::AddRAWTriangleMeshGeometry(
					const char* name,						///< [in] Name of the geometry
					const char* szFileName,					///< [in] Name of the file to load from
					const unsigned int max_polys,			///< [in] Maximum number of polygons in one octant node
					const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
					const bool double_sided,				///< [in] Are the triangles double sided ?
					const bool use_bsp						///< [in] Use a BSP tree rather than an Octree
					)
{
	ITriangleMeshGeometry* pGeometry = 0;
	RISE_API_CreateTriangleMeshGeometry( &pGeometry, max_polys, max_recur, double_sided, use_bsp );

	ITriangleMeshLoader* pLoader = 0;
	RISE_API_CreateRAWTriangleMeshLoader( &pLoader, szFileName );

	bool bRet = pLoader->LoadTriangleMesh( pGeometry );
	if( bRet ) {
		pGeomManager->AddItem( pGeometry, name );
	}

	safe_release( pGeometry );

	safe_release( pLoader );
	return bRet;
}

//! Creates a triangle mesh geometry from a file of version 2
//! The format of the file for this version is different from the one
//! above
/// \return TRUE if successful, FALSE otherwise
bool Job::AddRAW2TriangleMeshGeometry(
					const char* name,						///< [in] Name of the geometry
					const char* szFileName,					///< [in] Name of the file to load from
					const unsigned int max_polys,			///< [in] Maximum number of polygons in one octant node
					const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
					const bool double_sided,				///< [in] Are the triangles double sided ?
					const bool use_bsp,						///< [in] Use a BSP tree rather than an Octree
					const bool face_normals					///< [in] Use face normals rather than vertex normals
					)
{
	ITriangleMeshGeometryIndexed* pGeometry = 0;
	RISE_API_CreateTriangleMeshGeometryIndexed( &pGeometry, max_polys, max_recur, double_sided, use_bsp, face_normals );

	ITriangleMeshLoaderIndexed* pLoader = 0;
	RISE_API_CreateRAW2TriangleMeshLoader( &pLoader, szFileName );

	bool bRet = pLoader->LoadTriangleMesh( pGeometry );
	if( bRet ) {
		pGeomManager->AddItem( pGeometry, name );
	}

	safe_release( pGeometry );

	safe_release( pLoader );
	return bRet;
}


//! Creates a triangle mesh geometry from a series of bezier patches
/// \return TRUE if successful, FALSE otherwise
/// \todo this is deprecated and should be removed
bool Job::AddBezierTriangleMeshGeometry(
					const char* name,						///< [in] Name of the geometry
					const char* szFileName,					///< [in] Name of the file to load from
					const unsigned int detail,				///< [in] Level of tesselation
					const bool bCombineSharedVertices,		///< [in] Should we try to combine shared vertices?
					const bool bCenterObject,				///< [in] Should the object be re-centered around the origin
					const unsigned int max_polys,			///< [in] Maximum number of polygons in one octant node
					const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
					const bool double_sided,				///< [in] Are the triangles double sided ?
					const bool use_bsp,						///< [in] Use a BSP tree rather than an Octree
					const bool face_normals,				///< [in] Use face normals rather than vertex normals
					const char* displacement,				///< [in] Displacement function for static displacement mapping
					const double disp_scale					///< [in] Displacement scale factor
					)
{
	ITriangleMeshGeometryIndexed* pGeometry = 0;
	RISE_API_CreateTriangleMeshGeometryIndexed( &pGeometry, max_polys, max_recur, double_sided, use_bsp, face_normals );

	IFunction2D* pFunc = 0;
	if( displacement ) {
		pFunc = pFunc2DManager->GetItem( displacement );
		if( !pFunc ) {
			GlobalLog()->PrintEx( eLog_Error, "Job::AddBezierTriangleMeshGeometry:: Displacement function `%s` not found", displacement );
		}
	}

	ITriangleMeshLoaderIndexed* pLoader = 0;
	RISE_API_CreateBezierTriangleMeshLoader( &pLoader, szFileName, detail, bCombineSharedVertices, bCenterObject, pFunc, disp_scale );

	bool bRet = pLoader->LoadTriangleMesh( pGeometry );
	if( bRet ) {
		pGeomManager->AddItem( pGeometry, name );
	}

	safe_release( pGeometry );

	safe_release( pLoader );
	return bRet;
}

//! Creates a triangle mesh geometry from a ply file
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPLYTriangleMeshGeometry(
					const char* name,						///< [in] Name of the geometry
					const char* szFileName,					///< [in] Name of the file to load from
					const unsigned int max_polys,			///< [in] Maximum number of polygons in one octant node
					const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
					const bool double_sided,				///< [in] Are the triangles double sided ?
					const bool use_bsp,						///< [in] Use a BSP tree rather than an Octree
					const bool bInvertFaces,				///< [in] Should the faces be inverted?
					const bool face_normals					///< [in] Use face normals rather than vertex normals
					)
{
	ITriangleMeshGeometryIndexed* pGeometry = 0;
	RISE_API_CreateTriangleMeshGeometryIndexed( &pGeometry, max_polys, max_recur, double_sided, use_bsp, face_normals );

	ITriangleMeshLoaderIndexed* pLoader = 0;
	RISE_API_CreatePLYTriangleMeshLoader( &pLoader, szFileName, bInvertFaces );

	bool bRet = pLoader->LoadTriangleMesh( pGeometry );
	if( bRet ) {
		pGeomManager->AddItem( pGeometry, name );
	}

	safe_release( pGeometry );

	safe_release( pLoader );
	return bRet;
}

//! Creates a mesh from a .risemesh file
/// \return TRUE if successful, FALSE otherwise
bool Job::AddRISEMeshTriangleMeshGeometry(
					const char* name,						///< [in] Name of the geometry
					const char* szFileName,					///< [in] Name of the file to load from
					const bool load_into_memory,			///< [in] Do we load the entire file into memory before reading?
					const bool face_normals					///< [in] Use face normals rather than vertex normals
					)
{
	ITriangleMeshGeometryIndexed* pGeometry = 0;
	RISE_API_CreateTriangleMeshGeometryIndexed( &pGeometry, 0, 0, false, true, face_normals );

	if( load_into_memory ) {
		IMemoryBuffer* pBuffer = 0;
		RISE_API_CreateMemoryBufferFromFile( &pBuffer, szFileName );

		pGeometry->Deserialize( *pBuffer );
		safe_release( pBuffer );
	} else {
		IReadBuffer* pBuffer = 0;
		RISE_API_CreateDiskFileReadBuffer( &pBuffer, szFileName );

		pGeometry->Deserialize( *pBuffer );
		safe_release( pBuffer );
	}

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );

	return true;
}

//! Creates a bezier patch geometry
/// \return TRUE if successful, FALSE otherwise
bool Job::AddBezierPatchGeometry(
					const char* name,						///< [in] Name of the geometry
					const char* szFileName,					///< [in] Name of the file to load from
					const unsigned int max_patches,			///< [in] Maximum number of patches / octant node
					const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
					const bool use_bsp,						///< [in] Use a BSP tree rather than an Octree
					const bool bAnalytic,					///< [in] Should the patches be analytically rendered?
					const unsigned int cache_size,			///< [in] Size of the geometry cache for non-analytic rendering
					const unsigned int max_polys,			///< [in] Maximum polygons / triangle octant node
					const unsigned char max_poly_recursion, ///< [in] Maximum polygon recursion
					const bool bDoubleSided,				///< [in] Are generated polygons double sided ?
					const bool bPolyUseBSP,					///< [in] Should the polygon list use a BSP tree?
					const bool bUseFaceNormals,				///< [in] Should we use face normals rather than vertex normals?
					const unsigned int detail,				///< [in] Level of tesselation for polygons
					const char* displacement,				///< [in] Displacement function for static displacement mapping
					const double disp_scale					///< [in] Displacement scale factor
					)
{
	FILE* inputFile = fopen( GlobalMediaPathLocator().Find(szFileName).c_str(), "r" );

	if( !inputFile ) {
		GlobalLog()->Print( eLog_Error, "Job::AddBezierPatchGeometry:: Failed to open file" );
		return false;
	}

	IFunction2D* pFunc = 0;
	if( displacement ) {
		pFunc = pFunc2DManager->GetItem( displacement );
		if( !pFunc ) {
			GlobalLog()->PrintEx( eLog_Error, "Job::AddBezierPatchGeometry:: Displacement function `%s` not found", displacement );
		}
	}

	IBezierPatchGeometry* pGeometry = 0;
	RISE_API_CreateBezierPatchGeometry( &pGeometry, max_patches, max_recur, use_bsp, bAnalytic, cache_size, max_polys, max_poly_recursion, bDoubleSided, bPolyUseBSP, bUseFaceNormals, detail, pFunc, disp_scale );

	char line[4096] = {0};

	if( fgets( (char*)&line, 4096, inputFile ) != NULL ) {
		// Read that first line, it tells us how many
		// patches are dealing with here
		unsigned int	numPatches = 0;
		sscanf( line, "%u", &numPatches );

		for( unsigned int i=0; i<numPatches; i++ )
		{
			// We assume every 16 lines gives us a patch
			BezierPatch		patch;

			for( int j=0; j<4; j++ ) {
				for( int k=0; k<4; k++ ) {
					double x, y, z;
					if( fscanf( inputFile, "%lf %lf %lf", &x, &y, &z ) == EOF ) {
						GlobalLog()->PrintSourceError( "TriangleMeshLoaderBezier:: Fatal error while reading file.  Nothing will be loaded", __FILE__, __LINE__ );
						return false;
					}

					patch.c[j].pts[k] = Point3( x, y, z );
				}
			}

			pGeometry->AddPatch( patch );
		}

		pGeometry->Prepare();
	}

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//! Creates a bilinear patch geometry
/// \return TRUE if successful, FALSE otherwise
bool Job::AddBilinearPatchGeometry(
					const char* name,						///< [in] Name of the geometry
					const char* szFileName,					///< [in] Name of the file to load from
					const unsigned int max_polys,			///< [in] Maximum number of polygons in one octant node
					const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
					const bool use_bsp						///< [in] Use a BSP tree rather than an Octree
					)
{
	FILE* inputFile = fopen( GlobalMediaPathLocator().Find(szFileName).c_str(), "r" );

	if( !inputFile ) {
		GlobalLog()->Print( eLog_Error, "Job::AddBilinearPatchGeometry:: Failed to open file" );
		return false;
	}

	IBilinearPatchGeometry* pGeometry = 0;
	RISE_API_CreateBilinearPatchGeometry( &pGeometry, max_polys, max_recur, use_bsp );

	char line[1024] = {0};

	if( fgets( (char*)&line, 1024, inputFile ) != NULL ) {
		// Read that first line, it tells us how many
		// patches are dealing with here
		unsigned int	numPatches = 0;
		sscanf( line, "%u", &numPatches );

		for( unsigned int i=0; i<numPatches; i++ )
		{
			// We assume every 16 lines gives us a patch
			BilinearPatch		patch;

			for( int j=0; j<4; j++ ) {
				// Each line is a control point
				if( fgets( (char*)&line, 1024, inputFile ) == NULL ) {
					GlobalLog()->PrintSourceError( "Job::AddBilinearPatchGeometry:: Fatal error while reading file.  Nothing will be loaded", __FILE__, __LINE__ );
					return false;
				}

				double x, y, z;
				sscanf( line, "%lf %lf %lf", &x, &y, &z );
				patch.pts[j] = Point3( x, y, z );
			}

			pGeometry->AddPatch( patch );
		}

		pGeometry->Prepare();
	}

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//
//  Adds lights
//

//! Creates a infinite point omni light, located at the origin
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPointOmniLight(
	const char* name,										///< [in] Name of the light
	const double power,										///< [in] Power of the light in watts
	const double srgb[3],									///< [in] Color of the light in a non-linear colorspace
	const double pos[3],									///< [in] Position of the light
	const double linearAttenuation,							///< [in] Amount of linear attenuation
	const double quadraticAttenuation						///< [in] Amount of quadratic attenuation
	)
{
	ILightPriv* pLight = 0;
	RISE_API_CreatePointOmniLight( &pLight, power, sRGBPel(srgb), linearAttenuation, quadraticAttenuation );
	pLight->SetPosition( Point3( pos ) );
	pLight->FinalizeTransformations();
	pLightManager->AddItem( pLight, name );
	safe_release( pLight );
	return true;
}

//! Creates a infinite point spot light
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPointSpotLight(
	const char* name,										///< [in] Name of the light
	const double power,										///< [in] Power of the light in watts
	const double srgb[3],									///< [in] Color of the light in a non-linear colorspace
	const double foc[3],									///< [in] Point the center of the light is focussing on
	const double inner,										///< [in] Angle of the inner cone in radians
	const double outer,										///< [in] Angle of the outer cone in radians
	const double pos[3],									///< [in] Position of the light
	const double linearAttenuation,							///< [in] Amount of linear attenuation
	const double quadraticAttenuation						///< [in] Amount of quadratic attenuation
	)
{
	ILightPriv* pLight = 0;
	RISE_API_CreatePointSpotLight( &pLight, power, sRGBPel(srgb),Point3(foc), inner, outer, linearAttenuation, quadraticAttenuation );
	pLight->SetPosition( Point3( pos ) );
	pLight->FinalizeTransformations();
	pLightManager->AddItem( pLight, name );
	safe_release( pLight );
	return true;
}

//! Creates the ambient light
/// \return TRUE if successful, FALSE otherwise
bool Job::AddAmbientLight(
	const char* name,										///< [in] Name of the light
	const double power,										///< [in] Power of the light in watts
	const double srgb[3]									///< [in] Color of the light in a non-linear colorspace
	)
{
	ILightPriv* pLight = 0;
	RISE_API_CreateAmbientLight( &pLight, power, sRGBPel(srgb) );
	pLightManager->AddItem( pLight, name );
	safe_release( pLight );
	return true;
}

//! Adds an infinite directional light, shining in a particular direction
/// \return TRUE if successful, FALSE otherwise
bool Job::AddDirectionalLight(
	const char* name,										///< [in] Name of the light
	const double power,										///< [in] Power of the light in watts
	const double srgb[3],									///< [in] Color of the light in a non-linear colorspace
	const double dir[3]										///< [in] Direction of the light
	)
{
	ILightPriv* pLight = 0;
	RISE_API_CreateDirectionalLight( &pLight, power, sRGBPel(srgb), Vector3(dir) );
	pLightManager->AddItem( pLight, name );
	safe_release( pLight );
	return true;
}

//
// Adds functions
//

//! Adds a piecewise linear function
bool Job::AddPiecewiseLinearFunction(
	const char* name,										///< [in] Name of the function
	const double x[],										///< [in] X values of the function
	const double y[],										///< [in] Y values of the function
	const unsigned int num,									///< [in] Number of control points in the x and y arrays
	const bool bUseLUTs,									///< [in] Should the function use lookup tables
	const unsigned int lutsize								///< [in] Size of the lookup table
	)
{
	IPiecewiseFunction1D* pFunction = 0;
	RISE_API_CreatePiecewiseLinearFunction1D( &pFunction );

	for( unsigned int i=0; i<num; i++ ) {
		pFunction->addControlPoint( std::make_pair( x[i], y[i] ) );
	}

	if( bUseLUTs ) {
		pFunction->GenerateLUT( lutsize );
		pFunction->setUseLUT( true );
	}

	IPainter* pPainter = 0;
	RISE_API_CreateFunction1DSpectralPainter( &pPainter, *pFunction );

	pPntManager->AddItem( pPainter, name );
	safe_release( pPainter );

	pFunc1DManager->AddItem( pFunction, name );
	safe_release( pFunction );

	return true;
}

//! Adds a 2D piecewise linear function built up of other functions
bool Job::AddPiecewiseLinearFunction2D(
	const char* name,										///< [in] Name of the function
	const double x[],										///< [in] X values of the function
	char** y,												///< [in] Y values which is the name of other function1Ds
	const unsigned int num									///< [in] Number of control points in the x and y arrays
	)
{
	IPiecewiseFunction2D* pFunction = 0;
	RISE_API_CreatePiecewiseLinearFunction2D( &pFunction );

	for( unsigned int i=0; i<num; i++ ) {
		IFunction1D* pFunc = pFunc1DManager->GetItem( y[i] );
		if( !pFunc ) {
			GlobalLog()->PrintEx( eLog_Error, "Job::AddPiecewiseLinearFunction2D:: Failed to find function '%s'", y[i] );
			return false;
		}
		pFunction->addControlPoint( x[i], pFunc );
	}

	pFunc2DManager->AddItem( pFunction, name );
	safe_release( pFunction );

	return true;
}

//
// Adding modifiers
//

bool Job::AddBumpMapModifier(
	const char* name,										///< [in] Name of the modifiers
	const char* func,										///< [in] The function to use as the bump generator
	const double scale,										///< [in] Factor to scale values by
	const double window										///< [in] Size of the window
	)
{
	IFunction2D* pFunc = pFunc2DManager->GetItem( func );
	if( !pFunc ) {
		return false;
	}

	IRayIntersectionModifier* pModifier = 0;
	RISE_API_CreateBumpMapModifier( &pModifier, *pFunc, scale, window );

	pModManager->AddItem( pModifier, name );
	safe_release( pModifier );
	return true;
}

//
// Adding objects
//

//! Adds an object
/// \return TRUE if successful, FALSE otherwise
bool Job::AddObject(
	const char* name,										///< [in] Name of the object
	const char* geom,										///< [in] Name of the geometry for the object
	const char* material,									///< [in] Name of the material
	const char* modifier,									///< [in] Name of the modifier
	const char* shader,										///< [in] Name of the shader
	const char* radiancemap,								///< [in] Name of the painter to use as a radiance map
	const double radiancemap_scale,							///< [in] How much to scale the radiance map values by
	const double radiance_orient[3],						///< [in] Orientation of the object
	const double pos[3],									///< [in] Position of the object
	const double orient[3],									///< [in] Orientation of the object
	const double scale[3],									///< [in] Object scaling
	const bool bCastsShadows,								///< [in] Does the object cast shadows?
	const bool bReceivesShadows								///< [in] Does the object receive shadows?
   )
{
	IGeometry* pGeometry = pGeomManager->GetItem( geom );

	if( !pGeometry ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::AddObject:: Geometry not found `%s`", geom );
		return false;
	}

	IObjectPriv* object = 0;
	RISE_API_CreateObject( &object, pGeometry );

	object->SetShadowParams( bCastsShadows, bReceivesShadows );

	if( material ) {
		IMaterial* pMat = pMatManager->GetItem(material);
		if( pMat ) {
			object->AssignMaterial( *pMat );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::AddObject:: Material not found `%s`", material );
			return false;
		}
	}

	if( modifier ) {
		IRayIntersectionModifier* pMod = pModManager->GetItem(modifier);
		if( pMod ) {
			object->AssignModifier( *pMod );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::AddObject:: Modifier not found `%s`", modifier );
			return false;
		}
	}

	if( shader ) {
		IShader* pShader = pShaderManager->GetItem(shader);
		if( pShader ) {
			object->AssignShader( *pShader );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::AddObject:: Shader not found `%s`", shader );
			return false;
		}
	}

	if( radiancemap ) {
		IPainter* pPnt = pPntManager->GetItem(radiancemap);
		if( pPnt ) {
			IRadianceMap* pRadianceMap = 0;
			RISE_API_CreateRadianceMap( &pRadianceMap, *pPnt, radiancemap_scale );
			pRadianceMap->SetOrientation( Vector3( radiance_orient ) );
			object->AssignRadianceMap( *pRadianceMap );
			safe_release( pRadianceMap );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::AddObject:: Painter for radiance map not found `%s`", radiancemap );
			return false;
		}
	}

	object->SetPosition( Point3( pos ) );
	object->SetOrientation( Vector3( orient ) );
	object->SetStretch( Vector3( scale[0], scale[1], scale[2] ) );
	object->FinalizeTransformations();

	pObjectManager->AddItem( object, name );
	safe_release( object );

	return true;
}

//! Creates a CSG object
/// \return TRUE if successful, FALSE otherwise
bool Job::AddCSGObject(
	const char* name,										///< [in] Name of the object
	const char* objA,										///< [in] Name of the first object
	const char* objB,										///< [in] Name of the second object
	const char op,											///< [in] CSG operation
															///< 0 -> Union
															///< 1 -> Intersection
															///< 2 -> A-B
															///< 3 -> B-A
	const char* material,									///< [in] Name of the material
	const char* modifier,									///< [in] Name of the modifier
	const char* shader,										///< [in] Name of the shader
	const char* radiancemap,								///< [in] Name of the painter to use as a radiance map
	const double radiancemap_scale,							///< [in] How much to scale the radiance map values by
	const double radiance_orient[3],						///< [in] Orientation of the object
	const double pos[3],									///< [in] Position of the object
	const double orient[3],									///< [in] Orientation of the object
	const bool bCastsShadows,								///< [in] Does the object cast shadows?
	const bool bReceivesShadows								///< [in] Does the object receive shadows?
	)
{
	IObjectPriv* object = 0;
	RISE_API_CreateCSGObject(
		&object,
		pObjectManager->GetItem(objA),
		pObjectManager->GetItem(objB),
		op );

	object->SetShadowParams( bCastsShadows, bReceivesShadows );

	if( material ) {
		IMaterial* pMat = pMatManager->GetItem(material);
		if( pMat ) {
			object->AssignMaterial( *pMat );
		}
	}

	if( modifier ) {
		IRayIntersectionModifier* pMod = pModManager->GetItem(modifier);
		if( pMod ) {
			object->AssignModifier( *pMod );
		}
	}

	if( shader ) {
		IShader* pShader = pShaderManager->GetItem(shader);
		if( pShader ) {
			object->AssignShader( *pShader );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::AddObject:: Shader not found `%s`", modifier );
		}
	}

	if( radiancemap ) {
		IPainter* pPnt = pPntManager->GetItem(radiancemap);
		if( pPnt ) {
			IRadianceMap* pRadianceMap = 0;
			RISE_API_CreateRadianceMap( &pRadianceMap, *pPnt, radiancemap_scale );
			pRadianceMap->SetOrientation( Vector3( radiance_orient ) );
			object->AssignRadianceMap( *pRadianceMap );
			safe_release( pRadianceMap );
			safe_release( pPnt );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::AddCSGObject:: Painter for radiance map not found `%s`", radiancemap );
		}
	}

	object->SetPosition( Point3( pos ) );
	object->SetOrientation( Vector3( orient ) );
	object->FinalizeTransformations();

	pObjectManager->AddItem( object, name );
	safe_release( object );

	return true;
}

//
// Adds ShaderOps
//
bool Job::AddReflectionShaderOp(
	const char* name										///< [in] Name of the shaderop
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateReflectionShaderOp( &pShaderOp );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddRefractionShaderOp(
	const char* name										///< [in] Name of the shaderop
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateRefractionShaderOp( &pShaderOp );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddEmissionShaderOp(
	const char* name										///< [in] Name of the shaderop
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateEmissionShaderOp( &pShaderOp );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddDirectLightingShaderOp(
	const char* name,										///< [in] Name of the shaderop
	const char* bsdf,										///< [in] BSDF to use when computing radiance (overrides object BSDF)
	const bool nonmeshlights,								///< [in] Compute lighting from non mesh lights?
	const bool meshlights,									///< [in] Compute lighting from mesh lights (area light sources)?
	const bool cache										///< [in] Should the rasterizer state cache be used?
	)
{
	IMaterial* pBSDF = 0;
	if( bsdf ) {
		pBSDF = pMatManager->GetItem( bsdf );
	}

	IShaderOp* pShaderOp = 0;
	RISE_API_CreateDirectLightingShaderOp( &pShaderOp, pBSDF, nonmeshlights, meshlights, cache );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddCausticPelPhotonMapShaderOp(
	const char* name										///< [in] Name of the shaderop
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateCausticPelPhotonMapShaderOp( &pShaderOp );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddCausticSpectralPhotonMapShaderOp(
	const char* name										///< [in] Name of the shaderop
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateCausticSpectralPhotonMapShaderOp( &pShaderOp );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddGlobalPelPhotonMapShaderOp(
	const char* name										///< [in] Name of the shaderop
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateGlobalPelPhotonMapShaderOp( &pShaderOp );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddGlobalSpectralPhotonMapShaderOp(
	const char* name										///< [in] Name of the shaderop
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateGlobalSpectralPhotonMapShaderOp( &pShaderOp );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddTranslucentPelPhotonMapShaderOp(
	const char* name										///< [in] Name of the shaderop
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateTranslucentPelPhotonMapShaderOp( &pShaderOp );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddShadowPhotonMapShaderOp(
	const char* name										///< [in] Name of the shaderop
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateShadowPhotonMapShaderOp( &pShaderOp );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddDistributionTracingShaderOp(
	const char* name,										///< [in] Name of the shaderop
	const unsigned int samples,								///< [in] Number of sample to use in distribution
	const bool irradiancecaching,							///< [in] Should irradiance caching be used if available?
	const bool forcecheckemitters,							///< [in] Force rays allowing to hit emitters even though the material may have a BRDF
	const bool branch,										///< [in] Should we branch when doing scattering?
	const bool reflections,									///< [in] Should reflections be traced?
	const bool refractions,									///< [in] Should refractions be traced?
	const bool diffuse,										///< [in] Should diffuse rays be traced?
	const bool translucents									///< [in] Should translucent rays be traced?
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateDistributionTracingShaderOp( &pShaderOp, samples, irradiancecaching, forcecheckemitters, branch, reflections, refractions, diffuse, translucents );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddFinalGatherShaderOp(
	const char* name,										///< [in] Name of the shaderop
	const unsigned int numtheta,							///< [in] Number of samples in the theta direction
	const unsigned int numphi,								///< [in] Number of samples in the phi direction
	const bool cachegradients,								///< [in] Should cache gradients be used in the irradiance cache?
	const bool cache										///< [in] Should the rasterizer state cache be used?
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateFinalGatherShaderOp( &pShaderOp, numtheta, numphi, cachegradients, cache );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddPathTracingShaderOp(
	const char* name,										///< [in] Name of the shaderop
	const bool branch,										///< [in] Should we branch the rays ?
	const bool forcecheckemitters,							///< [in] Force rays allowing to hit emitters even though the material may have a BRDF
	const bool bFinalGather,								///< [in] Should the path tracer co-operate and act as final gather?
	const bool reflections,									///< [in] Should reflections be traced?
	const bool refractions,									///< [in] Should refractions be traced?
	const bool diffuse,										///< [in] Should diffuse rays be traced?
	const bool translucents									///< [in] Should translucent rays be traced?
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreatePathTracingShaderOp( &pShaderOp, branch, forcecheckemitters, bFinalGather, reflections, refractions, diffuse, translucents );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddAmbientOcclusionShaderOp(
	const char* name,										///< [in] Name of the shaderop
	const unsigned int numtheta,							///< [in] Number of samples in the theta direction
	const unsigned int numphi,								///< [in] Number of samples in the phi direction
	const bool multiplybrdf,								///< [in] Should individual samples be multiplied by the BRDF ?
	const bool irradiance_cache								///< [in] Should the irradiance state cache be used?
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateAmbientOcclusionShaderOp( &pShaderOp, numtheta, numphi, multiplybrdf, irradiance_cache );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddSimpleSubSurfaceScatteringShaderOp(
	const char* name,										///< [in] Name of the shaderop
	const unsigned int numPoints,							///< [in] Number of points to use in sampling
	const double error,										///< [in] Error tolerance for bounding the number of point samples
	const unsigned int maxPointsPerNode,					///< [in] Maximum number of points / octree node
	const unsigned char maxDepth,							///< [in] Maximum depth of the octree
	const double irrad_scale,								///< [in] Irradiance scale factor
	const double geometric_scale,							///< [in] Geometric scale factor
	const bool multiplyBSDF,								///< [in] Should the BSDF be evaluated at the point of exitance?
	const bool regenerate,									///< [in] Regenerate the point set on reset calls?
	const char* shader,										///< [in] Shader to use for irradiance calculations
	const bool cache,										///< [in] Should the rasterizer state cache be used?
	const bool low_discrepancy,								///< [in] Should use a low discrepancy sequence during sample point generation?
	const double extinction[3]								///< [in] Extinction in mm^-1
	)
{
	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::AddSimpleSubSurfaceScatteringShaderOp:: Shader not found '%s'", shader );
		return false;
	}

	IShaderOp* pShaderOp = 0;
	RISE_API_CreateSimpleSubSurfaceScatteringShaderOp( &pShaderOp, numPoints, error, maxPointsPerNode, maxDepth, irrad_scale, geometric_scale, multiplyBSDF, regenerate, *pShader, cache, low_discrepancy, RISEPel(extinction) );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddDiffusionApproximationSubSurfaceScatteringShaderOp(
	const char* name,										///< [in] Name of the shaderop
	const unsigned int numPoints,							///< [in] Number of points to use in sampling
	const double error,										///< [in] Error tolerance for bounding the number of point samples
	const unsigned int maxPointsPerNode,					///< [in] Maximum number of points / octree node
	const unsigned char maxDepth,							///< [in] Maximum depth of the octree
	const double irrad_scale,								///< [in] Irradiance scale factor
	const double geometric_scale,							///< [in] Geometric scale factor
	const bool multiplyBSDF,								///< [in] Should the BSDF be evaluated at the point of exitance?
	const bool regenerate,									///< [in] Regenerate the point set on reset calls?
	const char* shader,										///< [in] Shader to use for irradiance calculations
	const bool cache,										///< [in] Should the rasterizer state cache be used?
	const bool low_discrepancy,								///< [in] Should use a low discrepancy sequence during sample point generation?
	const double scattering[3],								///< [in] Scattering coefficient in mm^-1
	const double absorption[3],								///< [in] Absorption coefficient in mm^-1
	const double ior,										///< [in] Index of refraction ratio
	const double g											///< [in] Scattering asymmetry
	)
{
	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::AddDiffusionApproximationSubSurfaceScatteringShaderOp:: Shader not found '%s'", shader );
		return false;
	}

	IShaderOp* pShaderOp = 0;
	RISE_API_CreateDiffusionApproximationSubSurfaceScatteringShaderOp( &pShaderOp, numPoints, error, maxPointsPerNode, maxDepth, irrad_scale, geometric_scale, multiplyBSDF, regenerate, *pShader, cache, low_discrepancy, RISEPel(scattering), RISEPel(absorption), ior, g );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddAreaLightShaderOp(
		const char* name,										///< [in] Name of the shaderop
		const double width,										///< [in] Width of the light source
		const double height,									///< [in] Height of the light source
		const double location[3],								///< [in] Where is the light source located
		const double dir[3],									///< [in] What is the light source focussed on
		const unsigned int samples,								///< [in] Number of samples to take
		const char* emm,										///< [in] Emission painter of this light
		const double power,										///< [in] Power scale
		const char* N,											///< [in] Phong factor for focussing the light
		const double hotSpot,									///< [in] Angle in radians of the light's hot spot
		const bool cache										///< [in] Should the rasterizer state cache be used?
		)

{
	IPainter* pEmm = pPntManager->GetItem( emm );
	if( !pEmm ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::AddAreaLightShaderOp: Painter not found '%s'", emm );
		return false;
	}

	IPainter*		pN = pPntManager->GetItem( N );

	if( !pN )
	{
		double fn = atof(N);
		RISE_API_CreateUniformColorPainter( &pN, RISEPel(fn,fn,fn) );
	} else {
		pN->addref();
	}

	IShaderOp* pShaderOp = 0;
	RISE_API_CreateAreaLightShaderOp( &pShaderOp, width, height, Point3(location), Vector3Ops::Normalize(Vector3(dir)), samples, *pEmm, power, *pN, hotSpot, cache );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	safe_release( pN );
	return true;
}

bool Job::AddTransparencyShaderOp(
		const char* name,										///< [in] Name of the shaderop
		const char* transparency,								///< [in] Transparency painter
		const bool one_sided									///< [in] One sided transparency only (ignore backfaces)
		)
{
	IPainter* pTrans = pPntManager->GetItem( transparency );
	if( !pTrans ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::AddTransparencyShaderOp: Painter not found '%s'", transparency );
		return false;
	}

	IShaderOp* pShaderOp = 0;
	RISE_API_CreateTransparencyShaderOp( &pShaderOp, *pTrans, one_sided );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

//
// Adds Shaders
//

bool Job::AddStandardShader(
	const char* name,										///< [in] Name of the shader
	const unsigned int count,								///< [in] Number of shaderops
	const char** shaderops									///< [in] All of the shaderops
	)
{
	std::vector<IShaderOp*> shops;

	for( unsigned int i=0; i<count; i++ ) {
		IShaderOp* pShaderOp = pShaderOpManager->GetItem( shaderops[i] );
		if( pShaderOp ) {
			shops.push_back( pShaderOp );
		} else {
			GlobalLog()->PrintEx( eLog_Error, "Job::AddStandardShader:: The ShaderOp '%s' not found, failed to add shader", shaderops[i] );
			return false;
		}
	}

	IShader* pShader = 0;

	RISE_API_CreateStandardShader( &pShader, shops );

	pShaderManager->AddItem( pShader, name );
	safe_release( pShader );
	return true;
}

bool Job::AddAdvancedShader(
		const char* name,										///< [in] Name of the shader
		const unsigned int count,								///< [in] Number of shaderops
		const char** shaderops,									///< [in] All of the shaderops
		const unsigned int* mindepths,							///< [in] All of the minimum depths for the shaderops
		const unsigned int* maxdepths,							///< [in] All of the maximum depths for the shaderops
		const char* operations									///< [in] All the operations for the shaderops
		)
{
	std::vector<IShaderOp*> shops;
	std::vector<unsigned int> mins, maxs;

	for( unsigned int i=0; i<count; i++ ) {
		IShaderOp* pShaderOp = pShaderOpManager->GetItem( shaderops[i] );
		if( pShaderOp ) {
			shops.push_back( pShaderOp );
			mins.push_back( mindepths[i] );
			maxs.push_back( maxdepths[i] );
		} else {
			GlobalLog()->PrintEx( eLog_Error, "Job::AddStandardShader:: The ShaderOp '%s' not found, failed to add shader", shaderops[i] );
			return false;
		}
	}

	IShader* pShader = 0;

	RISE_API_CreateAdvancedShader( &pShader, shops, mins, maxs, operations );

	pShaderManager->AddItem( pShader, name );
	safe_release( pShader );
	return true;
}

bool Job::AddDirectVolumeRenderingShader(
	const char* name,										///< [in] Name of the shader
	const char* szVolumeFilePattern,						///< [in] File pattern for volume data
	const unsigned int width,								///< [in] Width of the volume
	const unsigned int height,								///< [in] Height of the volume
	const unsigned int startz,								///< [in] Starting z value for volume data
	const unsigned int endz,								///< [in] Ending z value for the volume data
	const char accessor,									///< [in] Type of volume accessor
	const char gradient,									///< [in] Type of gradient estimator to use
	const char composite,									///< [in] Type of composite operation to use
	const double dThresholdStart,							///< [in] Start of ISO threshold value (for ISO renderings only)
	const double dThresholdEnd,								///< [in] End of ISO threshold value (for ISO renderings only)
	const char sampler,										///< [in] Type of sampler to use
	const unsigned int samples,								///< [in] Number of samples along the ray to take
	const char* transfer_red,								///< [in] Name of the transfer function for the red channel
	const char* transfer_green,								///< [in] Name of the transfer function for the green channel
	const char* transfer_blue,								///< [in] Name of the transfer function for the blue channel
	const char* transfer_alpha,								///< [in] Name of the transfer function for the alpha channel
	const char* iso_shader									///< [in] Shader to use for ISO surface rendering (optional)
	)
{
	IFunction1D* pRed = pFunc1DManager->GetItem( transfer_red );
	IFunction1D* pGreen = pFunc1DManager->GetItem( transfer_green );
	IFunction1D* pBlue = pFunc1DManager->GetItem( transfer_blue );
	IFunction1D* pAlpha = pFunc1DManager->GetItem( transfer_alpha );

	if( !pRed || !pGreen || !pBlue || !pAlpha ) {
		GlobalLog()->PrintEasyError( "Job::AddDirectVolumeRenderingShader:: Could not find out of the transfer functions" );
		return 0;
	}

	ISampling1D* pSampler = 0;

	switch( sampler ) {
		default:
		case 'u':
			RISE_API_CreateUniformSampling1D( &pSampler, 1.0 );
			break;
		case 'j':
			RISE_API_CreateJitteredSampling1D( &pSampler, 1.0 );
			break;
	}

	pSampler->SetNumSamples( samples );

	IShader* pISOShader = 0;
	if( iso_shader ) {
		pISOShader = pShaderManager->GetItem(iso_shader);
		if( !pISOShader ) {
			GlobalLog()->PrintEx( eLog_Warning, "Job::AddDirectVolumeRenderingShader:: Shader not found `%s`", iso_shader );
		}
	}


	IShader* pShader = 0;

	RISE_API_CreateDirectVolumeRenderingShader( &pShader, szVolumeFilePattern, width, height, startz, endz, accessor,
		gradient, composite, dThresholdStart, dThresholdEnd, *pSampler, *pRed, *pGreen, *pBlue, *pAlpha, pISOShader );

	pShaderManager->AddItem( pShader, name );
	safe_release( pShader );
	return true;
}

bool Job::AddSpectralDirectVolumeRenderingShader(
	const char* name,										///< [in] Name of the shader
	const char* szVolumeFilePattern,						///< [in] File pattern for volume data
	const unsigned int width,								///< [in] Width of the volume
	const unsigned int height,								///< [in] Height of the volume
	const unsigned int startz,								///< [in] Starting z value for volume data
	const unsigned int endz,								///< [in] Ending z value for the volume data
	const char accessor,									///< [in] Type of volume accessor
	const char gradient,									///< [in] Type of gradient estimator to use
	const char composite,									///< [in] Type of composite operation to use
	const double dThresholdStart,							///< [in] Start of ISO threshold value (for ISO renderings only)
	const double dThresholdEnd,								///< [in] End of ISO threshold value (for ISO renderings only)
	const char sampler,										///< [in] Type of sampler to use
	const unsigned int samples,								///< [in] Number of samples along the ray to take
	const char* transfer_alpha,								///< [in] Name of the transfer function for the alpha channel
	const char* transfer_spectral,							///< [in] Name of the spectral transfer function
	const char* iso_shader									///< [in] Shader to use for ISO surface rendering (optional)
	)
{
	IFunction1D* pAlpha = pFunc1DManager->GetItem( transfer_alpha );

	if( !pAlpha ) {
		GlobalLog()->PrintEasyError( "Job::AddDirectVolumeRenderingShader:: Could not find alpha transfer functions" );
		return 0;
	}

	IFunction2D* pSpectral = pFunc2DManager->GetItem( transfer_spectral );

	if( !pSpectral ) {
		GlobalLog()->PrintEasyError( "Job::AddDirectVolumeRenderingShader:: Could not find spectral transfer functions" );
		return 0;
	}

	ISampling1D* pSampler = 0;

	switch( sampler ) {
		default:
		case 'u':
			RISE_API_CreateUniformSampling1D( &pSampler, 1.0 );
			break;
		case 'j':
			RISE_API_CreateJitteredSampling1D( &pSampler, 1.0 );
			break;
	}

	pSampler->SetNumSamples( samples );

	IShader* pISOShader = 0;
	if( iso_shader ) {
		pISOShader = pShaderManager->GetItem(iso_shader);
		if( !pISOShader ) {
			GlobalLog()->PrintEx( eLog_Warning, "Job::AddDirectVolumeRenderingShader:: Shader not found `%s`", iso_shader );
		}
	}


	IShader* pShader = 0;

	RISE_API_CreateSpectralDirectVolumeRenderingShader( &pShader, szVolumeFilePattern, width, height, startz, endz, accessor,
		gradient, composite, dThresholdStart, dThresholdEnd, *pSampler, *pAlpha, *pSpectral, pISOShader );

	pShaderManager->AddItem( pShader, name );
	safe_release( pShader );
	return true;
}

//
// Sets Rasterization parameters
//

bool GetSamplingAndFilterElements(
	ISampling2D** pPixelSampler,
	ISampling2D** pLumSampler,
	IPixelFilter** pPixelFilter,
	const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
	const unsigned int numLumSamples,						///< [in] Number of samples / lumin
	const char* pixelSampler,								///< [in] Type of sampling to use for the pixel sampler
	const double pixelSamplerParam,							///< [in] Parameter for the pixel sampler
	const char* luminarySampler,							///< [in] Type of sampling to use for luminaries
	const double luminarySamplerParam,						///< [in] Parameter for the luminary sampler
	const char* pixelFilter,								///< [in] Type of filtering to use for the pixels
	const double pixelFilterWidth,							///< [in] How wide is the pixel filter?
	const double pixelFilterHeight,							///< [in] How high is the pixel filter?
	const double pixelFilterParamA,							///< [in] Pixel filter parameter A
	const double pixelFilterParamB							///< [in] Pixel filter parameter B
	)
{
	if( numPixelSamples > 1) {
		if( pixelSampler ) {
			String sPixelSampler( pixelSampler );
			if( sPixelSampler == "nrooks" ) {
				RISE_API_CreateNRooksSampling2D( pPixelSampler, 1.0, 1.0, pixelSamplerParam );
			} else if( sPixelSampler == "uniform" ) {
				RISE_API_CreateUniformSampling2D( pPixelSampler, 1.0, 1.0 );
			} else if( sPixelSampler == "random" ) {
				RISE_API_CreateRandomSampling2D( pPixelSampler, 1.0, 1.0 );
			} else if( sPixelSampler == "stratified" ) {
				RISE_API_CreateStratifiedSampling2D( pPixelSampler, 1.0, 1.0, pixelSamplerParam );
			} else if( sPixelSampler == "poisson" ) {
				RISE_API_CreatePoissonDiskSampling2D( pPixelSampler, 1.0, 1.0, pixelSamplerParam );
			} else if( sPixelSampler == "multijittered" ) {
				RISE_API_CreateMultiJitteredSampling2D( pPixelSampler, 1.0, 1.0 );
			} else if( sPixelSampler == "halton" ) {
				RISE_API_CreateHaltonPointsSampling2D( pPixelSampler, 1.0, 1.0 );
			} else {
				GlobalLog()->PrintEx( eLog_Error, "Unknown sampler type: `%s`", pixelSampler );
				return false;
			}
		} else {
			RISE_API_CreateMultiJitteredSampling2D( pPixelSampler, 1.0, 1.0 );
		}
		(*pPixelSampler)->SetNumSamples( numPixelSamples );

		if( pixelFilter ) {
			String sPixelFilter( pixelFilter );
			if( sPixelFilter == "box" ) {
				RISE_API_CreateBoxPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
			} else if( sPixelFilter == "tent" ) {
				RISE_API_CreateTrianglePixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
			} else if( sPixelFilter == "gaussian" ) {
				RISE_API_CreateGaussianPixelFilter( pPixelFilter, pixelFilterParamA, pixelFilterParamB );
			} else if( sPixelFilter == "sinc" ) {
				RISE_API_CreateSincPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
			} else if( sPixelFilter == "windowed_sinc_box" ) {
				RISE_API_CreateBoxWindowedSincPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
			} else if( sPixelFilter == "windowed_sinc_bartlett" ) {
				RISE_API_CreateBartlettWindowedSincPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
			} else if( sPixelFilter == "windowed_sinc_welch" ) {
				RISE_API_CreateWelchWindowedSincPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
			} else if( sPixelFilter == "windowed_sinc_hanning" ) {
				RISE_API_CreateHanningWindowedSincPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
			} else if( sPixelFilter == "windowed_sinc_hamming" ) {
				RISE_API_CreateHammingWindowedSincPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
			} else if( sPixelFilter == "windowed_sinc_blackman" ) {
				RISE_API_CreateBlackmanWindowedSincPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
			} else if( sPixelFilter == "windowed_sinc_lanczos" ) {
				RISE_API_CreateLanczosWindowedSincPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
			} else if( sPixelFilter == "windowed_sinc_kaiser" ) {
				RISE_API_CreateKaiserWindowedSincPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight, pixelFilterParamA );
			} else if( sPixelFilter == "cook" ) {
				RISE_API_CreateCookPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
			} else if( sPixelFilter == "max" ) {
				RISE_API_CreateMaxPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight, pixelFilterParamA, pixelFilterParamB );
			} else if( sPixelFilter == "mitchell-netravali" ) {
				RISE_API_CreateMitchellNetravaliPixelFilter( pPixelFilter, pixelFilterParamA, pixelFilterParamB );
			} else if( sPixelFilter == "lanczos" ) {
				RISE_API_CreateLanczosPixelFilter( pPixelFilter );
			} else if( sPixelFilter == "catmull-rom" ) {
				RISE_API_CreateCatmullRomPixelFilter( pPixelFilter );
			} else if( sPixelFilter == "cubic_bspline" ) {
				RISE_API_CreateCubicBSplinePixelFilter( pPixelFilter );
			} else if( sPixelFilter == "quadratic_bspline" ) {
				RISE_API_CreateQuadraticBSplinePixelFilter( pPixelFilter );
			} else {
				GlobalLog()->PrintEx( eLog_Error, "Unknown filter type: `%s`", pixelFilter );
			}
		} else {
			RISE_API_CreateBoxPixelFilter( pPixelFilter, 1.0, 1.0 );
		}
	}

	if( numLumSamples > 1 ) {
		if( luminarySampler ) {
			String sLuminarySampler( luminarySampler );
			if( sLuminarySampler == "nrooks" ) {
				RISE_API_CreateNRooksSampling2D( pLumSampler, 1.0, 1.0, luminarySamplerParam );
			} else if( sLuminarySampler == "uniform" ) {
				RISE_API_CreateUniformSampling2D( pLumSampler, 1.0, 1.0 );
			} else if( sLuminarySampler == "random" ) {
				RISE_API_CreateRandomSampling2D( pLumSampler, 1.0, 1.0 );
			} else if( sLuminarySampler == "stratified" ) {
				RISE_API_CreateStratifiedSampling2D( pLumSampler, 1.0, 1.0, luminarySamplerParam );
			} else if( sLuminarySampler == "poisson" ) {
				RISE_API_CreatePoissonDiskSampling2D( pLumSampler, 1.0, 1.0, luminarySamplerParam );
			} else if( sLuminarySampler == "multijittered" ) {
				RISE_API_CreateMultiJitteredSampling2D( pLumSampler, 1.0, 1.0 );
			} else if( sLuminarySampler == "halton" ) {
				RISE_API_CreateHaltonPointsSampling2D( pLumSampler, 1.0, 1.0 );
			} else {
				GlobalLog()->PrintEx( eLog_Error, "Unknown sampler type: `%s`", pixelSampler );
				return false;
			}
		} else {
			RISE_API_CreateMultiJitteredSampling2D( pLumSampler, 1.0, 1.0 );
		}

		(*pLumSampler)->SetNumSamples( numLumSamples );
	}

	return true;
}

//! Sets the rasterizer type to be pixel based PEL
bool Job::SetPixelBasedPelRasterizer(
	const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
	const unsigned int numLumSamples,						///< [in] Number of samples / luminaire
	const unsigned int maxRecur,							///< [in] Maximum recursion level
	const double minImportance,								///< [in] Minimum importance to stop at
	const char* shader,										///< [in] The default shader
	const char* globalRadianceMap,							///< [in] Name of the painter for global IBL
	const bool bBackground,									///< [in] Is the radiance map a background object
	const double scale,										///< [in] How much to scale the radiance values
	const double orient[3],									///< [in] Euler angles for orienting the radiance map
	const char* pixelSampler,								///< [in] Type of sampling to use for the pixel sampler
	const double pixelSamplerParam,							///< [in] Parameter for the pixel sampler
	const char* luminarySampler,							///< [in] Type of sampling to use for luminaries
	const double luminarySamplerParam,						///< [in] Parameter for the luminary sampler
	const char* pixelFilter,								///< [in] Type of filtering to use for the pixels
	const double pixelFilterWidth,							///< [in] How wide is the pixel filter?
	const double pixelFilterHeight,							///< [in] How high is the pixel filter?
	const double pixelFilterParamA,							///< [in] Pixel filter parameter A
	const double pixelFilterParamB,							///< [in] Pixel filter parameter B
	const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
	const bool bUseIORStack,								///< [in] Should we use an index of refraction stack?
	const bool bChooseOnlyOneLight							///< [in] For the luminaire sampler only one random light is chosen for each sample
	)
{
	ISampling2D* pPixelSampler = 0;
	ISampling2D* pLumSampler = 0;
	IPixelFilter* pPixelFilter = 0;

	if( !GetSamplingAndFilterElements( &pPixelSampler, &pLumSampler, &pPixelFilter, numPixelSamples, numLumSamples,
		pixelSampler, pixelSamplerParam, luminarySampler, luminarySamplerParam, pixelFilter, pixelFilterWidth, pixelFilterHeight, pixelFilterParamA, pixelFilterParamB ) )
	{
		return false;
	}

	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEasyError( "Job::SetPixelBasedRasterizer:: Default shader not found" );
		return false;
	}

	IRayCaster* pCaster = 0;
	RISE_API_CreateRayCaster( &pCaster, bBackground, maxRecur, minImportance, *pShader, bShowLuminaires, bUseIORStack, bChooseOnlyOneLight );

	if( globalRadianceMap ) {
		IPainter* p = pPntManager->GetItem( globalRadianceMap );

		if( p ) {
			IRadianceMap* pRm = 0;
			RISE_API_CreateRadianceMap( &pRm, *p, scale );
			pRm->SetOrientation( Vector3( orient ) );

			pScene->SetGlobalRadianceMap( pRm );
			safe_release( pRm );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::SetPixelBasedPelRasterizer:: Global Radiance Map painter not found \'%s\'", p );
		}
	}

	if( pLumSampler ) {
		pCaster->SetLuminaireSampling( pLumSampler );
	}

	IRasterizer* pRaster = 0;
	RISE_API_CreatePixelBasedPelRasterizer( &pRaster, pCaster, pPixelSampler, pPixelFilter );

	safe_release( pPixelSampler );
	safe_release( pLumSampler );
	safe_release( pPixelFilter );
	safe_release( pCaster );
	safe_release( pRasterizer );

	pRasterizer = pRaster;

	return true;
}

//! Sets the rasterizer type to be pixel based spectral integrating
bool Job::SetPixelBasedSpectralIntegratingRasterizer(
	const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
	const unsigned int numLumSamples,						///< [in] Number of samples / luminaire
	const unsigned int specSamples,							///< [in] Number of spectral samples / pixel
	const double lambda_begin,								///< [in] Wavelength to start sampling at
	const double lambda_end,								///< [in] Wavelength to finish sampling at
	const unsigned int num_wavelengths,						///< [in] Number of wavelengths to sample
	const unsigned int maxRecur,							///< [in] Maximum recursion level
	const double minImportance,								///< [in] Minimum importance to stop at
	const char* shader,										///< [in] The default shader
	const char* globalRadianceMap,							///< [in] Name of the painter for global IBL
	const bool bBackground,									///< [in] Is the radiance map a background object
	const double scale,										///< [in] How much to scale the radiance values
	const double orient[3],									///< [in] Euler angles for orienting the radiance map
	const char* pixelSampler,								///< [in] Type of sampling to use for the pixel sampler
	const double pixelSamplerParam,							///< [in] Parameter for the pixel sampler
	const char* luminarySampler,							///< [in] Type of sampling to use for luminaries
	const double luminarySamplerParam,						///< [in] Parameter for the luminary sampler
	const char* pixelFilter,								///< [in] Type of filtering to use for the pixels
	const double pixelFilterWidth,							///< [in] How wide is the pixel filter?
	const double pixelFilterHeight,							///< [in] How high is the pixel filter?
	const double pixelFilterParamA,							///< [in] Pixel filter parameter A
	const double pixelFilterParamB,							///< [in] Pixel filter parameter B
	const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
	const bool bUseIORStack,								///< [in] Should we use an index of refraction stack?
	const bool bChooseOnlyOneLight,							///< [in] For the luminaire sampler only one random light is chosen for each sample
	const bool bIntegrateRGB,								///< [in] Should we use the CIE XYZ spd functions or will they be specified now?
	const unsigned int numSPDvalues,						///< [in] Number of values in the RGB SPD arrays
	const double rgb_spd_frequencies[],						///< [in] Array that contains the RGB SPD frequencies
	const double rgb_spd_r[],								///< [in] Array that contains the RGB SPD amplitudes for red
	const double rgb_spd_g[],								///< [in] Array that contains the RGB SPD amplitudes for green
	const double rgb_spd_b[]								///< [in] Array that contains the RGB SPD amplitudes for blue
	)
{
	ISampling2D* pPixelSampler = 0;
	ISampling2D* pLumSampler = 0;
	IPixelFilter* pPixelFilter = 0;

	if( !GetSamplingAndFilterElements( &pPixelSampler, &pLumSampler, &pPixelFilter, numPixelSamples, numLumSamples,
		pixelSampler, pixelSamplerParam, luminarySampler, luminarySamplerParam, pixelFilter, pixelFilterWidth, pixelFilterHeight, pixelFilterParamA, pixelFilterParamB ) )
	{
		return false;
	}

	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEasyInfo( "Job::SetPixelBasedRasterizer:: Specified shader not found" );
		return false;
	}

	IRayCaster* pCaster = 0;
	RISE_API_CreateRayCaster( &pCaster, bBackground, maxRecur, minImportance, *pShader, bShowLuminaires, bUseIORStack, bChooseOnlyOneLight );

	if( globalRadianceMap ) {
		IPainter* p = pPntManager->GetItem( globalRadianceMap );

		if( p ) {
			IRadianceMap* pRm = 0;
			RISE_API_CreateRadianceMap( &pRm, *p, scale );
			pRm->SetOrientation( Vector3( orient ) );

			pScene->SetGlobalRadianceMap( pRm );
			safe_release( pRm );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::SetPixelBasedSpectralIntegratingRasterizer:: Global Radiance Map painter not found \'%s\'", p );
		}
	}

	if( pLumSampler ) {
		pCaster->SetLuminaireSampling( pLumSampler );
	}

	IRasterizer* pRaster = 0;

	if( bIntegrateRGB ) {

		GlobalLog()->PrintEasyError( "Job::SetPixelBasedSpectralIntegratingRasterizer:: Custom RGB curve integration is no longer supported" );

		/*
		if( rgb_spd_frequencies && rgb_spd_r && rgb_spd_g && rgb_spd_b ) {
			IPiecewiseFunction1D* red = 0;
			IPiecewiseFunction1D* green = 0;
			IPiecewiseFunction1D* blue = 0;

			RISE_API_CreatePiecewiseLinearFunction1D( &red );
			RISE_API_CreatePiecewiseLinearFunction1D( &green );
			RISE_API_CreatePiecewiseLinearFunction1D( &blue );

			red->addControlPoints( numSPDvalues, rgb_spd_frequencies, rgb_spd_r );
			green->addControlPoints( numSPDvalues, rgb_spd_frequencies, rgb_spd_g );
			blue->addControlPoints( numSPDvalues, rgb_spd_frequencies, rgb_spd_b );

			RISE_API_CreatePixelBasedSpectralIntegratingRasterizerRGB( &pRaster, pCaster, pPixelSampler, pPixelFilter, specSamples, lambda_begin, lambda_end, num_wavelengths, *red, *green, *blue );

			safe_release( red );
			safe_release( green );
			safe_release( blue );
		} else {
			GlobalLog()->PrintEasyWarning( "Job::SetPixelBasedSpectralIntegratingRasterizer:: Asked to integrate RGB but didn't properly specify SPD tables" );
		}
		*/
	} else {
		RISE_API_CreatePixelBasedSpectralIntegratingRasterizer( &pRaster, pCaster, pPixelSampler, pPixelFilter, specSamples, lambda_begin, lambda_end, num_wavelengths );
	}

	safe_release( pPixelSampler );
	safe_release( pLumSampler );
	safe_release( pPixelFilter );
	safe_release( pCaster );
	safe_release( pRasterizer );

	pRasterizer = pRaster;

	return true;
}

//! Sets the rasterizer type to be adaptive pixel based PEL
bool Job::SetAdaptivePixelBasedPelRasterizer(
	const unsigned int numMinPixelSamples,					///< [in] Minimum or base number of samples to start with
	const unsigned int numMaxPixelSamples,					///< [in] Maximum number of samples to go to
	const unsigned int numSteps,							///< [in] Number of steps to maximum sampling level
	const unsigned int numLumSamples,						///< [in] Number of samples / luminaire
	const double threshold,									///< [in] Threshold at which to stop sampling further
	const bool bOutputSamples,								///< [in] Should the renderer show how many samples rather than an image
	const unsigned int maxRecur,							///< [in] Maximum recursion level
	const double minImportance,								///< [in] Minimum importance to stop at
	const char* shader,										///< [in] The default shader
	const char* globalRadianceMap,							///< [in] Name of the painter for global IBL
	const bool bBackground,									///< [in] Is the radiance map a background object
	const double scale,										///< [in] How much to scale the radiance values
	const double orient[3],									///< [in] Euler angles for orienting the radiance map
	const char* pixelSampler,								///< [in] Type of sampling to use for the pixel sampler
	const double pixelSamplerParam,							///< [in] Parameter for the pixel sampler
	const char* luminarySampler,							///< [in] Type of sampling to use for luminaries
	const double luminarySamplerParam,						///< [in] Parameter for the luminary sampler
	const char* pixelFilter,								///< [in] Type of filtering to use for the pixels
	const double pixelFilterWidth,							///< [in] How wide is the pixel filter?
	const double pixelFilterHeight,							///< [in] How high is the pixel filter?
	const double pixelFilterParamA,							///< [in] Pixel filter parameter A
	const double pixelFilterParamB,							///< [in] Pixel filter parameter B
	const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
	const bool bUseIORStack,								///< [in] Should we use an index of refraction stack?
	const bool bChooseOnlyOneLight							///< [in] For the luminaire sampler only one random light is chosen for each sample
	)
{
	ISampling2D* pPixelSampler = 0;
	ISampling2D* pLumSampler = 0;
	IPixelFilter* pPixelFilter = 0;

	if( numMinPixelSamples <= 1 ) {
		GlobalLog()->PrintEasyError( "Job::SetAdaptivePixelBasedPelRasterizer:: Select more than one sample!" );
		return false;
	}

	if( !GetSamplingAndFilterElements( &pPixelSampler, &pLumSampler, &pPixelFilter, numMinPixelSamples, numLumSamples,
		pixelSampler, pixelSamplerParam, luminarySampler, luminarySamplerParam, pixelFilter, pixelFilterWidth, pixelFilterHeight, pixelFilterParamA, pixelFilterParamB ) )
	{
		return false;
	}

	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEasyInfo( "Job::SetPixelBasedRasterizer:: Specified shader not found" );
		return false;
	}

	IRayCaster* pCaster = 0;
	RISE_API_CreateRayCaster( &pCaster, bBackground, maxRecur, minImportance, *pShader, bShowLuminaires, bUseIORStack, bChooseOnlyOneLight );

	if( globalRadianceMap ) {
		IPainter* p = pPntManager->GetItem( globalRadianceMap );

		if( p ) {
			IRadianceMap* pRm = 0;
			RISE_API_CreateRadianceMap( &pRm, *p, scale );
			pRm->SetOrientation( Vector3( orient ) );

			pScene->SetGlobalRadianceMap( pRm );
			safe_release( pRm );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::SetPixelBasedPelRasterizer:: Global Radiance Map painter not found \'%s\'", p );
		}
	}

	if( pLumSampler ) {
		pCaster->SetLuminaireSampling( pLumSampler );
	}

	IRasterizer* pRaster = 0;
	RISE_API_CreateAdaptiveSamplingPixelBasedPelRasterizer(
		&pRaster, pCaster, pPixelSampler, pPixelFilter, numMaxPixelSamples, threshold, numSteps, bOutputSamples );

	safe_release( pPixelSampler );
	safe_release( pLumSampler );
	safe_release( pPixelFilter );
	safe_release( pCaster );
	safe_release( pRasterizer );

	pRasterizer = pRaster;

	return true;
}

//! Sets the rasterizer type to be contrast AA pixel pel
bool Job::SetContrastAAPixelBasedPelRasterizer(
	const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
	const unsigned int numLumSamples,						///< [in] Number of samples / luminaire
	const unsigned int maxRecur,							///< [in] Maximum recursion level
	const double minImportance,								///< [in] Minimum importance to stop at
	const char* shader,										///< [in] The default shader
	const char* globalRadianceMap,							///< [in] Name of the painter for global IBL
	const bool bBackground,									///< [in] Is the radiance map a background object
	const double scale,										///< [in] How much to scale the radiance values
	const double orient[3],									///< [in] Euler angles for orienting the radiance map
	const char* pixelSampler,								///< [in] Type of sampling to use for the pixel sampler
	const double pixelSamplerParam,							///< [in] Parameter for the pixel sampler
	const char* luminarySampler,							///< [in] Type of sampling to use for luminaries
	const double luminarySamplerParam,						///< [in] Parameter for the luminary sampler
	const char* pixelFilter,								///< [in] Type of filtering to use for the pixels
	const double pixelFilterWidth,							///< [in] How wide is the pixel filter?
	const double pixelFilterHeight,							///< [in] How high is the pixel filter?
	const double pixelFilterParamA,							///< [in] Pixel filter parameter A
	const double pixelFilterParamB,							///< [in] Pixel filter parameter B
	const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
	const bool bUseIORStack,								///< [in] Should we use an index of refraction stack?
	const bool bChooseOnlyOneLight,							///< [in] For the luminaire sampler only one random light is chosen for each sample
	const double contrast_threshold[3],						///< [in] Contrast threshold for each color component
	const bool show_samples									///< [in] Should the number of samples be taken be shown?
	)
{
	GlobalLog()->PrintEasyWarning( "Job::SetContrastAAPixelBasedPelRasterizer:: This rasterizer is EXPERIMENTAL and not meant for actual use.  In fact it really doesn't work well at all.  You really should use it" );

	ISampling2D* pPixelSampler = 0;
	ISampling2D* pLumSampler = 0;
	IPixelFilter* pPixelFilter = 0;

	if( !GetSamplingAndFilterElements( &pPixelSampler, &pLumSampler, &pPixelFilter, numPixelSamples, numLumSamples,
		pixelSampler, pixelSamplerParam, luminarySampler, luminarySamplerParam, pixelFilter, pixelFilterWidth, pixelFilterHeight, pixelFilterParamA, pixelFilterParamB ) )
	{
		return false;
	}

	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEasyError( "Job::SetContrastAAPixelBasedPelRasterizer:: Default shader not found" );
		return false;
	}

	IRayCaster* pCaster = 0;
	RISE_API_CreateRayCaster( &pCaster, bBackground, maxRecur, minImportance, *pShader, bShowLuminaires, bUseIORStack, bChooseOnlyOneLight );

	if( globalRadianceMap ) {
		IPainter* p = pPntManager->GetItem( globalRadianceMap );

		if( p ) {
			IRadianceMap* pRm = 0;
			RISE_API_CreateRadianceMap( &pRm, *p, scale );
			pRm->SetOrientation( Vector3( orient ) );

			pScene->SetGlobalRadianceMap( pRm );
			safe_release( pRm );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::SetContrastAAPixelBasedPelRasterizer:: Global Radiance Map painter not found \'%s\'", p );
		}
	}

	if( pLumSampler ) {
		pCaster->SetLuminaireSampling( pLumSampler );
	}

	IRasterizer* pRaster = 0;
	RISE_API_CreatePixelBasedPelRasterizerContrastAA( &pRaster, pCaster, pPixelSampler, pPixelFilter, RISEPel(contrast_threshold), show_samples );

	safe_release( pPixelSampler );
	safe_release( pLumSampler );
	safe_release( pPixelFilter );
	safe_release( pCaster );
	safe_release( pRasterizer );

	pRasterizer = pRaster;

	return true;
}


//
// Adds raster outputs
//

//! Creates a file rasterizer output
//! This should be called after a rasterizer has been set
//! Note that setting a new rasterizer after adding file rasterizer outputs will
//! delete existing outputs
/// \return TRUE if successful, FALSE otherwise
bool Job::AddFileRasterizerOutput(
	const char* szPattern,									///< [in] File pattern
	const bool bMultiple,									///< [in] Output multiple files (for animations usually)
	const char type,										///< [in] Type of file
															///		0 - TGA
															///		1 - PPM
															///		2 - PNG
															///		3 - HDR
															///     4 - TIFF
															///		5 - RGBEA
	const unsigned char bpp,								///< [in] Bits / pixel for the file
	const char color_space									///< [in] Color space to apply
															///		0 - Rec709 RGB linear
															///		1 - sRGB profile
															///		2 - ROMM RGB (ProPhotoRGB) linear
															///		3 - ROMM RGB (ProPhotoRGB) non-linear
	)
{
	if( !pRasterizer ) {
		return false;
	}

	COLOR_SPACE gc = eColorSpace_sRGB;
	switch( color_space )
	{
	case 1:
		gc = eColorSpace_sRGB;
		break;
	case 0:
		gc = eColorSpace_Rec709RGB_Linear;
		break;
	case 2:
		gc = eColorSpace_ROMMRGB_Linear;
		break;
	case 3:
		gc = eColorSpace_ProPhotoRGB;
		break;
	};

	IRasterizerOutput* ro = 0;
	RISE_API_CreateFileRasterizerOutput( &ro, szPattern, bMultiple, type, bpp, gc );

	pRasterizer->AddRasterizerOutput( ro );
	safe_release( ro );

	return true;
}

namespace RISE {
	namespace Implementation {
		//! This is out dispatcher that handles the rasterizer output calls and pipes them off
		//! to the client application
		class CallbackRasterizerOutputDispatch : public virtual IRasterizerOutput, public virtual Reference
		{
		protected:
			IJobRasterizerOutput&	pObj;
			RGBA16*					pBuffer;
			unsigned int			width;
			unsigned int			height;
			bool					bPremultipliedAlpha;
			COLOR_SPACE				color_space;

			virtual ~CallbackRasterizerOutputDispatch()
			{
				if( pBuffer ) {
					GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
					delete pBuffer;
					pBuffer = 0;
				}
			}

		public:

			CallbackRasterizerOutputDispatch( const CallbackRasterizerOutputDispatch& o ) :
			  pObj( o.pObj ), pBuffer( 0 ), width( o.width ), height( o.height ), bPremultipliedAlpha( o.bPremultipliedAlpha ), color_space( o.color_space )
			{
			}

			CallbackRasterizerOutputDispatch( IJobRasterizerOutput& pObj_ ) :
			  pObj( pObj_ ), pBuffer( 0 ), width( 0 ), height( 0 ), bPremultipliedAlpha( false ), color_space( eColorSpace_sRGB )
			{
			}

			//! Outputs an intermediate scanline of rasterized data
			void OutputIntermediateImage(
				const IRasterImage& pImage,					///< [in] Rasterized image
				const Rect* pRegion							///< [in] Rasterized region, if its NULL then the entire image should be output
				)
			{
				if( !pBuffer ) {
					width = pImage.GetWidth();
					height = pImage.GetHeight();
					pBuffer = new RGBA16[ width*height ];
					GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );
					bPremultipliedAlpha = pObj.PremultipliedAlpha();
					int cp = pObj.GetColorSpace();
					switch( cp )
					{
					case 0:
						color_space = eColorSpace_Rec709RGB_Linear;
						break;
					case 1:
					default:
						color_space = eColorSpace_sRGB;
						break;
					case 2:
						color_space = eColorSpace_ROMMRGB_Linear;
						break;
					case 3:
						color_space = eColorSpace_ProPhotoRGB;
						break;
					};
				}

				// Set the appropriate bytes
				Rect rc( 0, 0, height-1, width-1 );
				if( pRegion ) {
					rc.left = pRegion->left;
					rc.top = pRegion->top;
					rc.right = pRegion->right;
					rc.bottom = pRegion->bottom;
				}

				for( unsigned int y=rc.top; y<=rc.bottom; y++ ) {
					for( unsigned int x=rc.left; x<= rc.right; x++ ) {

						RGBA16 conv;

						switch( color_space )
						{
						case eColorSpace_sRGB:
							conv = pImage.GetPEL(x,y).Integerize<sRGBPel,unsigned short>( 65535.0 );
							break;
						case eColorSpace_Rec709RGB_Linear:
							conv = pImage.GetPEL(x,y).Integerize<Rec709RGBPel,unsigned short>( 65535.0 );
							break;
						case eColorSpace_ROMMRGB_Linear:
							conv = pImage.GetPEL(x,y).Integerize<ROMMRGBPel,unsigned short>( 65535.0 );
							break;
						case eColorSpace_ProPhotoRGB:
							conv = pImage.GetPEL(x,y).Integerize<ProPhotoRGBPel,unsigned short>( 65535.0 );
							break;
						};

						if( bPremultipliedAlpha ) {
							pBuffer[y*width+x] = ColorUtils::PremultiplyAlphaRGB<RGBA16,0xFFFF>( conv );
						} else {
							pBuffer[y*width+x] = conv;
						}
					}
				}

				// Send the data off
				pObj.OutputImageRGBA16(	(unsigned short*)pBuffer, width, height, rc.top, rc.left, rc.bottom, rc.right );
			}

			//! A full rasterization was complete, and the full image should be output
			void OutputImage(
				const IRasterImage& pImage,					///< [in] Rasterized image
				const Rect* pRegion,						///< [in] Rasterized region, if its NULL then the entire image should be output
				const unsigned int frame
				)
			{
				OutputIntermediateImage( pImage, pRegion );
			}
		};
	}
}

//! Creates a user callback rasterizer output
//! This should be called after a rasterizer has been set
//! Note that no attemps at reference counting are made, the user
//! better not go delete the object
bool Job::AddCallbackRasterizerOutput(
	IJobRasterizerOutput* pObj
	)
{
	if( !pObj || !pRasterizer ) {
		return false;
	}

	IRasterizerOutput* pRo = new CallbackRasterizerOutputDispatch( *pObj );
	GlobalLog()->PrintNew( pRo, __FILE__, __LINE__, "callback rasterizer output dispatch" );

	pRasterizer->AddRasterizerOutput( pRo );
	safe_release( pRo );

	return true;
}

//
// Photon mapping
//

//! Sets the gather parameters for the caustic pel photon map
/// \return TRUE if successful, FALSE otherwise
bool Job::SetCausticPelGatherParameters(
	const double radius,							///< [in] Search radius
	const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
	const unsigned int min,							///< [in] Maximum number of photons to store
	const unsigned int max							///< [in] Total number of photons to shoot
	)
{
	IPhotonMap* pMap = pScene->GetCausticPelMap();

	if( !pMap ) {
		return false;
	}

	pMap->SetGatherParams( radius, ellipse_ratio, min, max, pGlobalProgress );
	return true;
}

//! Sets the gather parameters for the global pel photon map
/// \return TRUE if successful, FALSE otherwise
bool Job::SetGlobalPelGatherParameters(
	const double radius,							///< [in] Search radius
	const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
	const unsigned int min,							///< [in] Maximum number of photons to store
	const unsigned int max							///< [in] Total number of photons to shoot
	)
{
	IPhotonMap* pMap = pScene->GetGlobalPelMap();

	if( !pMap ) {
		return false;
	}

	pMap->SetGatherParams( radius, ellipse_ratio, min, max, pGlobalProgress );
	return true;
}

//! Sets the gather parameters for the translucent pel photon map
/// \return TRUE if successful, FALSE otherwise
bool Job::SetTranslucentPelGatherParameters(
	const double radius,							///< [in] Search radius
	const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
	const unsigned int min,							///< [in] Maximum number of photons to store
	const unsigned int max							///< [in] Total number of photons to shoot
	)
{
	IPhotonMap* pMap = pScene->GetTranslucentPelMap();

	if( !pMap ) {
		return false;
	}

	pMap->SetGatherParams( radius, ellipse_ratio, min, max, pGlobalProgress );
	return true;
}

//! Sets the gather parameters for the caustic spectral photon map
/// \return TRUE if successful, FALSE otherwise
bool Job::SetCausticSpectralGatherParameters(
	const double radius,							///< [in] Search radius
	const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
	const unsigned int min,							///< [in] Maximum number of photons to store
	const unsigned int max,							///< [in] Total number of photons to shoot
	const double nm_range							///< [in] Range of wavelengths to search for a NM irradiance estimate
	)
{
	ISpectralPhotonMap* pMap = pScene->GetCausticSpectralMap();

	if( !pMap ) {
		return false;
	}

	pMap->SetGatherParamsNM( radius, ellipse_ratio, min, max, nm_range, pGlobalProgress );
	return true;
}

//! Sets the gather parameters for the global spectral photon map
/// \return TRUE if successful, FALSE otherwise
bool Job::SetGlobalSpectralGatherParameters(
	const double radius,							///< [in] Search radius
	const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
	const unsigned int min,							///< [in] Maximum number of photons to store
	const unsigned int max,							///< [in] Total number of photons to shoot
	const double nm_range							///< [in] Range of wavelengths to search for a NM irradiance estimate
	)
{
	ISpectralPhotonMap* pMap = pScene->GetGlobalSpectralMap();

	if( !pMap ) {
		return false;
	}

	pMap->SetGatherParamsNM( radius, ellipse_ratio, min, max, nm_range, pGlobalProgress );
	return true;
}

//! Sets the gather parameters for the shadow photon map
/// \return TRUE if successful, FALSE otherwise
bool Job::SetShadowGatherParameters(
	const double radius,							///< [in] Search radius
	const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
	const unsigned int min,							///< [in] Maximum number of photons to store
	const unsigned int max							///< [in] Total number of photons to shoot
	)
{
	IShadowPhotonMap* pMap = pScene->GetShadowMap();

	if( !pMap ) {
		return false;
	}

	pMap->SetGatherParams( radius, ellipse_ratio, min, max, pGlobalProgress );
	return true;
}

//! Sets the irradiance cache parameters
/// \return TRUE if successful, FALSE otherwise
bool Job::SetIrradianceCacheParameters(
	const unsigned int size,			///< [in] Size of the cache
	const double tolerance,				///< [in] Tolerance of the cache
	const double min_spacing,			///< [in] Minimum seperation
	const double max_spacing			///< [in] Maximum seperation
	)
{
	IIrradianceCache* pCache = 0;
	RISE_API_CreateIrradianceCache( &pCache, size, tolerance, min_spacing, max_spacing );

    pScene->SetIrradianceCache( pCache );

	safe_release( pCache );
	return true;
}

//! Saves the caustic pel photon map to disk
bool Job::SaveCausticPelPhotonmap(
	const char* file_name							///< [in] Name of the file to save it to
	)
{
	if( !file_name ) {
		return false;
	}

	IWriteBuffer* buffer = 0;
	RISE_API_CreateDiskFileWriteBuffer( &buffer, file_name );

	IPhotonMap* pMap = pScene->GetCausticPelMap();

	if( !pMap ) {
		GlobalLog()->PrintEasyError( "Scene doesn't contain a caustic pel photon map" );
		return false;
	}

	pMap->Serialize( *buffer );

	safe_release( buffer );
	return true;
}

//! Saves the global pel photon map to disk
bool Job::SaveGlobalPelPhotonmap(
	const char* file_name							///< [in] Name of the file to save it to
	)
{
	if( !file_name ) {
		return false;
	}

	IWriteBuffer* buffer = 0;
	RISE_API_CreateDiskFileWriteBuffer( &buffer, file_name );

	IPhotonMap* pMap = pScene->GetGlobalPelMap();

	if( !pMap ) {
		GlobalLog()->PrintEasyError( "Scene doesn't contain a global pel photon map" );
		return false;
	}

	pMap->Serialize( *buffer );

	safe_release( buffer );
	return true;
}

//! Saves the translucent pel photon map to disk
bool Job::SaveTranslucentPelPhotonmap(
	const char* file_name							///< [in] Name of the file to save it to
	)
{
	if( !file_name ) {
		return false;
	}

	IWriteBuffer* buffer = 0;
	RISE_API_CreateDiskFileWriteBuffer( &buffer, file_name );

	IPhotonMap* pMap = pScene->GetTranslucentPelMap();

	if( !pMap ) {
		GlobalLog()->PrintEasyError( "Scene doesn't contain a translucent pel photon map" );
		return false;
	}

	pMap->Serialize( *buffer );

	safe_release( buffer );
	return true;
}

//! Saves the caustic spectral photon map to disk
bool Job::SaveCausticSpectralPhotonmap(
	const char* file_name							///< [in] Name of the file to save it to
	)
{
	if( !file_name ) {
		return false;
	}

	IWriteBuffer* buffer = 0;
	RISE_API_CreateDiskFileWriteBuffer( &buffer, file_name );

	IPhotonMap* pMap = pScene->GetCausticSpectralMap();

	if( !pMap ) {
		GlobalLog()->PrintEasyError( "Scene doesn't contain a caustic spectral photon map" );
		return false;
	}

	pMap->Serialize( *buffer );

	safe_release( buffer );
	return true;
}

//! Saves the global spectral photon map to disk
bool Job::SaveGlobalSpectralPhotonmap(
	const char* file_name							///< [in] Name of the file to save it to
	)
{
	if( !file_name ) {
		return false;
	}

	IWriteBuffer* buffer = 0;
	RISE_API_CreateDiskFileWriteBuffer( &buffer, file_name );

	IPhotonMap* pMap = pScene->GetGlobalSpectralMap();

	if( !pMap ) {
		GlobalLog()->PrintEasyError( "Scene doesn't contain a global spectral photon map" );
		return false;
	}

	pMap->Serialize( *buffer );

	safe_release( buffer );
	return true;
}

//! Loads the shadow photon map to disk
bool Job::SaveShadowPhotonmap(
	const char* file_name							///< [in] Name of the file to save it to
	)
{
	if( !file_name ) {
		return false;
	}

	IWriteBuffer* buffer = 0;
	RISE_API_CreateDiskFileWriteBuffer( &buffer, file_name );

	IShadowPhotonMap* pMap = pScene->GetShadowMap();

	if( !pMap ) {
		GlobalLog()->PrintEasyError( "Scene doesn't contain a shadow photon map" );
		return false;
	}

	pMap->Serialize( *buffer );

	safe_release( buffer );
	return true;
}


//! Loads the caustic pel photon map from disk
bool Job::LoadCausticPelPhotonmap(
	const char* file_name							///< [in] Name of the file to load from
	)
{
	if( !file_name ) {
		return false;
	}

	IReadBuffer* buffer = 0;
	RISE_API_CreateDiskFileReadBuffer( &buffer, file_name );

	IPhotonMap* pmap = 0;
	RISE_API_CreateCausticPelPhotonMap( &pmap, 0 );

	pmap->Deserialize( *buffer );
	pScene->SetCausticPelMap( pmap );

	safe_release( pmap );
	safe_release( buffer );
	return true;
}

//! Loads the global pel photon map from disk
bool Job::LoadGlobalPelPhotonmap(
	const char* file_name							///< [in] Name of the file to load from
	)
{
	if( !file_name ) {
		return false;
	}

	IReadBuffer* buffer = 0;
	RISE_API_CreateDiskFileReadBuffer( &buffer, file_name );

	IPhotonMap* pmap = 0;
	RISE_API_CreateGlobalPelPhotonMap( &pmap, 0 );

	pmap->Deserialize( *buffer );
	pScene->SetGlobalPelMap( pmap );

	safe_release( pmap );
	safe_release( buffer );
	return true;
}

//! Loads the translucent pel photon map from disk
bool Job::LoadTranslucentPelPhotonmap(
	const char* file_name							///< [in] Name of the file to load from
	)
{
	if( !file_name ) {
		return false;
	}

	IReadBuffer* buffer = 0;
	RISE_API_CreateDiskFileReadBuffer( &buffer, file_name );

	IPhotonMap* pmap = 0;
	RISE_API_CreateTranslucentPelPhotonMap( &pmap, 0 );

	pmap->Deserialize( *buffer );
	pScene->SetTranslucentPelMap( pmap );

	safe_release( pmap );
	safe_release( buffer );
	return true;
}

//! Loads the caustic spectral photon map from disk
bool Job::LoadCausticSpectralPhotonmap(
	const char* file_name							///< [in] Name of the file to load from
	)
{
	if( !file_name ) {
		return false;
	}


	IReadBuffer* buffer = 0;
	RISE_API_CreateDiskFileReadBuffer( &buffer, file_name );

	ISpectralPhotonMap* pmap = 0;
	RISE_API_CreateCausticSpectralPhotonMap( &pmap, 0 );

	pmap->Deserialize( *buffer );
	pScene->SetCausticSpectralMap( pmap );

	safe_release( pmap );
	safe_release( buffer );
	return true;
}

//! Loads the caustic spectral photon map from disk
bool Job::LoadGlobalSpectralPhotonmap(
	const char* file_name							///< [in] Name of the file to load from
	)
{
	if( !file_name ) {
		return false;
	}


	IReadBuffer* buffer = 0;
	RISE_API_CreateDiskFileReadBuffer( &buffer, file_name );

	ISpectralPhotonMap* pmap = 0;
	RISE_API_CreateGlobalSpectralPhotonMap( &pmap, 0 );

	pmap->Deserialize( *buffer );
	pScene->SetGlobalSpectralMap( pmap );

	safe_release( pmap );
	safe_release( buffer );
	return true;
}

//! Loads the shadow photon map from disk
bool Job::LoadShadowPhotonmap(
	const char* file_name							///< [in] Name of the file to load from
	)
{
	if( !file_name ) {
		return false;
	}

	IReadBuffer* buffer = 0;
	RISE_API_CreateDiskFileReadBuffer( &buffer, file_name );

	IShadowPhotonMap* pmap = 0;
	RISE_API_CreateShadowPhotonMap( &pmap, 0 );

	pmap->Deserialize( *buffer );
	pScene->SetShadowMap( pmap );

	safe_release( pmap );
	safe_release( buffer );
	return true;
}


//
// Commands
//

//! Shoots caustic photons and populates the caustic pel photon map
/// \return TRUE if successful, FALSE otherwise
bool Job::ShootCausticPelPhotons(
	const unsigned int num,							///< [in] Number of photons to acquire
	const double power_scale,						///< [in] How much to scale light power by
	const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
	const double minImportance,						///< [in] Minimum importance when a photon is discarded
	const bool branch,								///< [in] Should the tracer branch or follow a single path?
	const bool reflect,								///< [in] Should we trace reflected rays?
	const bool refract,								///< [in] Should we trace refracted rays?
	const bool nonmeshlights,						///< [in] Should we shoot from non mesh based lights?
	const bool useiorstack,							///< [in] Should the ray caster use a index of refraction stack?
	const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
	const bool regenerate							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
	)
{
	IPhotonTracer* pTracer = 0;
	RISE_API_CreateCausticPelPhotonTracer( &pTracer, maxRecur, minImportance, branch, reflect, refract, nonmeshlights, useiorstack, power_scale, temporal_samples, regenerate );

	pTracer->AttachScene( pScene );
	pTracer->TracePhotons( num, 1.0, false, pGlobalProgress );

	safe_release( pTracer );
	return true;
}

//! Shoots global photons and populates the global pel photon map
/// \return TRUE if successful, FALSE otherwise
bool Job::ShootGlobalPelPhotons(
	const unsigned int num,							///< [in] Number of photons to acquire
	const double power_scale,						///< [in] How much to scale light power by
	const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
	const double minImportance,						///< [in] Minimum importance when a photon is discarded
	const bool branch,								///< [in] Should the tracer branch or follow a single path?
	const bool nonmeshlights,						///< [in] Should we shoot from non mesh based lights?
	const bool useiorstack,							///< [in] Should the ray caster use a index of refraction stack?
	const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
	const bool regenerate							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
	)
{
	IPhotonTracer* pTracer = 0;
	RISE_API_CreateGlobalPelPhotonTracer( &pTracer, maxRecur, minImportance, branch, nonmeshlights, useiorstack, power_scale, temporal_samples, regenerate );

	pTracer->AttachScene( pScene );
	pTracer->TracePhotons( num, 1.0, false, pGlobalProgress );

	safe_release( pTracer );

	return true;
}

//! Shoots translucent photons and populates the translucent pel photon map
/// \return TRUE if successful, FALSE otherwise
bool Job::ShootTranslucentPelPhotons(
	const unsigned int num,							///< [in] Number of photons to acquire
	const double power_scale,						///< [in] How much to scale light power by
	const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
	const double minImportance,						///< [in] Minimum importance when a photon is discarded
	const bool reflect,								///< [in] Should we trace reflected rays?
	const bool refract,								///< [in] Should we trace refracted rays?
	const bool direct_translucent,					///< [in] Should we trace translucent primary interaction rays?
	const bool nonmeshlights,						///< [in] Should we shoot from non mesh based lights?
	const bool useiorstack,							///< [in] Should the ray caster use a index of refraction stack?
	const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
	const bool regenerate							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
	)
{
	GlobalLog()->PrintEasyWarning( "The Translucent PhotonMap is deprecated.  You should consider using one of the subsurface scattering shaders instead." );

	IPhotonTracer* pTracer = 0;
	RISE_API_CreateTranslucentPelPhotonTracer( &pTracer, maxRecur, minImportance, reflect, refract, direct_translucent, nonmeshlights, useiorstack, power_scale, temporal_samples, regenerate );

	pTracer->AttachScene( pScene );
	pTracer->TracePhotons( num, 1.0, false, pGlobalProgress );

	safe_release( pTracer );
	return true;
}

//! Shoots caustic photons and populates the caustic spectral photon map
/// \return TRUE if successful, FALSE otherwise
bool Job::ShootCausticSpectralPhotons(
	const unsigned int num,							///< [in] Number of photons to acquire
	const double power_scale,						///< [in] How much to scale light power by
	const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
	const double minImportance,						///< [in] Minimum importance when a photon is discarded
	const double nm_begin,							///< [in] Wavelength to start shooting photons at
	const double nm_end,							///< [in] Wavelength to end shooting photons at
	const unsigned int num_wavelengths,				///< [in] Number of wavelengths to shoot photons at
	const bool useiorstack,							///< [in] Should the ray caster use a index of refraction stack?
	const bool branch,								///< [in] Should the tracer branch or follow a single path?
	const bool reflect,								///< [in] Should we trace reflected rays?
	const bool refract,								///< [in] Should we trace refracted rays?
	const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
	const bool regenerate							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
	)
{
	IPhotonTracer* pTracer = 0;
	RISE_API_CreateCausticSpectralPhotonTracer( &pTracer, maxRecur, minImportance, nm_begin, nm_end, num_wavelengths, useiorstack, branch, reflect, refract, power_scale, temporal_samples, regenerate );

	pTracer->AttachScene( pScene );
	pTracer->TracePhotons( num, 1.0, false, pGlobalProgress );

	safe_release( pTracer );

	return true;
}

//! Shoots global photons and populates the global spectral photon map
/// \return TRUE if successful, FALSE otherwise
bool Job::ShootGlobalSpectralPhotons(
	const unsigned int num,							///< [in] Number of photons to acquire
	const double power_scale,						///< [in] How much to scale light power by
	const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
	const double minImportance,						///< [in] Minimum importance when a photon is discarded
	const double nm_begin,							///< [in] Wavelength to start shooting photons at
	const double nm_end,							///< [in] Wavelength to end shooting photons at
	const unsigned int num_wavelengths,				///< [in] Number of wavelengths to shoot photons at
	const bool useiorstack,							///< [in] Should the ray caster use a index of refraction stack?
	const bool branch,								///< [in] Should the tracer branch or follow a single path?
	const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
	const bool regenerate							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
	)
{
	IPhotonTracer* pTracer = 0;
	RISE_API_CreateGlobalSpectralPhotonTracer( &pTracer, maxRecur, minImportance, nm_begin, nm_end, num_wavelengths, useiorstack, branch, power_scale, temporal_samples, regenerate );

	pTracer->AttachScene( pScene );
	pTracer->TracePhotons( num, 1.0, false, pGlobalProgress );

	safe_release( pTracer );

	return true;
}

//! Shoots shadow photons and populates the shadow photon map
/// \return TRUE if successful, FALSE otherwise
bool Job::ShootShadowPhotons(
	const unsigned int num,							///< [in] Number of photons to acquire
	const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
	const bool regenerate							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
	)
{
	IPhotonTracer* pTracer = 0;
	RISE_API_CreateShadowPhotonTracer( &pTracer, temporal_samples, regenerate );

	pTracer->AttachScene( pScene );
	pTracer->TracePhotons( num, 1.0, false, pGlobalProgress );

	safe_release( pTracer );

	return true;
}

//! Predicts the amount of time in ms it will take to rasterize the current scene
/// \return TRUE if successful, FALSE otherwise
bool Job::PredictRasterizationTime(
	unsigned int num,								///< [in] Number of samples to take when determining how long it will take (higher is more accurate)
	unsigned int* ms,								///< [out] Amount of in ms it would take to rasterize
	unsigned int* actual							///< [out] Actual time it took to do the predicted kernel
	)
{
	if( !pRasterizer ) {
		return false;
	}

	ISampling2D* pSampling = 0;
	RISE_API_CreateNRooksSampling2D( &pSampling, 1.0, 1.0, 1.0 );
	pSampling->SetNumSamples( num );

	unsigned int nMs = pRasterizer->PredictTimeToRasterizeScene( *pScene, *pSampling, actual );

	if( ms ) {
		*ms = nMs;
	}

	safe_release( pSampling );
	return true;
}

static IRasterizeSequence* RasterizeSequenceFromOptions()
{
	// Read the raster sequence options from the options file
	IOptions& options = GlobalOptions();

	const int raster_sequence_type = options.ReadInt( "raster_sequence_type", 3 );
	RISE::String raster_sequence = options.ReadString( "raster_sequence_options", "" );

	IRasterizeSequence* pSeq = 0;
	// parse the options
	// Get the raster sequence type
	switch( raster_sequence_type )
	{
	default:
	case 3:
		// Random
		if( GlobalRNG().CanonicalRandom() < 0.1 ) {
			RISE_API_CreateHilbertRasterizeSequence( &pSeq, 4 );
		} else {
			RISE_API_CreateBlockRasterizeSequence( &pSeq, 24, 24, (char)floor(GlobalRNG().CanonicalRandom()*8.999999) );
		}
		break;

	case 0:
		RISE_API_CreateScanlineRasterizeSequence( &pSeq );
		// Scanline
		break;
	case 1:
		unsigned int width, height, type;
		if( sscanf( raster_sequence.c_str(), "%u %u %u", &width, &height, &type ) == 3 ) {
			RISE_API_CreateBlockRasterizeSequence( &pSeq, width, height, (char)type );
		}
		// Block
		break;
	case 2:
		// Hilbert
		unsigned int depth;
		if( sscanf( raster_sequence.c_str(), "%u", &depth ) == 1 ) {
			RISE_API_CreateHilbertRasterizeSequence( &pSeq, depth );
		}
		break;
	}

	return pSeq;
}

//! Rasterizes the entire scene
/// \return TRUE if successful, FALSE otherwise
bool Job::Rasterize(
	)
{
	if( !pRasterizer ) {
		return false;
	}

	IRasterizeSequence* pSeq = 0;

	if( pGlobalProgress ) {
		pRasterizer->SetProgressCallback( pGlobalProgress );

		pSeq = RasterizeSequenceFromOptions();
		if( !pSeq ) {
			RISE_API_CreateBlockRasterizeSequence( &pSeq, 24, 24, 0 );
		}
	}

	pRasterizer->RasterizeScene( *pScene, 0, pSeq );
	safe_release( pSeq );

	return true;
}

//! Rasterizes an animation
/// \return TRUE if successful, FALSE otherwise
bool Job::RasterizeAnimation(
	const double time_start,						///< [in] Scene time to start rasterizing at
	const double time_end,							///< [in] Scene time to finish rasterizing
	const unsigned int num_frames,					///< [in] Number of frames to rasterize
	const bool do_fields,							///< [in] Should the rasterizer do fields?
	const bool invert_fields						///< [in] Should the fields be temporally inverted?
	)
{
	if( !pRasterizer ) {
		return false;
	}

	IRasterizeSequence* pSeq = 0;

	if( pGlobalProgress ) {
		pRasterizer->SetProgressCallback( pGlobalProgress );

		pSeq = RasterizeSequenceFromOptions();
		if( !pSeq ) {
			RISE_API_CreateBlockRasterizeSequence( &pSeq, 24, 24, 0 );
		}
	}

	pRasterizer->RasterizeSceneAnimation( *pScene, time_start, time_end, num_frames, do_fields, invert_fields, 0, 0, pSeq );

	return true;
}

//! Raterizes the scene in this region.  The region values are inclusive!
/// \return TRUE if successful, FALSE otherwise
bool Job::RasterizeRegion(
	const unsigned int left,						///< [in] Left most pixel
	const unsigned int top,							///< [in] Top most scanline
	const unsigned int right,						///< [in] Right most pixel
	const unsigned int bottom						///< [in] Bottom most scanline
	)
{
	if( !pRasterizer ) {
		return false;
	}

	IRasterizeSequence* pSeq = 0;

	if( pGlobalProgress ) {
		pRasterizer->SetProgressCallback( pGlobalProgress );

		pSeq = RasterizeSequenceFromOptions();
		if( !pSeq ) {
			RISE_API_CreateBlockRasterizeSequence( &pSeq, 24, 24, 0 );
		}
	}

	Rect	rc( top, left, bottom, right );
	pRasterizer->RasterizeScene( *pScene, &rc, pSeq );
	safe_release( pSeq );

	return true;
}


//
// Transformation of elements
//

//! Sets the a given object's position
/// \return TRUE if successful, FALSE otherwise
bool Job::SetObjectPosition(
	const char* name,								///< [in] Name of the object
	const double pos[3]								///< [in] Position of the object
	)
{
	if( !name ) {
		return false;
	}

	IObjectPriv* pObj = pObjectManager->GetItem( name );

	if( !pObj ) {
		return false;
	}

	pObj->SetPosition( Point3( pos ) );
	pObj->FinalizeTransformations();
	return true;
}

//! Sets a given object's orientation
/// \return TRUE if successful, FALSE otherwise
bool Job::SetObjectOrientation(
	const char* name,								///< [in] Name of the object
	const double orient[3]							///< [in] Orientation of the object
	)
{
	if( !name ) {
		return false;
	}

	IObjectPriv* pObj = pObjectManager->GetItem( name );

	if( !pObj ) {
		return false;
	}

	pObj->SetOrientation( Vector3( orient ) );
	pObj->FinalizeTransformations();
	return true;
}

//! Sets a given object's scale
/// \return TRUE if successful, FALSE otherwise
bool Job::SetObjectScale(
	const char* name,								///< [in] Name of the object
	const double scale								///< [in] Scaling of the object
	)
{
	if( !name ) {
		return false;
	}

	IObjectPriv* pObj = pObjectManager->GetItem( name );

	if( !pObj ) {
		return false;
	}

	pObj->SetScale( scale );
	pObj->FinalizeTransformations();
	return true;
}

//
// Object modification functions
//

//! Sets the UV generator for an object
/// \return TRUE if successful, FALSE otherwise
bool Job::SetObjectUVToSpherical(
	const char* name,								///< [in] Name of the object
	const double radius								///< [in] Radius of the sphere
	)
{
	IObjectPriv* pObj = pObjectManager->GetItem( name );

	if( !pObj ) {
		return false;
	}

	IUVGenerator* pUV = 0;
	RISE_API_CreateSphericalUVGenerator( &pUV, radius );

	return pObj->SetUVGenerator( *pUV );
}

//! Sets the UV generator for an object
/// \return TRUE if successful, FALSE otherwise
bool Job::SetObjectUVToBox(
	const char* name,								///< [in] Name of the object
	const double width,								///< [in] Width of the box
	const double height,							///< [in] Height of the box
	const double depth								///< [in] Depth of the box
	)
{
	IObjectPriv* pObj = pObjectManager->GetItem( name );

	if( !pObj ) {
		return false;
	}

	IUVGenerator* pUV = 0;
	RISE_API_CreateBoxUVGenerator( &pUV, width, height, depth );

	return pObj->SetUVGenerator( *pUV );
}

//! Sets the UV generator for an object
/// \return TRUE if successful, FALSE otherwise
bool Job::SetObjectUVToCylindrical(
	const char* name,								///< [in] Name of the object
	const double radius,							///< [in] Radius of the cylinder
	const char axis,								///< [in] Axis the cylinder is sitting on
	const double size								///< [in] Size of the cylinder
	)
{
	IObjectPriv* pObj = pObjectManager->GetItem( name );

	if( !pObj ) {
		return false;
	}

	IUVGenerator* pUV = 0;
	RISE_API_CreateCylindricalUVGenerator( &pUV, radius, axis, size );

	return pObj->SetUVGenerator( *pUV );
}

//! Sets the object's surface intersection threshold
/// \return TRUE if successful, FALSE otherwise
bool Job::SetObjectIntersectionError(
	const char* name,								///< [in] Name of the object
	const double error								///< [in] Threshold of error
	)
{
	IObjectPriv* pObj = pObjectManager->GetItem( name );

	if( !pObj ) {
		return false;
	}

	pObj->SetSurfaceIntersecError( error );
	return true;
}

//
// Removal of objects
//

//! Removes the given painter from the scene
/// \return TRUE if successful, FALSE otherwise
bool Job::RemovePainter(
	const char* name								///< [in] Name of the painter to remove
	)
{
	return pPntManager->RemoveItem( name );
}

//! Removes the given material from the scene
/// \return TRUE if successful, FALSE otherwise
bool Job::RemoveMaterial(
	const char* name								///< [in] Name of the material to remove
	)
{
	return pMatManager->RemoveItem( name );
}

//! Removes the given geometry from the scene
/// \return TRUE if successful, FALSE otherwise
bool Job::RemoveGeometry(
	const char* name								///< [in] Name of the geometry to remove
	)
{
	return pGeomManager->RemoveItem( name );
}

//! Removes the given object from the scene
/// \return TRUE if successful, FALSE otherwise
bool Job::RemoveObject(
	const char* name								///< [in] Name of the object to remove
	)
{
	return pObjectManager->RemoveItem( name );
}

//! Removes the given light from the scene
/// \return TRUE if successful, FALSE otherwise
bool Job::RemoveLight(
	const char* name								///< [in] Name of the light to remove
	)
{
	return pLightManager->RemoveItem( name );
}

//! Removes the given modifier from the scene
/// \return TRUE if successful, FALSE otherwise
bool Job::RemoveModifier(
	const char* name								///< [in] Name of the modifer to remove
	)
{
	return pModManager->RemoveItem( name );
}

//! Removes all the rasterizer outputs
/// \return TRUE if successful, FALSE otherwise
bool Job::RemoveRasterizerOutputs(
	)
{
	pRasterizer->FreeRasterizerOutputs();
	return true;
}

//! Clears the entire scene, resets everything back to defaults
/// \return TRUE if successful, FALSE otherwise
bool Job::ClearAll(
	)
{
	DestroyContainers();
	InitializeContainers();

	return true;
}


//! Loading an ascii scene description
/// \return TRUE if successful, FALSE otherwise
bool Job::LoadAsciiScene(
	const char* filename							///< [in] Name of the file containing the scene
	)
{
	if( !filename ) {
		return false;
	}

	ISceneParser* sceneParser = 0;
	RISE_API_CreateAsciiSceneParser( &sceneParser, filename );
	bool bRet = sceneParser->ParseAndLoadScene( *this );
	safe_release( sceneParser );

	return bRet;
}

//! Runs an ascii script
/// \return TRUE if successful, FALSE otherwise
bool Job::RunAsciiScript(
	const char* filename							///< [in] Name of the file containing the script
	)
{
	if( !filename ) {
		return false;
	}

	IScriptParser* scriptParser = 0;
	RISE_API_CreateAsciiScriptParser( &scriptParser, filename );
	bool bRet = scriptParser->ParseScript( *this );
	safe_release( scriptParser );

	return bRet;
}

//! Tells us whether anything is keyframed
bool Job::AreThereAnyKeyframedObjects()
{
	return pScene->GetAnimator()->AreThereAnyKeyframedObjects();
}

//! Adds a keyframe for the specified element
bool Job::AddKeyframe(
	const char* element_type,						///< [in] Type of element to keyframe (ie. camera, painter, geometry, object...)
	const char* element,							///< [in] Name of the element to keyframe
	const char* param,								///< [in] Name of the parameter to keyframe
	const char* value,								///< [in] Value at this keyframe
	const double time,								///< [in] Time of the keyframe
	const char* interp,								///< [in] Type of interpolation to use between this keyframe and the next
	const char* interp_params						///< [in] Parameters to pass to the interpolator (this can be NULL)
	)
{
	if( !element_type || !element || !param || !value ) {
		return false;
	}

	IKeyframable* pkf = 0;

	String type( element_type );
	if( type == "object" ) {
		pkf = pObjectManager->GetItem( element );
	} else if( type == "camera" ) {
		pkf = pScene->GetCamera();
	} else if( type == "geometry" ) {
		pkf = pGeomManager->GetItem( element );
	} else if( type == "painter" ) {
		pkf = pPntManager->GetItem( element );
	} else if( type == "light" ) {
		pkf = pLightManager->GetItem( element );
	}

	if( !pkf ) {
		return false;
	}

	String szinterp = String(interp);
	String szinterpparams = String(interp_params);

	return pScene->GetAnimator()->InsertKeyframe( pkf, String(param), String(value), time, interp?&szinterp:0, interp_params?&szinterpparams:0 );
}

//! Sets animation rasterization options
//! Basically everything that can be passed to RasterizeAnimation can be passed here
//! Then you can just call RasterizeAnimationUsingOptions
bool Job::SetAnimationOptions(
	const double time_start,						///< [in] Scene time to start rasterizing at
	const double time_end,							///< [in] Scene time to finish rasterizing
	const unsigned int num_frames,					///< [in] Number of frames to rasterize
	const bool do_fields,							///< [in] Should the rasterizer do fields?
	const bool invert_fields						///< [in] Should the fields be temporally inverted?
	)
{
	animOptions.time_start = time_start;
	animOptions.time_end = time_end;
	animOptions.num_frames = num_frames;
	animOptions.do_fields = do_fields;
	animOptions.invert_fields = invert_fields;

	return true;
}

//! Rasterizes an animation using the global preset options
/// \return TRUE if successful, FALSE otherwise
bool Job::RasterizeAnimationUsingOptions(
	)

{
	if( !pRasterizer ) {
		return false;
	}

	IRasterizeSequence* pSeq = 0;

	if( pGlobalProgress ) {
		pRasterizer->SetProgressCallback( pGlobalProgress );

		pSeq = RasterizeSequenceFromOptions( );
		if( !pSeq ) {
			RISE_API_CreateBlockRasterizeSequence( &pSeq, 24, 24, 0 );
		}
	}

	pRasterizer->RasterizeSceneAnimation( *pScene,
		animOptions.time_start, animOptions.time_end, animOptions.num_frames, animOptions.do_fields, animOptions.invert_fields, 0, 0, pSeq );

	return true;
}

//! Rasterizes a frame of an animation using the global preset options
/// \return TRUE if successful, FALSE otherwise
bool Job::RasterizeAnimationUsingOptions(
	const unsigned int frame						///< [in] The frame to rasterize
	)
{
	if( !pRasterizer ) {
		return false;
	}

	IRasterizeSequence* pSeq = 0;

	if( pGlobalProgress ) {
		pRasterizer->SetProgressCallback( pGlobalProgress );

		pSeq = RasterizeSequenceFromOptions();
		if( !pSeq ) {
			RISE_API_CreateBlockRasterizeSequence( &pSeq, 24, 24, 0 );
		}
	}

	pRasterizer->RasterizeSceneAnimation( *pScene,
		animOptions.time_start, animOptions.time_end, animOptions.num_frames, animOptions.do_fields, animOptions.invert_fields, 0, &frame, pSeq );

	return true;
}

void Job::SetProgress( IProgressCallback* pProgress ) {
	pGlobalProgress = pProgress;
}
