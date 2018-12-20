//////////////////////////////////////////////////////////////////////
//
//  AnimationTask.cpp - Simple task implementation
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 30, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "AnimationTask.h"
#include "../Library/RISE_API.h"
#include "../Library/Utilities/RTime.h"
#include <string>

//
// Windows version has a windows window that shows the render progress
//
#ifdef _WIN32
#include "../Library/Rendering/Win32WindowRasterizerOutput.h"
#include <windows.h>
#endif

namespace RISE
{
	namespace Implementation
	{
		AnimationTask::AnimationTask( const char * scene, const unsigned int x, const unsigned int y, const char * output, const unsigned int frames ) : 
		nResX( x ),
		nResY( y ), 
		nFrames( frames ),
		nTotalActionsSendOut( 0 ),
		bFinishedSendingOut( false ),
		nLastFrame( 0 ),
		nNumActionsComplete( 0 ), 
		taskLife( 0 ),
		pRasterizerOutput( 0 )
		{
			strncpy( szSceneFileName, scene, 1024 );
			strncpy( szOutputFileName, output, 1024 );

			RISE_API_CreateRISEColorRasterImage( &pOutputImage, nResX, nResY, RISEColor( 0, 0, 0, 0 ) );

	#ifdef WIN32
			/* disabled for Inscriber Christmas rendering */
			pRasterizerOutput = new Implementation::Win32WindowRasterizerOutput(
				x, y,
				50, 50, "D.R.I.S.E. Server Results Window" );
	#endif
		}

		AnimationTask::~AnimationTask( )
		{
			safe_release( pOutputImage );
		}

		bool AnimationTask::GetNewTaskAction( TaskActionID& task_id, IMemoryBuffer& task_data  )
		{
			if( nLastFrame >= nFrames ) {
				// We're done!
				bFinishedSendingOut = true;
				return false;
			}

			if( nLastFrame == 0 ) {
				taskLife = GetMilliseconds();
			}

			task_data.Resize( sizeof(char)*1025 + sizeof(unsigned int)*4 );
			task_data.seek( MemoryBuffer::START, 0 );

			task_id = nLastFrame;

			// Buffer contents
			//   type of task - so the clients know how to interpret the rest of the buffer
			//   filename, 1024 characters long
			//   time to render at
			task_data.setChar( 1 );
			task_data.setBytes( szSceneFileName, 1024 );
			task_data.setUInt( nLastFrame );

			nLastFrame++;

			nTotalActionsSendOut++;

			return true;
		}

		bool AnimationTask::GetTaskAction( TaskActionID task_id, IMemoryBuffer& task_data )
		{
			// !@@ Add sanity checks here!

			task_data.Resize( sizeof(char)*1025 + sizeof(unsigned int)*4 );
			task_data.seek( MemoryBuffer::START, 0 );

			// The frame number is encoded in the task id
			task_data.setChar( 1 );
			task_data.setBytes( szSceneFileName, 1024 );
			task_data.setUInt( task_id );

			return true;
		}

		bool AnimationTask::FinishedTaskAction( const TaskActionID id, IMemoryBuffer& results )
		{
			if( nLastFrame >= nFrames ) {
				// We're done!
				bFinishedSendingOut = true;
			}

			// The only thing the results buffer should contain is 
			// a perfect RISEImage of the correct resolution
			for( unsigned int y=0; y<nResY; y++ ) {
				for( unsigned int x=0; x<nResX; x++ ) {
					RISEColor c;
					results.getBytes( &c, sizeof(RISEColor) );
					pOutputImage->SetPEL( x, y, c );
				}
			}

			// Output if necessary
			if( pRasterizerOutput ) {
				pRasterizerOutput->OutputIntermediateImage( *pOutputImage, 0 );
			}


			// Flush the frame to disk
			{
				char fname[1024] = {0};

				IRasterizerOutput* fro = 0;

				sprintf( fname, "%s_%.5d", szOutputFileName, id );
				RISE_API_CreateFileRasterizerOutput( &fro, fname, false, 2, 16, eColorSpace_ProPhotoRGB );

				fro->OutputImage( *pOutputImage, 0, 0 );
				fro->release();
			}

			nNumActionsComplete++;

			if( nNumActionsComplete == nTotalActionsSendOut && bFinishedSendingOut ) {
				// Then we are done!, tell the job engine we are done
				unsigned int		timeforTask = GetMilliseconds() - taskLife;

				unsigned int		days = timeforTask/1000/60/60/24;
				timeforTask -= days*1000*60*60*24;
				unsigned int		hours = timeforTask/1000/60/60;
				timeforTask -= hours*1000*60*60;
				unsigned int		mins = timeforTask/1000/60;
				timeforTask -= mins*1000*60;
				unsigned int		secs = timeforTask/1000;
				unsigned int		ms = timeforTask % 1000;

				char buf[1024] = {0};
				strcat( buf, "Total Rasterization Time: " );
				char daybuf[32] = {0};
				sprintf( daybuf, "%d days ", days );
				char hourbuf[32] = {0};
				sprintf( hourbuf, "%d hours ", hours );
				char minbuf[32] = {0};
				sprintf( minbuf, "%d minutes ", mins );
				char secbuf[32] = {0};
				sprintf( secbuf, "%d seconds ", secs );
				char msbuf[32] = {0};
				sprintf( msbuf, "%d ms", ms );

				if( days ) {
					strcat( buf, daybuf );
				}

				if( hours ) {
					strcat( buf, hourbuf );
				}

				if( mins ) {
					strcat( buf, minbuf );
				}

				if( secs ) {
					strcat( buf, secbuf );
				}

				if( ms ) {
					strcat( buf, msbuf );
				}

				strcat( buf, "\n" );

				GlobalLog()->Print( eLog_Event, buf );

				return true;
			}

			return false;
		}
	}
}

