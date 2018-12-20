//////////////////////////////////////////////////////////////////////
//
//  risempi.cpp - This uses MPI to render a scene in parallel, except
//  this uses a better scheme of dividing up the data, such that
//  there is better load balancing.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 28, 2002
//  Tabs: 4
//  Comments:
//
//  VERY IMPORTANT:  NOTE that scenes passed to us MUST be complete
//  AND must NOT include the render command.  Doing that will 
//  screw this renderer.  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <stdio.h>
#include <fstream>
#include "../Library/RISE_API.h"
#include "../Library/Interfaces/ILogPriv.h"
#include "../Library/Interfaces/IJobPriv.h"
#include "../Library/Utilities/RTime.h"
#include "../Library/Utilities/Reference.h"
#include "../Library/Utilities/Log/StreamPrinter.h"
#include "mpi.h"

using namespace RISE;
using namespace RISE::Implementation;

//
// Defines a render output object that simply keeps the output surface here
// 
class StoreRasterizerOutput : public virtual IRasterizerOutput, public virtual Reference
{
public:
	int			width;
	int			height;
	RISEColor*	pStoredOutput;

protected:
	virtual ~StoreRasterizerOutput( )
	{
		if( pStoredOutput ) {
			GlobalLog()->PrintDelete( pStoredOutput, __FILE__, __LINE__ );
			delete [] pStoredOutput;
		}
	}
	
public:
	StoreRasterizerOutput() :
	  width( 0 ),
	  height( 0 ),
	  pStoredOutput( 0 )
	{
	}

	virtual void	OutputIntermediateImage( 
		const IRasterImage& pImage,
		const Rect* pRegion
		)
	{
		// We don't really do anything for intermediate output
	}
	
	virtual void	OutputImage( const IRasterImage& pImage, const Rect* pRegion, const unsigned int frame )
	{
		if( !pRegion ) {
			return;
		}

		if( !pStoredOutput ) {
			// Allocate our store
			width = pImage.GetWidth();
			height = pImage.GetHeight();
			pStoredOutput = new RISEColor[ width * height ];
			GlobalLog()->PrintNew( pStoredOutput, __FILE__, __LINE__, "store" );
		}

		// Copy
		for( unsigned int y = pRegion->top; y<= pRegion->bottom; y++ ) {
			for( unsigned int x = pRegion->left; x<= pRegion->right; x++ ) {
				pStoredOutput[ y*width + x ] = pImage.GetPEL( x, y );
			}
		}
	}
};

void CopyScanlineToSurface( IRasterImage* pDest, RISEColor* pSrc, int scanline, int width )
{
	// Copy the RISEColor information to the surface
	for( int x=0; x<=width; x++ ) {
		pDest->SetPEL( x, scanline, pSrc[x] );
	}
}

std::vector<IRasterizerOutput*> saved_rasterizer_outputs;

class EnumRenderOutputsCallback : public IEnumCallback<IRasterizerOutput>
{
	public:
	bool operator() (const IRasterizerOutput& ro )
	{
		ro.addref();
		saved_rasterizer_outputs.push_back( (IRasterizerOutput*)&ro );
		return true;
	}
};

void Usage()
{
	std::cout << "Usage: mpirun -np #procs ./risempi <scene file> [S|A] <num frames>" << std::endl;
	std::cout << "Example: mpirun -np 8 ./risempi ../scenes/cornellbox.RISEscene S      -or-" << std::endl;
	std::cout << "         mpirun -np 8 ./risempi ../scenes/cornellbox.RISEscene A 240" << std::endl;

}

void DoStill( int rank, int size, IJobPriv* pJob )
{
	if( rank == 0 ) {
		// Save the render outputs, so that we can write back to them later
		EnumRenderOutputsCallback dispatch;
		pJob->GetRasterizer()->EnumerateRasterizerOutputs( dispatch ); 
	}

	pJob->GetRasterizer()->FreeRasterizerOutputs();

	// Now attach a new rasterizeroutput, our rasterizer output
	StoreRasterizerOutput* sro = new StoreRasterizerOutput( );
	GlobalLog()->PrintNew( sro, __FILE__, __LINE__, "store rasterizer output" );
	pJob->GetRasterizer()->AddRasterizerOutput( sro );

	unsigned int total_width = pJob->GetScene()->GetCamera()->GetWidth();
	unsigned int total_height = pJob->GetScene()->GetCamera()->GetHeight();

	if( rank == 0 )
	{
#if 1
		// We should predict how long the scene will take to render under ideal load balancing
		unsigned int		predicted = 0;
		pJob->PredictRasterizationTime( 4096, &predicted, 0 );

		unsigned int		mins = predicted/1000/60;
		unsigned int		secs = (predicted-(mins*1000*60))/1000;
		unsigned int		ms = predicted % 1000;
		std::cout << "Predicted Render Time for 1 CPU: " << mins << " minutes, " << secs << " seconds, " << ms << " ms" << std::endl;

		unsigned int		ideal_predicted = predicted / size;
		unsigned int		ideal_mins = ideal_predicted/1000/60;
		unsigned int		ideal_secs = (ideal_predicted-(ideal_mins*1000*60))/1000;
		unsigned int		ideal_ms = ideal_predicted % 1000;
		
		std::cout << "Thus under ideal load balancing: "  << ideal_mins << " minutes, " << ideal_secs << " seconds, " << ideal_ms << " ms" << std::endl;
#endif

		std::cout << "Image is: " << total_width << "x" << total_height << std::endl;
		std::cout << "CPU 0 is starting to render..." << std::endl;
	}

	// Now loop through all the scanlines, and render the ones
	// that when modded by the number of units equals our rank!
	for( unsigned int j=0; j<total_height; j++ ) {
		if( int(j%size) == rank ) {
			pJob->RasterizeRegion( 0, 0, j, total_width-1, j, 0, 0 );
		}
	}
	
	if( rank == 0 ) {
		std::cout << "CPU 0 has finished rendering..." << std::endl;
	}
		
	// Delete objects
	pJob->release();

	// Now if we are processing unit 0, we collect the data
	// If we aren't, we transmit our data to processing unit 0
	if( rank == 0 )
	{
		// First copy our own data into an image 
		IRasterImage*	pImage = 0;
		RISE_API_CreateRISEColorRasterImage( &pImage, total_width, total_height, RISEColor(0,0,0,0) );

		// Now recieve the data from all the other processing units and copy it to our local buffer as it comes in
		std::cout << "CPU 0 is getting data from CPUs" << std::endl;

		// Allocate the buffer in which to receive the data
		int buf_size = total_width;
		RISEColor*	buf = new RISEColor[ buf_size ];
		GlobalLog()->PrintNew( buf, __FILE__, __LINE__, "buffer" );
		
		for( unsigned int j=0; j<total_height; j++ )
		{
			int cpu_for_scanline = j % size;
			
			// If this is our own scanline, then great! just copy
			if( cpu_for_scanline == 0 ) {
				CopyScanlineToSurface( pImage, &sro->pStoredOutput[ j*total_width ], j, total_width );
			} else {	
				// Recieve data from the processors in order
				MPI_Status status;
			
				// The message ID is the scanline number
				// And we know exactly what CPU we are to recieve that scanline from
				MPI_Recv( buf, buf_size*sizeof(RISEColor), MPI_BYTE, cpu_for_scanline, j, MPI_COMM_WORLD, &status );

				CopyScanlineToSurface( pImage, buf, j, total_width );
			}
		}

		// Free the buffer
		GlobalLog()->PrintDelete( buf, __FILE__, __LINE__ );
		delete [] buf;

		std::cout << "Writing data to original render outputs" << std::endl;

		std::vector<IRasterizerOutput*>::iterator it;
		for( it=saved_rasterizer_outputs.begin(); it != saved_rasterizer_outputs.end(); it++ ) {
			IRasterizerOutput* pRO = *it;
			pRO->OutputImage( *pImage, 0, 0 );
			pRO->release();
		}

		saved_rasterizer_outputs.clear();

		pImage->release();
		
	}
	else
	{
		// Again loop through and send our scanlines
		for( unsigned int j=0; j<total_height; j++ )
		{
			// Again the message tag is the scanline
			if( int(j % size) == rank ) {
				MPI_Send( &sro->pStoredOutput[ j*total_width ], sro->width*sizeof(RISEColor), MPI_BYTE, 0, j, MPI_COMM_WORLD );
			}
		}
	}

	// Free render output
	sro->release();
}

void DoAnimation( int rank, int size, IJobPriv* pJob, unsigned int total_frames )
{
	// Animations are a bit different
	// There is no master and no slave, every processor just does its appropriate frame and writes all its output
	for( unsigned int i=0; i<total_frames; i++ ) {
		if( int(i%size) == rank ) {
			pJob->RasterizeAnimationUsingOptions( i, 0, 2, "5" );
		}
	}
}

int main( int argc, char** argv )
{
	// Intialize MPI, get our rank and size
	int rank, size;

	MPI_Init( &argc, &argv );
	MPI_Comm_rank( MPI_COMM_WORLD, &rank );
	MPI_Comm_size( MPI_COMM_WORLD, &size );

	// Disable the memory leak tracer
	// @!! Have to turn this into an API call 
//	if( GlobalLogPriv()->memTracker ) {
//		delete GlobalLogPriv()->memTracker;
//		GlobalLogPriv()->memTracker = 0;
//	}

	// Disable aspects of the logger, so that different processes don't conflict on the same log file
	GlobalLogPriv()->RemoveAllPrinters();

	// Now add back the file printer
	char buf[1024];
	sprintf( buf, "RISELog_Proc%d.txt", rank );
	std::ofstream*		fs = new std::ofstream( buf );
	StreamPrinter*		pB = new StreamPrinter( fs, true );
	GlobalLog()->PrintNew( pB, __FILE__, __LINE__, "file stream printer" );
	GlobalLogPriv()->AddPrinter( pB );
	pB->release();

	// If we are processor 0, then add a standard out printer
	if( rank == 0 ) {
		StreamPrinter*	pA = new StreamPrinter( &std::cout, true, eLog_Console );
		GlobalLog()->PrintNew( pA, __FILE__, __LINE__, "cout stream printer" );
		GlobalLogPriv()->AddPrinter( pA );
		pA ->release();
	}

	// Seed the randomizer, we want to make sure that different processes aren't generating the same random numbers
	srand( rank );

	int major, minor, revision, build;
	bool isdebug;

	RISE_API_GetVersion( &major, &minor, &revision, &build, &isdebug );

	// Only display the following info if we are processor 0
	if( rank == 0 )
	{
		std::cout << "===============================================" << std::endl;
		std::cout << "                                               " << std::endl;
		std::cout << "  R.I.S.E - Realistic Image Synthesis Engine   " << std::endl;
		std::cout << "       Message Passing Interface (MPI)         " << std::endl;
		std::cout << "              Parallel Renderer                " << std::endl;
		std::cout << "              v." << major  << "." << minor << "." << revision << " build " << build << std::endl;
		std::cout << "      built on " << __DATE__ << " at " << __TIME__ << std::endl;
		std::cout << "     (c) 2001-2004 Aravind Krishnaswamy        " << std::endl;
		std::cout << "                                               " << std::endl;
		std::cout << "===============================================" << std::endl;
	}
	
	// Start the barrier, so that we can do some timing
	MPI_Barrier( MPI_COMM_WORLD );
	double start = MPI_Wtime();

	IJobPriv* pJob = 0;
	RISE_CreateJobPriv( &pJob );

	// If there was a file specified in the arguments, then assume its an 
	// ascii scene file and load it

	if( argc < 3 ) {
		if( rank == 0 ) {
			Usage();
		}
		MPI_Finalize();		
		return 1;
	}
	
	if( rank == 0 ) {
		std::cout << "Loading ascii scene file: " << argv[1] << std::endl;
	}
	pJob->LoadAsciiScene( argv[1] );
	
	if( argv[2][0] == 'S' ) {
		DoStill( rank, size, pJob );
	} else if( argv[2][0] == 'A' ) {
		if( argc < 4 ) {
			if( rank == 0 ) {
				Usage();
			}	
			MPI_Finalize();		
			return 1;
		}
		DoAnimation( rank, size, pJob, atoi(argv[3]) );
	} else {
		if( rank == 0 ) {
			Usage();
		}
		MPI_Finalize();		
		return 1;
	}


	// Barrier again to sync everybody up
	// Start the barrier, so that we can do some timing
	MPI_Barrier( MPI_COMM_WORLD );
	double finish = MPI_Wtime();
	double time = finish-start;

	if( rank == 0 )
	{
		// Write out the total time
		int mins = int(time) / 60;
		int secs = int(time) % 60;
		std::cout << "Actual time for all operations: " << mins << " minutes, " << secs << " seconds." << std::endl;
	}

	// Free MPI
	MPI_Finalize();

	return 1;
}

