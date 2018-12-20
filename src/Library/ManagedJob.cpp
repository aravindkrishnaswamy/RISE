//////////////////////////////////////////////////////////////////////
//
//  ManagedJob.h - This is a managed job.  This means that the job
//    is exported to .NET under the RISE namespace, allowing
//    C# to use it
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 20, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "stdlib.h"
#include "Interfaces/ILogPriv.h"
#include "Interfaces/IJobPriv.h"
#include "Rendering/Win32WindowRasterizerOutput.h"
#include "Utilities/Log/Win32Console.h"
#include "Utilities/MediaPathLocator.h"
#include <windows.h>
#include <iostream>

using namespace System;
using namespace System::Text;
using namespace System::Runtime::InteropServices;

using namespace RISE;

namespace RISE
{
	__nogc class DummyProgressCallback : public IProgressCallback
	{
		bool Progress( const double progress, const double total ){ return true; }
		void SetTitle( const char* title ){ }
	};

	DummyProgressCallback							dummy;

	__gc public class Job
	{
	protected:
		RISE::IJobPriv*									pBackObj;			// The actual job we are wrapping around

	public:
		Job( ) : pBackObj( 0 )
		{
			IJobPriv* pTemp = 0;
			RISE::RISE_CreateJobPriv( &pTemp );
			pBackObj = pTemp;

			pBackObj->SetProgress( &dummy );

			// Setup the media path locator
			const char* szmediapath = getenv( "RISE_MEDIA_PATH" );

			if( szmediapath ) {
				GlobalMediaPathLocator().AddPath( szmediapath );
			} else {
				GlobalLog()->PrintEasyWarning("Warning! the 'RISE_MEDIA_PATH' environment variable is not set.");
				GlobalLog()->PrintEasyWarning("unless you have been very carefull with explicit media pathing,");
				GlobalLog()->PrintEasyWarning("certain resources may not load.  See the README for information");
				GlobalLog()->PrintEasyWarning("on setting this path.");
			}

			{
				char myfilename[1024] = {0};
				GetModuleFileName( NULL, myfilename, 1023 );
				char mypath[1024] = {0};
				{
					char drive[_MAX_PATH] = {0};
					char dir[_MAX_PATH] = {0};
					_splitpath( myfilename, drive, dir, 0, 0 );
					_makepath( mypath, drive, dir, 0, 0 );
				}

				GlobalMediaPathLocator().AddPath( mypath );
			}
		}

		virtual ~Job( )
		{
			pBackObj->release();
		}

		//
		// Gets information about the library
		//
		bool GetVersion( 
			int pMajorVersion __gc[],									///< [out] Pointer to recieve the major version
			int pMinorVersion __gc[],									///< [out] Pointer to recieve the minor version
			int pRevision __gc[],										///< [out] Pointer to recieve the revision number
			int pBuildNumber __gc[],									///< [out] Pointer to recieve the build numbers
			bool pDebug __gc[]											///< [out] Pointer to bool to recieve whether this is a debug build
			)
		{
			int __pin* pMajorVersion_ = &pMajorVersion[0];
			int __pin* pMinorVersion_ = &pMinorVersion[0];
			int __pin* pRevision_ = &pRevision[0];
			int __pin* pBuildNumber_ = &pBuildNumber[0];
			bool __pin* pDebug_ = &pDebug[0];

			return RISE_API_GetVersion( pMajorVersion_, pMinorVersion_, pRevision_, pBuildNumber_, pDebug_ );
		}

		//! Queries the date the library was built
		/// \return TRUE if successful, FALSE otherwise
		System::String* GetBuildDate(
			)
		{
			char szbuf [1024] = {0};
			RISE_API_GetBuildDate( szbuf, 1024 );

			return new System::String( szbuf );
		}

		//! Queries the time the library was built
		/// \return TRUE if successful, FALSE otherwise
		System::String* GetBuildTime(
			)
		{
			char szbuf [1024] = {0};
			RISE_API_GetBuildTime( szbuf, 1024 );

			return new System::String( szbuf );
		}

		//! Queries for any copyright information
		/// \return TRUE if successful, FALSE otherwise
		System::String* GetCopyrightInformation(
			)
		{
			char szbuf[1024] = {0};
			RISE_API_GetCopyrightInformation( szbuf, 1024 );

			return new System::String( szbuf );
		}

		//! Queries for any special build information
		/// \return TRUE if successful, FALSE otherwise
		System::String* GetBuildSpecialInfo(
			)
		{
			char szbuf[1024] = {0};
			RISE_API_GetBuildSpecialInfo( szbuf, 1024 );

			return new System::String( szbuf );
		}

		//
		// Sets Rasterization parameters
		//

		//! Sets the rasterizer type to be pixel based PEL
		bool SetPixelBasedPelRasterizer(
			const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
			const unsigned int numLumSamples,						///< [in] Number of samples / luminaire
			const unsigned int maxRecur,							///< [in] Maximum recursion level
			const double minImportance,								///< [in] Minimum importance to stop at
			System::String& shader,											///< [in] The default shader
			System::String& globalRadianceMap,								///< [in] Name of the painter for global IBL
			const bool bBackground,									///< [in] Is the radiance map a background object
			const double scale,										///< [in] How much to scale the radiance values
			const double orient __gc[],								///< [in] Euler angles for orienting the radiance map
			System::String& pixelSampler,									///< [in] Type of sampling to use for the pixel sampler
			const double pixelSamplerParam,							///< [in] Parameter for the pixel sampler
			System::String& luminarySampler,								///< [in] Type of sampling to use for luminaries
			const double luminarySamplerParam,						///< [in] Parameter for the luminary sampler
			System::String& pixelFilter,									///< [in] Type of filtering to use for the pixels
			const double pixelFilterWidth,							///< [in] How wide is the pixel filter?
			const double pixelFilterHeight,							///< [in] How high is the pixel filter?
			const double pixelFilterParamA,							///< [in] Pixel filter parameter A
			const double pixelFilterParamB,							///< [in] Pixel filter parameter B
			const bool bShowLuminaires,								///< [in] Should luminaires be shown?
			const bool bUseIORStack,								///< [in] Should the index of refraction stack be used?
			const bool bChooseOnlyOneLight							///< [in] For the luminaire sampler only one random light is chosen for each sample
			)
		{
			IntPtr intptr = Marshal::StringToHGlobalAnsi(&shader);
			const char* szshader = (const char*)(intptr).ToPointer();

			IntPtr intptr2 = Marshal::StringToHGlobalAnsi(&globalRadianceMap);
			const char* szglobalradiancemap = (const char*)(intptr2).ToPointer();

			IntPtr intptr3 = Marshal::StringToHGlobalAnsi(&pixelSampler);
			const char* szpixelsampler = (const char*)(intptr3).ToPointer();

			IntPtr intptr4 = Marshal::StringToHGlobalAnsi(&luminarySampler);
			const char* szluminarysampler = (const char*)(intptr4).ToPointer();

			IntPtr intptr5 = Marshal::StringToHGlobalAnsi(&pixelFilter);
			const char* szpixelfilter = (const char*)(intptr5).ToPointer();

			const double __pin* orient_ = &orient[0];

			bool bRet = pBackObj->SetPixelBasedPelRasterizer(
				numPixelSamples, numLumSamples, maxRecur, minImportance, szshader, szglobalradiancemap, bBackground, scale, orient_, szpixelsampler, pixelSamplerParam, szluminarysampler, luminarySamplerParam, szpixelfilter, pixelFilterWidth, pixelFilterHeight, pixelFilterParamA, pixelFilterParamB, bShowLuminaires, bUseIORStack, bChooseOnlyOneLight );

			Marshal::FreeCoTaskMem( intptr );
			Marshal::FreeCoTaskMem( intptr2 );
			Marshal::FreeCoTaskMem( intptr3 );
			Marshal::FreeCoTaskMem( intptr4 );
			Marshal::FreeCoTaskMem( intptr5 );

			return bRet;
		}

		//! Sets the rasterizer type to be pixel based spectral integrating
		bool SetPixelBasedSpectralIntegratingRasterizer( 
			const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
			const unsigned int numLumSamples,						///< [in] Number of samples / luminaire
			const unsigned int specSamples,							///< [in] Number of spectral samples / pixel
			const double lambda_begin,								///< [in] Wavelength to start sampling at
			const double lambda_end,								///< [in] Wavelength to finish sampling at
			const unsigned int num_wavelengths,						///< [in] Number of wavelengths to sample
			const unsigned int maxRecur,							///< [in] Maximum recursion level
			const double minImportance,								///< [in] Minimum importance to stop at
			System::String& shader,											///< [in] The default shader
			System::String& globalRadianceMap,								///< [in] Name of the painter for global IBL
			const bool bBackground,									///< [in] Is the radiance map a background object
			const double scale,										///< [in] How much to scale the radiance values
			const double orient __gc[],								///< [in] Euler angles for orienting the radiance map
			System::String& pixelSampler,									///< [in] Type of sampling to use for the pixel sampler
			const double pixelSamplerParam,							///< [in] Parameter for the pixel sampler
			System::String& luminarySampler,								///< [in] Type of sampling to use for luminaries
			const double luminarySamplerParam,						///< [in] Parameter for the luminary sampler
			System::String& pixelFilter,									///< [in] Type of filtering to use for the pixels
			const double pixelFilterWidth,							///< [in] How wide is the pixel filter?
			const double pixelFilterHeight,							///< [in] How high is the pixel filter?
			const double pixelFilterParamA,							///< [in] Pixel filter parameter A
			const double pixelFilterParamB,							///< [in] Pixel filter parameter B
			const bool bShowLuminaires,								///< [in] Should luminaires be shown?
			const bool bUseIORStack,								///< [in] Should the index of refraction stack be used?
			const bool bChooseOnlyOneLight							///< [in] For the luminaire sampler only one random light is chosen for each sample
			)
		{
			IntPtr intptr = Marshal::StringToHGlobalAnsi(&shader);
			const char* szshader = (const char*)(intptr).ToPointer();

			IntPtr intptr2 = Marshal::StringToHGlobalAnsi(&globalRadianceMap);
			const char* szglobalradiancemap = (const char*)(intptr2).ToPointer();

			IntPtr intptr3 = Marshal::StringToHGlobalAnsi(&pixelSampler);
			const char* szpixelsampler = (const char*)(intptr3).ToPointer();

			IntPtr intptr4 = Marshal::StringToHGlobalAnsi(&luminarySampler);
			const char* szluminarysampler = (const char*)(intptr4).ToPointer();

			IntPtr intptr5 = Marshal::StringToHGlobalAnsi(&pixelFilter);
			const char* szpixelfilter = (const char*)(intptr5).ToPointer();

			const double __pin* orient_ = &orient[0];

			bool bRet = pBackObj->SetPixelBasedSpectralIntegratingRasterizer(
				numPixelSamples, numLumSamples, specSamples, lambda_begin, lambda_end, num_wavelengths, maxRecur, minImportance, szshader, szglobalradiancemap, bBackground, scale, orient_, szpixelsampler, pixelSamplerParam, szluminarysampler, luminarySamplerParam, szpixelfilter, pixelFilterWidth, pixelFilterHeight, pixelFilterParamA, pixelFilterParamB, bShowLuminaires, bUseIORStack, bChooseOnlyOneLight, false, 0, 0, 0, 0, 0 );

			Marshal::FreeCoTaskMem( intptr );
			Marshal::FreeCoTaskMem( intptr2 );
			Marshal::FreeCoTaskMem( intptr3 );
			Marshal::FreeCoTaskMem( intptr4 );
			Marshal::FreeCoTaskMem( intptr5 );

			return bRet;
		}

		//! Sets the rasterizer type to be adaptive pixel based PEL
		bool SetAdaptivePixelBasedPelRasterizer( 
			const unsigned int numMinPixelSamples,					///< [in] Minimum or base number of samples to start with
			const unsigned int numMaxPixelSamples,					///< [in] Maximum number of samples to go to
			const unsigned int numSteps,							///< [in] Number of steps to maximum sampling level
			const unsigned int numLumSamples,						///< [in] Number of samples / luminaire
			const double threshold,									///< [in] Threshold at which to stop sampling further
			const bool bOutputSamples,								///< [in] Should the renderer show how many samples rather than an image
			const unsigned int maxRecur,							///< [in] Maximum recursion level
			const double minImportance,								///< [in] Minimum importance to stop at
			System::String& shader,											///< [in] The default shader
			System::String& globalRadianceMap,								///< [in] Name of the painter for global IBL
			const bool bBackground,									///< [in] Is the radiance map a background object
			const double scale,										///< [in] How much to scale the radiance values
			const double orient __gc[],								///< [in] Euler angles for orienting the radiance map
			System::String& pixelSampler,									///< [in] Type of sampling to use for the pixel sampler
			const double pixelSamplerParam,							///< [in] Parameter for the pixel sampler
			System::String& luminarySampler,								///< [in] Type of sampling to use for luminaries
			const double luminarySamplerParam,						///< [in] Parameter for the luminary sampler
			System::String& pixelFilter,									///< [in] Type of filtering to use for the pixels
			const double pixelFilterWidth,							///< [in] How wide is the pixel filter?
			const double pixelFilterHeight,							///< [in] How high is the pixel filter?
			const double pixelFilterParamA,							///< [in] Pixel filter parameter A
			const double pixelFilterParamB,							///< [in] Pixel filter parameter B
			const bool bShowLuminaires,								///< [in] Should luminaires be shown?
			const bool bUseIORStack,								///< [in] Should the index of refraction stack be used?
			const bool bChooseOnlyOneLight							///< [in] For the luminaire sampler only one random light is chosen for each sample
			)
		{
			IntPtr intptr = Marshal::StringToHGlobalAnsi(&shader);
			const char* szshader = (const char*)(intptr).ToPointer();

			IntPtr intptr2 = Marshal::StringToHGlobalAnsi(&globalRadianceMap);
			const char* szglobalradiancemap = (const char*)(intptr2).ToPointer();

			IntPtr intptr3 = Marshal::StringToHGlobalAnsi(&pixelSampler);
			const char* szpixelsampler = (const char*)(intptr3).ToPointer();

			IntPtr intptr4 = Marshal::StringToHGlobalAnsi(&luminarySampler);
			const char* szluminarysampler = (const char*)(intptr4).ToPointer();

			IntPtr intptr5 = Marshal::StringToHGlobalAnsi(&pixelFilter);
			const char* szpixelfilter = (const char*)(intptr5).ToPointer();

			const double __pin* orient_ = &orient[0];

			bool bRet = pBackObj->SetAdaptivePixelBasedPelRasterizer( 
				numMinPixelSamples, numMaxPixelSamples, numSteps, numLumSamples, threshold, bOutputSamples, maxRecur, minImportance, szshader, szglobalradiancemap, bBackground, scale, orient_, szpixelsampler, pixelSamplerParam, szluminarysampler, luminarySamplerParam, szpixelfilter, pixelFilterWidth, pixelFilterHeight, pixelFilterParamA, pixelFilterParamB, bShowLuminaires, bUseIORStack, bChooseOnlyOneLight );

			Marshal::FreeCoTaskMem( intptr );
			Marshal::FreeCoTaskMem( intptr2 );
			Marshal::FreeCoTaskMem( intptr3 );
			Marshal::FreeCoTaskMem( intptr4 );
			Marshal::FreeCoTaskMem( intptr5 );

			return bRet;
		}


		//
		// Adds rasterizer outputs
		//

		//! Creates a file rasterizer output
		//! This should be called after a rasterizer has been set
		//! Note that setting a new rasterizer after adding file rasterizer outputs will
		//! delete existing outputs
		/// \return TRUE if successful, FALSE otherwise
		bool AddFileRasterizerOutput(
			System::String& szPattern,										///< [in] File pattern
			const bool bMultiple,									///< [in] Output multiple files (for animations usually)
			const char type,										///< [in] Type of file
																	///		0 - TGA
																	///		1 - PPM
																	///		2 - PNG
			const unsigned char bpp,								///< [in] Bits / pixel for the file
			const char color_space									///< [in] Color space
			)
		{
			IntPtr intptr = Marshal::StringToHGlobalAnsi(&szPattern);
			const char* pattern = (const char*)(intptr).ToPointer();

			bool ret = pBackObj->AddFileRasterizerOutput( pattern, bMultiple, type, bpp, color_space );

			Marshal::FreeCoTaskMem( intptr );

			return ret;
		}

		//! Creates a window rasterizeroutput
		/// \return TRUE if successful, FALSE otherwise
		bool AddWindowRasterizerOutput(
			System::String& title,										///< [in] Window title
			const unsigned int xpos,							///< [in] X position of window
			const unsigned int ypos								///< [in] Y position of window
			)
		{
			IntPtr intptr = Marshal::StringToHGlobalAnsi(&title);
			const char* sztitle = (const char*)(intptr).ToPointer();

			IRasterizerOutput* pWinRO = new Implementation::Win32WindowRasterizerOutput(
						pBackObj->GetScene()->GetCamera()->GetWidth(),
						pBackObj->GetScene()->GetCamera()->GetHeight(),
						xpos, ypos, sztitle );

			pBackObj->GetRasterizer()->AddRasterizerOutput( pWinRO );
			safe_release( pWinRO );

			Marshal::FreeCoTaskMem( intptr );

			return true;
		}

		//! Creates a Win32 Console log printer
		/// \return TRUE if successful, FALSE otherwise
		bool AddWin32ConsoleLogPrinter(
			System::String& title,										///< [in] Window title
			const unsigned int width,							///< [in] Width of the console
			const unsigned int height,							///< [in] Height of the console
			const unsigned int xpos,							///< [in] X position of window
			const unsigned int ypos,							///< [in] Y position of window
			const bool bCreateCommandWindow						///< [in] Should the command window be shown
			)
		{
			IntPtr intptr = Marshal::StringToHGlobalAnsi(&title);
			const char* sztitle = (const char*)(intptr).ToPointer();

			Implementation::Win32Console* pConsole = new Implementation::Win32Console();
			pConsole->Init( sztitle, 0, 0, width, height, xpos, ypos, bCreateCommandWindow );
			{
				// Print some neat little stuff
				LogEvent le;
				le.eType = eLog_Info;
				RISE_API_GetCopyrightInformation( le.szMessage, MAX_STR_SIZE );
				pConsole->Print( le );
			}
			GlobalLogPriv()->AddPrinter( pConsole );		
			safe_release( pConsole );

			Marshal::FreeCoTaskMem( intptr );

			return true;
		}

		void DestroyAllPrinters()
		{
			GlobalLogPriv()->RemoveAllPrinters();
		}

		//! Predicts the amount of time in ms it will take to rasterize the current scene
		/// \return TRUE if successful, FALSE otherwise
		unsigned int PredictRasterizationTime( 
			unsigned int num								///< [in] Number of samples to take when determining how long it will take (higher is more accurate)
			)
		{
			unsigned int ms = 0;
			bool bRet = pBackObj->PredictRasterizationTime( num, &ms, 0 );

			return ms;
		}

		//! Rasterizes the entire scene
		/// \return TRUE if successful, FALSE otherwise
		bool Rasterize( 
			)
		{
			return pBackObj->Rasterize( );
		}

		//! Rasterizes the scene in this region.  The region values are inclusive!
		/// \return TRUE if successful, FALSE otherwise
		bool RasterizeRegion( 
			const unsigned int left,						///< [in] Left most pixel
			const unsigned int top,							///< [in] Top most scanline
			const unsigned int right,						///< [in] Right most pixel
			const unsigned int bottom						///< [in] Bottom most scanline
			)
		{
			return pBackObj->RasterizeRegion( left, top, right, bottom );
		}

		//! Rasterizes an animation
		/// \return TRUE if successful, FALSE otherwise
		bool RasterizeAnimation( 
			const double time_start,						///< [in] Scene time to start rasterizing at
			const double time_end,							///< [in] Scene time to finish rasterizing
			const unsigned int num_frames,					///< [in] Number of frames to rasterize
			const bool do_fields,							///< [in] Should the rasterizer do fields?
			const bool invert_fields						///< [in] Should the fields be temporally inverted?
			)
		{
			return pBackObj->RasterizeAnimation( time_start, time_end, num_frames, do_fields, invert_fields );
		}

		//! Rasterizes an animation using the global preset options
		/// \return TRUE if successful, FALSE otherwise
		bool RasterizeAnimationUsingOptions(
			)
		{
			return pBackObj->RasterizeAnimationUsingOptions( );
		}

		//! Clears the entire scene, resets everything back to defaults
		/// \return TRUE if successful, FALSE otherwise
		bool ClearAll( 
			)
		{
			return pBackObj->ClearAll();
		}

		//! Loading an ascii scene description
		/// \return TRUE if successful, FALSE otherwise
		bool LoadAsciiScene(
			System::String& filename							///< [in] Name of the file containing the scene
			)
		{
			IntPtr intptr = Marshal::StringToHGlobalAnsi(&filename);
			const char* szfilename = (const char*)(intptr).ToPointer();
			
			bool ret = pBackObj->LoadAsciiScene( szfilename );

			Marshal::FreeCoTaskMem( intptr );
			return ret;
		}

		//! Runs an ascii script
		/// \return TRUE if successful, FALSE otherwise
		bool RunAsciiScript(
			System::String& filename						///< [in] Name of the file containing the script
			)
		{
			IntPtr intptr = Marshal::StringToHGlobalAnsi(&filename);
			const char* szfilename = (const char*)(intptr).ToPointer();
			
			bool ret = pBackObj->RunAsciiScript( szfilename );

			Marshal::FreeCoTaskMem( intptr );
			return ret;
		}

		//! Tells us whether anything is keyframed
		bool AreThereAnyKeyframedObjects()
		{
			return pBackObj->AreThereAnyKeyframedObjects();
		}

		//! Adds a keyframe for the specified element
		bool AddKeyframe( 
			System::String& element_type,						///< [in] Type of element to keyframe (ie. camera, painter, geometry, object...)
			System::String& element,							///< [in] Name of the element to keyframe
			System::String& param,								///< [in] Name of the parameter to keyframe
			System::String& value,								///< [in] Value at this keyframe
			const double time,									///< [in] Time of the keyframe
			System::String& interp,								///< [in] Type of interpolation to use between this keyframe and the next
			System::String& interp_params						///< [in] Parameters to pass to the interpolator (this can be NULL)
			)
		{
			IntPtr intptr = Marshal::StringToHGlobalAnsi(&element_type);
			const char* szetype = (const char*)(intptr).ToPointer();

			IntPtr intptr2 = Marshal::StringToHGlobalAnsi(&element_type);
			const char* szelem = (const char*)(intptr2).ToPointer();

			IntPtr intptr3 = Marshal::StringToHGlobalAnsi(&element_type);
			const char* szparam = (const char*)(intptr3).ToPointer();

			IntPtr intptr4 = Marshal::StringToHGlobalAnsi(&element_type);
			const char* szvalue = (const char*)(intptr4).ToPointer();

			IntPtr intptr5 = Marshal::StringToHGlobalAnsi(&element_type);
			const char* szinterp = (const char*)(intptr5).ToPointer();

			IntPtr intptr6 = Marshal::StringToHGlobalAnsi(&element_type);
			const char* szinterpparam = (const char*)(intptr6).ToPointer();

			bool ret = pBackObj->AddKeyframe( szetype, szelem, szparam, szvalue, time, szinterp, szinterpparam );

			Marshal::FreeCoTaskMem( intptr );
			Marshal::FreeCoTaskMem( intptr2 );
			Marshal::FreeCoTaskMem( intptr3 );
			Marshal::FreeCoTaskMem( intptr4 );
			Marshal::FreeCoTaskMem( intptr5 );
			Marshal::FreeCoTaskMem( intptr6 );
			return ret;
		}

		//! Sets animation rasterization options
		//! Basically everything that can be passed to RasterizeAnimation can be passed here
		//! Then you can just call RasterizeAnimationUsingOptions
		/// \return TRUE if successful, FALSE otherwise
		bool SetAnimationOptions(
			const double time_start,						///< [in] Scene time to start rasterizing at
			const double time_end,							///< [in] Scene time to finish rasterizing
			const unsigned int num_frames,					///< [in] Number of frames to rasterize
			const bool do_fields,							///< [in] Should the rasterizer do fields?
			const bool invert_fields						///< [in] Should the fields be temporally inverted?
			)
		{
			return pBackObj->SetAnimationOptions( time_start, time_end, num_frames, do_fields, invert_fields );
		}
	};
}
