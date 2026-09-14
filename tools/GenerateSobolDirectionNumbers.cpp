//////////////////////////////////////////////////////////////////////
//
//  GenerateSobolDirectionNumbers.cpp - Offline tool that converts the
//    Joe-Kuo `new-joe-kuo-6.21201` initial direction numbers into the
//    embedded table RISE's Sobol' sequence is built from.
//
//  Outputs: src/Library/Sampling/SobolDirectionNumbers.cpp
//
//  Provenance of the input
//  -----------------------
//    https://web.maths.unsw.edu.au/~fkuo/sobol/new-joe-kuo-6.21201
//    (the authors' recommended set, search criterion D(6), last
//    updated 16 September 2010; licence at
//    https://web.maths.unsw.edu.au/~fkuo/sobol/licence -- BSD-style,
//    reproduced verbatim in the generated file).
//
//    Columns are `d s a m_1 ... m_s`: d is the Joe-Kuo dimension
//    number (1-based, and d=1 -- the van der Corput dimension -- is
//    NOT in the file), s the degree of the primitive polynomial, a the
//    number whose bits are the polynomial's interior coefficients, and
//    m_i the initial direction numbers.  RISE dimension j corresponds
//    to Joe-Kuo dimension d = j + 1, so RISE dimension 0 is the van
//    der Corput sequence and RISE dimension 1 is Joe-Kuo d=2 (s=1,
//    a=0, m_1=1), which is exactly `SobolSequence::SobolDim1`.
//
//  What this tool verifies before emitting anything
//  ------------------------------------------------
//    * every m_i is admissible (odd, and m_i < 2^i);
//    * every (s, a) pair really is a primitive polynomial over GF(2),
//      by checking that x has multiplicative order exactly 2^s - 1 in
//      GF(2)[x]/(p);
//    * the file's polynomials appear in the canonical Sobol' order --
//      by increasing degree, and within a degree by increasing a --
//      with no gaps, which is what lets the generated table be a flat
//      per-dimension record with no polynomial index.
//
//  Build (from project root):
//    c++ -O2 -o bin/tools/gen_sobol_dirnums tools/GenerateSobolDirectionNumbers.cpp
//
//  Run:
//    curl -O https://web.maths.unsw.edu.au/~fkuo/sobol/new-joe-kuo-6.21201
//    bin/tools/gen_sobol_dirnums new-joe-kuo-6.21201 8192 \
//        > src/Library/Sampling/SobolDirectionNumbers.cpp
//
//  Author: Aravind Krishnaswamy
//  Date: September 14, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	struct Row
	{
		uint32_t				d;
		uint32_t				s;
		uint32_t				a;
		std::vector<uint32_t>	m;
	};

	//! (a * b) mod p in GF(2)[x], p of degree d.
	uint32_t PolyMulMod( uint32_t a, uint32_t b, uint32_t p, unsigned int d )
	{
		uint32_t r = 0;
		while( b != 0 ) {
			if( b & 1u ) r ^= a;
			b >>= 1;
			a <<= 1;
			if( ( a >> d ) & 1u ) a ^= p;
		}
		return r;
	}

	//! Is x of multiplicative order exactly 2^d - 1 in GF(2)[x]/(p)?
	//! Walks the powers of x directly -- O(2^d) but d <= 17 here, and a
	//! direct walk is the version least likely to share a mistake with
	//! any clever factorisation-based test.
	bool PolyOrderIsFull( uint32_t p, unsigned int d )
	{
		const uint32_t n = ( 1u << d ) - 1u;
		uint32_t x = PolyMulMod( 1u, 2u, p, d );
		uint32_t order = 1u;
		while( x != 1u && order <= n ) { x = PolyMulMod( x, 2u, p, d ); order++; }
		return x == 1u && order == n;
	}
}

int main( int argc, char** argv )
{
	if( argc < 2 ) {
		std::fprintf( stderr,
			"usage: %s <new-joe-kuo-6.21201> [numDimensions]\n", argv[0] );
		return 1;
	}
	const unsigned int numDims = ( argc > 2 ) ? unsigned( std::atoi( argv[2] ) ) : 8192u;

	std::ifstream in( argv[1] );
	if( !in ) {
		std::fprintf( stderr, "cannot open %s\n", argv[1] );
		return 1;
	}

	std::string line;
	std::getline( in, line );		// column header: "d  s  a  m_i"

	std::vector<Row> rows;
	while( std::getline( in, line ) ) {
		std::istringstream ss( line );
		Row r;
		if( !( ss >> r.d >> r.s >> r.a ) ) continue;
		uint32_t mi;
		while( ss >> mi ) r.m.push_back( mi );
		if( r.m.size() != r.s ) {
			std::fprintf( stderr, "dimension %u: %zu initial numbers, degree %u\n",
				r.d, r.m.size(), r.s );
			return 1;
		}
		if( r.d > numDims ) break;
		rows.push_back( r );
	}

	if( rows.size() + 1u != numDims ) {
		std::fprintf( stderr, "wanted %u dimensions, file supplied %zu\n",
			numDims, rows.size() + 1u );
		return 1;
	}

	// ---- verification ------------------------------------------------
	uint32_t expectDim = 2, expectDeg = 1, expectA = 0;
	for( size_t i = 0; i < rows.size(); i++ )
	{
		const Row& r = rows[i];
		if( r.d != expectDim ) {
			std::fprintf( stderr, "dimension gap: expected %u, got %u\n", expectDim, r.d );
			return 1;
		}
		for( size_t k = 0; k < r.m.size(); k++ ) {
			const uint32_t bit = uint32_t( k ) + 1u;
			if( ( r.m[k] & 1u ) == 0u || r.m[k] >= ( 1u << bit ) ) {
				std::fprintf( stderr, "dimension %u: m_%u = %u is not admissible\n",
					r.d, bit, r.m[k] );
				return 1;
			}
		}
		// Canonical order: walk degrees and `a` values ourselves, skipping
		// the non-primitive `a`, and require the file to name exactly the
		// next one we reach.
		for( ;; ) {
			if( expectA >= ( 1u << ( expectDeg - 1u ) ) ) { expectDeg++; expectA = 0; continue; }
			const uint32_t p = ( 1u << expectDeg ) | ( expectA << 1 ) | 1u;
			if( PolyOrderIsFull( p, expectDeg ) ) break;
			expectA++;
		}
		if( r.s != expectDeg || r.a != expectA ) {
			std::fprintf( stderr,
				"dimension %u: file says (s=%u, a=%u), canonical order says (s=%u, a=%u)\n",
				r.d, r.s, r.a, expectDeg, expectA );
			return 1;
		}
		expectA++;
		expectDim++;
	}
	std::fprintf( stderr,
		"verified %zu dimensions: admissible initial numbers, primitive polynomials,"
		" canonical order, max degree %u\n",
		rows.size(), rows.back().s );

	// ---- emit --------------------------------------------------------
	size_t total = 0;
	for( size_t i = 0; i < rows.size(); i++ ) total += 2u + rows[i].m.size();

	std::printf(
"//////////////////////////////////////////////////////////////////////\n"
"//\n"
"//  SobolDirectionNumbers.cpp - initial direction numbers for RISE's\n"
"//    Sobol' sequence (`src/Library/Sampling/SobolSequence.h`).\n"
"//\n"
"//  GENERATED FILE -- DO NOT EDIT BY HAND.  Regenerate with:\n"
"//\n"
"//    curl -O https://web.maths.unsw.edu.au/~fkuo/sobol/new-joe-kuo-6.21201\n"
"//    c++ -O2 -o bin/tools/gen_sobol_dirnums tools/GenerateSobolDirectionNumbers.cpp\n"
"//    bin/tools/gen_sobol_dirnums new-joe-kuo-6.21201 %u \\\n"
"//        > src/Library/Sampling/SobolDirectionNumbers.cpp\n"
"//\n"
"//  The generator re-verifies the data every time it runs: each m_i is\n"
"//  admissible (odd, m_i < 2^i), each (s, a) is a primitive polynomial\n"
"//  over GF(2), and the polynomials appear in the canonical Sobol' order\n"
"//  (increasing degree, then increasing a) with no gaps.\n"
"//\n"
"//  Layout: one variable-length record per Sobol' dimension 1 ..\n"
"//  kSobolJoeKuoDimensions-1, laid end to end --\n"
"//      s, a, m_1, m_2, ... m_s\n"
"//  Dimension 0 (van der Corput) carries no polynomial and no record.\n"
"//\n"
"//  Source data\n"
"//  -----------\n"
"//    Joe & Kuo, \"Constructing Sobol sequences with better\n"
"//    two-dimensional projections\", SIAM J. Sci. Comput. 30, 2635-2654\n"
"//    (2008); file `new-joe-kuo-6.21201`, the authors' recommended set\n"
"//    (search criterion D(6)), covering 21201 dimensions, last updated\n"
"//    16 September 2010.  Only the first %u dimensions are embedded here;\n"
"//    see `SobolSequence::kNumDimensions` for why that is the size.\n"
"//\n"
"//  Licence pertaining to sobol.cc and the accompanying sets of\n"
"//  direction numbers (https://web.maths.unsw.edu.au/~fkuo/sobol/licence),\n"
"//  reproduced verbatim as its redistribution terms require:\n"
"//\n"
"//    Copyright (c) 2008, Frances Y. Kuo and Stephen Joe\n"
"//    All rights reserved.\n"
"//\n"
"//    Redistribution and use in source and binary forms, with or without\n"
"//    modification, are permitted provided that the following conditions\n"
"//    are met:\n"
"//\n"
"//        * Redistributions of source code must retain the above copyright\n"
"//          notice, this list of conditions and the following disclaimer.\n"
"//\n"
"//        * Redistributions in binary form must reproduce the above\n"
"//          copyright notice, this list of conditions and the following\n"
"//          disclaimer in the documentation and/or other materials\n"
"//          provided with the distribution.\n"
"//\n"
"//        * Neither the names of the copyright holders nor the names of\n"
"//          the University of New South Wales and the University of\n"
"//          Waikato and its contributors may be used to endorse or\n"
"//          promote products derived from this software without specific\n"
"//          prior written permission.\n"
"//\n"
"//    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY\n"
"//    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE\n"
"//    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR\n"
"//    PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE\n"
"//    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR\n"
"//    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF\n"
"//    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR\n"
"//    BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF\n"
"//    LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\n"
"//    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE\n"
"//    USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH\n"
"//    DAMAGE.\n"
"//\n"
"//  Tabs: 4\n"
"//\n"
"//  License Information: Please see the attached LICENSE.TXT file\n"
"//\n"
"//////////////////////////////////////////////////////////////////////\n"
"\n"
"#include <stdint.h>\n"
"\n"
"namespace RISE\n"
"{\n"
"\tnamespace\n"
"\t{\n"
"\t\t//! %zu values: one `s, a, m_1 .. m_s` record per dimension 1 .. %u.\n"
"\t\tconst uint32_t kJoeKuoRecords[] = {\n",
		numDims, numDims, total, numDims - 1u );

	std::string out;
	out.reserve( total * 8u );
	char buf[32];
	for( size_t i = 0; i < rows.size(); i++ )
	{
		const Row& r = rows[i];
		out += "\t\t\t";
		std::snprintf( buf, sizeof(buf), "%u,%u", r.s, r.a );
		out += buf;
		for( size_t k = 0; k < r.m.size(); k++ ) {
			std::snprintf( buf, sizeof(buf), ",%u", r.m[k] );
			out += buf;
		}
		out += ",\n";
	}
	std::fwrite( out.data(), 1, out.size(), stdout );

	std::printf(
"\t\t};\n"
"\t}\n"
"\n"
"\t//! Flat `s, a, m_1 .. m_s` records for Sobol' dimensions 1 .. N-1.\n"
"\tconst uint32_t* SobolJoeKuoInitialNumbers()\n"
"\t{\n"
"\t\treturn kJoeKuoRecords;\n"
"\t}\n"
"\n"
"\t//! How many Sobol' dimensions the records cover, dimension 0 included.\n"
"\tunsigned int SobolJoeKuoDimensionCount()\n"
"\t{\n"
"\t\treturn %uu;\n"
"\t}\n"
"\n"
"\t//! How many uint32_t values the record array holds.\n"
"\tunsigned int SobolJoeKuoRecordCount()\n"
"\t{\n"
"\t\treturn %zuu;\n"
"\t}\n"
"}\n",
		numDims, total );

	std::fprintf( stderr, "emitted %zu values for %u dimensions (%zu bytes of table data)\n",
		total, numDims, total * sizeof(uint32_t) );
	return 0;
}
