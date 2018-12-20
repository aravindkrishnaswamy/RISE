//////////////////////////////////////////////////////////////////////
//
//  MersenneTwister.h - Declaration of the MersenneTwister class which
//  generates random numbers
//
//  Author: Aravind Krishnaswamy (class shell), see below for others
//  Date of Birth: September 19, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//                       Also the original authors of this code have
//                       their licence information below.  Their license
//                       is a BSD license, so no worries.
//
//////////////////////////////////////////////////////////////////////


/* 
   A C-program for MT19937, with initialization improved 2002/2/10.
   Coded by Takuji Nishimura and Makoto Matsumoto.
   This is a faster version by taking Shawn Cokus's optimization,
   Matthe Bellew's simplification, Isaku Wada's real version.

   Before using, initialize the state by using init_genrand(seed) 
   or init_by_array(init_key, key_length).

   Copyright (C) 1997 - 2002, Makoto Matsumoto and Takuji Nishimura,
   All rights reserved.                          

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

     1. Redistributions of source code must retain the above copyright
        notice, this list of conditions and the following disclaimer.

     2. Redistributions in binary form must reproduce the above copyright
        notice, this list of conditions and the following disclaimer in the
        documentation and/or other materials provided with the distribution.

     3. The names of its contributors may not be used to endorse or promote 
        products derived from this software without specific prior written 
        permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


   Any feedback is very welcome.
   http://www.math.keio.ac.jp/matumoto/emt.html
   email: matumoto@math.keio.ac.jp
*/

#ifndef MERSENNE_TWISTER_
#define MERSENNE_TWISTER_

namespace RISE
{
	static const unsigned int state_vector_size = 624;

	class MersenneTwister
	{
	protected:
		unsigned long state[state_vector_size]; /* the array for the state vector  */
		int left;
		int initf;
		unsigned long *next;

		void next_state(void);

	public:
		MersenneTwister();
		MersenneTwister( const unsigned int seed );
		virtual ~MersenneTwister();

		void init_genrand(unsigned long s);
		void init_by_array(unsigned long  init_key[], unsigned long key_length);
		unsigned long genrand_int32(void);
		long genrand_int31(void);
		double genrand_real1(void);
		double genrand_real2(void);
		double genrand_real3(void);
		double genrand_res53(void);
	};
}

#endif

