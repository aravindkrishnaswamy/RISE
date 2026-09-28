// DL283 SCRATCH HARNESS 2 (removed before merge): salted Sobol (S) vs
// independent (I) renders, interleaved; argv: scene n seedBase modes
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <cmath>
#include <string>
#include <chrono>
#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Utilities/SobolSampler.h"
using namespace RISE;
using namespace RISE::Implementation;
namespace RISE { bool RISE_CreateJobPriv( IJobPriv** ppi ); }
class Cap : public virtual IRasterizerOutput, public virtual Reference {
public:
	std::vector<RISEColor> px;
protected:
	virtual ~Cap() {}
public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	virtual void OutputImage( const IRasterImage& im, const Rect*, const unsigned int ) override {
		px.resize( im.GetWidth() * im.GetHeight() );
		for( unsigned int y = 0; y < im.GetHeight(); y++ )
			for( unsigned int x = 0; x < im.GetWidth(); x++ ) px[y * im.GetWidth() + x] = im.GetPEL( x, y );
	}
};
int main( int argc, char** argv )
{
	if( argc < 5 ) { std::fprintf( stderr, "usage: scene n seedBase modes(e.g. SI)\n" ); return 2; }
	const int n = std::atoi( argv[2] );
	const unsigned int sb = (unsigned int)std::atoi( argv[3] );
	const std::string modes = argv[4];
	for( int i = 0; i < n; i++ ) {
		for( char mode : modes ) {
			IJobPriv* pJob = nullptr;
			if( !RISE_CreateJobPriv( &pJob ) || !pJob->LoadAsciiSceneViaCst( argv[1] ) ) { std::fprintf( stderr, "load failed\n" ); return 1; }
			pJob->RemoveRasterizerOutputs();
			Cap* c = new Cap();
			GlobalLog()->PrintNew( c, __FILE__, __LINE__, "cap" );
			pJob->GetRasterizer()->AddRasterizerOutput( c );
			SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( sb + unsigned(i), 0x283u + unsigned(mode) ) );
			SobolSamplerTestHooks::Independent().store( mode == 'I' );
			std::srand( sb + unsigned(i) );
			const auto t0 = std::chrono::steady_clock::now();
			if( !pJob->Rasterize() ) { std::fprintf( stderr, "render failed\n" ); return 1; }
			const double secs = std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count();
			double s = 0; for( const RISEColor& p : c->px ) s += ( p.base.r + p.base.g + p.base.b ) / 3.0;
			std::printf( "RAW %c %d %.9f %.2f\n", mode, i, s / double( c->px.size() ), secs );
			std::fflush( stdout );
			safe_release( c ); safe_release( pJob );
		}
	}
	SobolSamplerTestHooks::ValueSalt().store( 0 );
	SobolSamplerTestHooks::Independent().store( false );
	return 0;
}
