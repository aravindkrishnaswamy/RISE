// DL283 SCRATCH HARNESS (reverted before merge): render a scene file n
// times with std::srand(seedBase+i) per render, print mean (r+g+b)/3.
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
	if( argc < 4 ) { std::fprintf( stderr, "usage: scene n seedBase\n" ); return 2; }
	const int n = std::atoi( argv[2] );
	const unsigned int sb = (unsigned int)std::atoi( argv[3] );
	std::vector<double> v; double secs = 0;
	for( int i = 0; i < n; i++ ) {
		IJobPriv* pJob = nullptr;
		if( !RISE_CreateJobPriv( &pJob ) || !pJob->LoadAsciiSceneViaCst( argv[1] ) ) { std::fprintf( stderr, "load failed\n" ); return 1; }
		pJob->RemoveRasterizerOutputs();
		Cap* c = new Cap();
		GlobalLog()->PrintNew( c, __FILE__, __LINE__, "cap" );
		pJob->GetRasterizer()->AddRasterizerOutput( c );
		std::srand( sb + unsigned(i) );
		const auto t0 = std::chrono::steady_clock::now();
		if( !pJob->Rasterize() ) { std::fprintf( stderr, "render failed\n" ); return 1; }
		secs += std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count();
		double s = 0; for( const RISEColor& p : c->px ) s += ( p.base.r + p.base.g + p.base.b ) / 3.0;
		v.push_back( s / double( c->px.size() ) );
		safe_release( c ); safe_release( pJob );
	}
	double m = 0; for( double x : v ) m += x; m /= n;
	double var = 0; for( double x : v ) var += ( x - m ) * ( x - m );
	const double sd = n > 1 ? std::sqrt( var / ( n - 1 ) ) : 0;
	std::printf( "RESULT n=%d mean=%.8f sd=%.4g sem=%.4g secs=%.2f\n", n, m, sd, sd / std::sqrt( (double)n ), secs );
	return 0;
}
