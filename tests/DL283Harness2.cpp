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
	// argv: scene n seedBase modes -> per mode: load once, n salted renders,
	// mean of per-render means and mean per-pixel variance across renders.
	if( argc < 5 ) { std::fprintf( stderr, "usage: scene n seedBase modes(e.g. SI)\n" ); return 2; }
	const int n = std::atoi( argv[2] );
	const unsigned int sb = (unsigned int)std::atoi( argv[3] );
	const std::string modes = argv[4];
	for( char mode : modes ) {
		IJobPriv* pJob = nullptr;
		if( !RISE_CreateJobPriv( &pJob ) || !pJob->LoadAsciiSceneViaCst( argv[1] ) ) { std::fprintf( stderr, "load failed\n" ); return 1; }
		pJob->RemoveRasterizerOutputs();
		Cap* c = new Cap();
		GlobalLog()->PrintNew( c, __FILE__, __LINE__, "cap" );
		pJob->GetRasterizer()->AddRasterizerOutput( c );
		std::vector<double> sum, sum2;
		for( int i = 0; i < n; i++ ) {
			SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( sb + unsigned(i), 0x283u ) );
			SobolSamplerTestHooks::Independent().store( mode == 'I' );
			std::srand( sb + unsigned(i) );
			if( !pJob->Rasterize() ) { std::fprintf( stderr, "render failed\n" ); return 1; }
			if( sum.empty() ) { sum.assign( c->px.size(), 0.0 ); sum2.assign( c->px.size(), 0.0 ); }
			double s = 0;
			for( size_t k = 0; k < c->px.size(); k++ ) {
				const double a = ( c->px[k].base.r + c->px[k].base.g + c->px[k].base.b ) / 3.0;
				s += a; sum[k] += a; sum2[k] += a * a;
			}
			std::printf( "RAW %c %d %.9f\n", mode, i, s / double( c->px.size() ) );
			std::fflush( stdout );
		}
		double v = 0;
		for( size_t k = 0; k < sum.size(); k++ ) {
			const double m = sum[k] / n;
			v += ( sum2[k] - n * m * m ) / ( n - 1 );
		}
		std::printf( "PIXVAR %c n=%d mean_per_pixel_variance %.6f\n", mode, n, v / double( sum.size() ) );
		safe_release( c ); safe_release( pJob );
	}
	SobolSamplerTestHooks::ValueSalt().store( 0 );
	SobolSamplerTestHooks::Independent().store( false );
	return 0;
}
