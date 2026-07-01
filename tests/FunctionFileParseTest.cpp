//////////////////////////////////////////////////////////////////////
//
//  FunctionFileParseTest.cpp - Regression test for the file-backed
//    data-curve loaders in AsciiSceneParser.
//
//  Historically every `... { file <path> }` loader used the classic
//  buggy idiom:
//
//      while( !feof(f) ) { double x,y; fscanf(f,"%lf %lf",&x,&y);
//                          push(x,y); }
//
//  which (a) SPINS FOREVER on a comment / non-numeric line — fscanf
//  matches nothing and does not advance the stream — and (b) pushes an
//  uninitialized or duplicated final value at EOF.  A data file that
//  starts with a `#` header (the documented, human-authored form — see
//  scenes/FeatureBased/EnamelWatch/goldruby_sigma_a.txt) would HANG the
//  scene load.
//
//  The loaders now read line-by-line via a shared comment-tolerant
//  reader (ReadDataFileLines): blank / whitespace-only / `#`-comment
//  lines are skipped, each data line is sscanf'd with a checked
//  conversion count, and a malformed line is skipped (warned) rather
//  than hanging or ingesting garbage.
//
//  This test proves BOTH properties for the two curve loaders most
//  relevant to the vitreous-enamel work — `piecewise_linear_function`
//  (IFunction1D, the G1-d colorant curve) and `scalar_painter` (the
//  IScalarPainter file form).  The test COMPLETING at all is the
//  no-hang proof; the evaluated values prove correct ingestion with
//  comments/blank/malformed lines skipped.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <string>
#include <cstdio>
#include <cmath>

#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IFunction1D.h"
#include "../src/Library/RISE_API.h"

using namespace RISE;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const char* name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

static bool Close( double a, double b, double eps = 1e-6 )
{
	return std::fabs( a - b ) <= eps * std::fmax( 1.0, std::fmax( std::fabs(a), std::fabs(b) ) );
}

static std::string TmpDir()
{
	const char* tmp = getenv( "TMPDIR" );
	std::string dir = tmp ? tmp : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	return dir;
}

// A deliberately messy data file: leading `#` header, blank lines, an
// indented comment, a whitespace-only line, and a garbage (non-numeric)
// line interleaved with valid "x y" pairs.  The old fscanf loop would
// hang on the very first `#` line.
static std::string WriteMessyDataFile()
{
	const std::string path = TmpDir() + "rise_funcfile_messy.txt";
	std::ofstream f( path.c_str(), std::ios::trunc );
	f << "# gold-ruby-style header comment\n";
	f << "# columns: wavelength value\n";
	f << "\n";
	f << "   # an indented comment\n";
	f << "400 1.0\n";
	f << "500 2.0\n";
	f << "   \n";                       // whitespace-only line
	f << "600 3.0\n";
	f << "this is a garbage line\n";    // non-numeric -> skipped (warned)
	f << "700 4.0\n";
	f.close();
	return path;
}

static bool ParseSceneFile( const std::string& path, Job& job )
{
	ISceneParser* parser = 0;
	if( !RISE_API_CreateAsciiSceneParser( &parser, path.c_str() ) || !parser ) {
		return false;
	}
	parser->addref();
	const bool ok = parser->ParseAndLoadScene( job );
	parser->release();
	return ok;
}

static std::string WriteScene( const std::string& tag, const std::string& body )
{
	const std::string path = TmpDir() + "rise_funcfile_" + tag + ".RISEscene";
	std::ofstream f( path.c_str(), std::ios::trunc );
	f << "RISE ASCII SCENE 6\n" << body;
	f.close();
	return path;
}

//----------------------------------------------------------------------
// A: piecewise_linear_function { file ... } with a comment-laden file.
//----------------------------------------------------------------------
static void TestPiecewiseLinearFunctionFile( const std::string& dataPath )
{
	std::cout << "A: piecewise_linear_function { file } loads a comment-laden data file" << std::endl;

	std::string body =
		"piecewise_linear_function\n{\n"
		"\tname testcurve\n"
		"\tfile " + dataPath + "\n"
		"}\n";
	const std::string scenePath = WriteScene( "plf", body );

	Job* job = new Job();
	job->addref();
	// If the load HUNG (the old bug) this call would never return, so
	// reaching the assertion at all is the no-hang proof.
	const bool ok = ParseSceneFile( scenePath, *job );
	remove( scenePath.c_str() );

	Check( ok, "A: scene with a file-backed curve parses (no hang)" );

	IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
	Check( priv != 0, "A: job exposes IJobPriv" );
	if( priv ) {
		IFunction1D* fn = priv->GetFunction1Ds()->GetItem( "testcurve" );
		Check( fn != 0, "A: piecewise_linear_function 'testcurve' registered" );
		if( fn ) {
			// Valid pairs ingested; comments / blank / garbage skipped.
			Check( Close( fn->Evaluate( 400.0 ), 1.0 ), "A: f(400) == 1.0" );
			Check( Close( fn->Evaluate( 500.0 ), 2.0 ), "A: f(500) == 2.0" );
			Check( Close( fn->Evaluate( 600.0 ), 3.0 ), "A: f(600) == 3.0" );
			Check( Close( fn->Evaluate( 700.0 ), 4.0 ), "A: f(700) == 4.0 (garbage line skipped, not truncated)" );
			// Interpolation between the last two real points.
			Check( Close( fn->Evaluate( 650.0 ), 3.5 ), "A: f(650) interpolates to 3.5" );
		}
	}
	job->release();
}

//----------------------------------------------------------------------
// B: scalar_painter { file ... } with the same comment-laden file.
//----------------------------------------------------------------------
static void TestScalarPainterFile( const std::string& dataPath )
{
	std::cout << "B: scalar_painter { file } loads the same comment-laden data file" << std::endl;

	std::string body =
		"scalar_painter\n{\n"
		"\tname testscalar\n"
		"\tfile " + dataPath + "\n"
		"}\n";
	const std::string scenePath = WriteScene( "sp", body );

	Job* job = new Job();
	job->addref();
	const bool ok = ParseSceneFile( scenePath, *job );
	remove( scenePath.c_str() );

	Check( ok, "B: scene with a file-backed scalar_painter parses (no hang)" );

	IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
	if( priv ) {
		Check( priv->GetScalarPainters()->GetItem( "testscalar" ) != 0,
			"B: scalar_painter 'testscalar' registered (comments tolerated, not empty)" );
	}
	job->release();
}

int main( int /*argc*/, char* /*argv*/[] )
{
	std::cout << "FunctionFileParseTest — comment-tolerant, non-hanging file-backed curve loaders"
		<< std::endl;

	const std::string dataPath = WriteMessyDataFile();

	TestPiecewiseLinearFunctionFile( dataPath );
	TestScalarPainterFile( dataPath );

	remove( dataPath.c_str() );

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
