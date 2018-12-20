//////////////////////////////////////////////////////////////////////
//
//  Task.cpp - Simple task implementation
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Task.h"
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
		static inline int divRoundUp( int a, int b )
		{
			return (a+b/2)/b;
		}

		Task::Task( const char * scene, unsigned int x, unsigned int y, const char * output, unsigned int granx, unsigned int grany ) : 
		nResX( x ),
		nResY( y ), 
		nXGranularity( granx ),
		nYGranularity( grany ),
		nTotalActionsSendOut( 0 ),
		bFinishedSendingOut( false ),
		nLastXPixel( 0 ),
		nLastScanline( 0 ),
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

		Task::~Task( )
		{
			safe_release( pOutputImage );
			safe_release( pRasterizerOutput );
		}

		bool Task::GetNewTaskAction( TaskActionID& task_id, IMemoryBuffer& task_data  )
		{
			if( nLastScanline >= nResY-1 ) {
				// We're done!
				bFinishedSendingOut = true;
				return false;
			}

			if( nLastScanline == 0 && nLastXPixel == 0 ) {
				taskLife = GetMilliseconds();
			}

			task_data.Resize( sizeof(char)*1025 + sizeof(unsigned int)*4 );
			task_data.seek( MemoryBuffer::START, 0 );

			unsigned int xstart = nLastXPixel;
			unsigned int xend = xstart+nXGranularity-1;
			unsigned int ystart = nLastScanline;
			unsigned int yend = ystart+nYGranularity-1;

			task_id = ((nLastScanline & 0x0000FFFF) << 16) | (nLastXPixel & 0x0000FFFF);

			if( xend >= nResX ) {
				xend = nResX-1;
			}

			nLastXPixel = xend+1;

			if( xend == nResX-1 ) {
				// Move us to the next y block
				nLastXPixel = 0;
				nLastScanline = yend+1;

				if( nLastScanline >= nResY ) {
					nLastScanline = nResY-1;
				}
			}

			if( yend >= nResY ) {
				yend = nResY-1;
			}

			// Buffer contents
			//   type of task - so the clients know how to interpret the rest of the buffer
			//   filename, 1024 characters long
			//   x pixel to start
			//   x pixel to end
			//   scanline to start, unsigned int
			//   scanline to end
			task_data.setChar( 0 );
			task_data.setBytes( szSceneFileName, 1024 );
			task_data.setUInt( xstart );
			task_data.setUInt( xend );
			task_data.setUInt( ystart );
			task_data.setUInt( yend );

			nTotalActionsSendOut++;

			return true;
		}

		bool Task::GetTaskAction( TaskActionID task_id, IMemoryBuffer& task_data )
		{
			// !@@ Add sanity checks here!

			task_data.Resize( sizeof(char)*1025 + sizeof(unsigned int)*4 );
			task_data.seek( MemoryBuffer::START, 0 );

			// The xstart and ystart are encoded in the task id
			unsigned int xstart = task_id & 0x0000FFFF;
			unsigned int xend = xstart+nXGranularity-1;
			unsigned int ystart = (task_id & 0xFFFF0000) >> 16;;
			unsigned int yend = ystart+nYGranularity-1;

			if( xend >= nResX ) {
				xend = nResX-1;
			}

			if( yend >= nResY ) {
				yend = nResY-1;
			}

			task_data.setChar( 0 );
			task_data.setBytes( szSceneFileName, 1024 );
			task_data.setUInt( xstart );
			task_data.setUInt( xend );
			task_data.setUInt( ystart );
			task_data.setUInt( yend );

			return true;
		}

		bool Task::FinishedTaskAction( const TaskActionID id, IMemoryBuffer& results )
		{
			/* !@@ Do a sanity check here
			if( id > nLastScanline ) {
				// Something is wrong
				GlobalLog()->PrintEx( eLog_Warning, TYPICAL_PRIORITY, "Task::FinishedTaskAction:: says done scanline %d, when last scanline is only %d", id, nLastScanline );
				return false;
			}
			*/

			if( nLastScanline >= nResY-1 ) {
				// We're done!
				bFinishedSendingOut = true;
			}

			// The only thing the results buffer should contain is 
			// the correct scanline
			unsigned int xstart = results.getUInt();
			unsigned int xend = results.getUInt();
			unsigned int scanline_start = results.getUInt();
			unsigned int scanline_end = results.getUInt();

			for( unsigned int y=scanline_start; y<=scanline_end; y++ ) {
				for( unsigned int x=xstart; x<=xend; x++ ) {
					RISEColor c;
					results.getBytes( &c, sizeof(RISEColor) );
					pOutputImage->SetPEL( x, y, c );
				}
			}

			nNumActionsComplete++;

			// Output if necessary
			if( pRasterizerOutput ) {
				Rect rc( scanline_start, xstart, scanline_end, xend );
				pRasterizerOutput->OutputIntermediateImage( *pOutputImage, &rc );
			}

			if( nNumActionsComplete == nTotalActionsSendOut && bFinishedSendingOut ) {
				// Then we are done!, flush to disk, then tell the job engine we are done
				{
					char fname[1024] = {0};

					IRasterizerOutput* fro = 0;

					sprintf( fname, "%s-sRGB", szOutputFileName );
					RISE_API_CreateFileRasterizerOutput( &fro, fname, false, 2, 8, eColorSpace_sRGB );

					fro->OutputImage( *pOutputImage, 0, 0 );
					fro->release();

					sprintf( fname, "%s-ProPhoto", szOutputFileName );
					RISE_API_CreateFileRasterizerOutput( &fro, fname, false, 2, 16, eColorSpace_ProPhotoRGB );

					fro->OutputImage( *pOutputImage, 0, 0 );
					fro->release();				
				}

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
