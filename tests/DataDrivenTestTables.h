//////////////////////////////////////////////////////////////////////
//
//  DataDrivenTestTables.h - synthetic .bdf tables for datadriven_material
//    tests (DL-325).  Header-only; the format is the one
//    DataDrivenBSDF.cpp reads (signature 0xBDF, version 1, emitter
//    positions, patches per emitter; each patch is a BRDF record then a
//    BTDF record, FILE ORDER DESCENDING in theta -- the loader reverses
//    it).  The BTDF records are written as zero: the loader parses them
//    but DataDrivenBSDF::value is reflection-only.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DATADRIVEN_TEST_TABLES_H
#define DATADRIVEN_TEST_TABLES_H

#include <fstream>

namespace DataDrivenTestTables
{
	//! One emitter position, two patches, value `albedo/pi` on every
	//! view/light pair: a Lambertian of that albedo.
	inline bool WriteConstant( const char* path, const double albedo )
	{
		std::ofstream f( path, std::ios::binary );
		if( !f.is_open() ) return false;
		const double kPI = 3.14159265358979323846;
		const int hdr[4] = { 0xBDF, 1, 1, 2 };
		f.write( reinterpret_cast<const char*>( hdr ), sizeof( hdr ) );
		const double v = albedo / kPI;
		const double rec[21] = { kPI / 2,
			0.0, kPI / 4, v, v, v,   0.0, kPI / 4, 0, 0, 0,
			kPI / 4, kPI / 2, v, v, v,   kPI / 4, kPI / 2, 0, 0, 0 };
		f.write( reinterpret_cast<const char*>( rec ), sizeof( rec ) );
		return f.good();
	}

	//! Two emitter positions x three patches, chromatic and
	//! direction-dependent, positive everywhere (so the loader's
	//! non-negative clamp never engages): a kray that disagreed with the
	//! BSDF in SHAPE cannot hide behind a flat function.
	inline bool WriteVaried( const char* path )
	{
		std::ofstream f( path, std::ios::binary );
		if( !f.is_open() ) return false;
		const double kPI = 3.14159265358979323846;
		const int hdr[4] = { 0xBDF, 1, 2, 3 };
		f.write( reinterpret_cast<const char*>( hdr ), sizeof( hdr ) );
		const double emitTheta[2] = { kPI / 3, kPI / 2 };
		for( int e = 0; e < 2; ++e ) {
			f.write( reinterpret_cast<const char*>( &emitTheta[e] ), sizeof( double ) );
			for( int pi = 2; pi >= 0; --pi ) {
				const double begin = pi * kPI / 6, end = ( pi + 1 ) * kPI / 6;
				const double rec[10] = {
					kPI / 2 - end, kPI / 2 - begin,
					( 0.25 + 0.15 * pi + 0.10 * e ) / kPI,
					( 0.50 - 0.12 * pi + 0.05 * e ) / kPI,
					( 0.20 + 0.05 * pi * pi + 0.02 * e ) / kPI,
					begin, end, 0, 0, 0 };
				f.write( reinterpret_cast<const char*>( rec ), sizeof( rec ) );
			}
		}
		return f.good();
	}
}

#endif
