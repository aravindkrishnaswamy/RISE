#include "pch.h"
#include "../Utilities/Math3D/Math3D.h"
#include <memory.h>
#include <algorithm>
#include "Resultant.h"

using namespace RISE;

template<typename T> T Determinant2x2( T const & a,
							  T const & b,
							  T const & c,
							  T const & d) {
    return a*d-b*c;
}
							  
template<typename T> T Determinant3x3( T const & a,
							  T const & b,
							  T const & c,
							  T const & d,
							  T const & e,
							  T const & f,
							  T const & g,
							  T const & h,
							  T const & i ) {
    return a*Determinant2x2(e,f,h,i)+
		   b*Determinant2x2(f,d,i,g)+
		   c*Determinant2x2(d,e,g,h);
}

namespace RISE
{
	// These operators access the underlying fixed-size `coef[]` array
	// directly rather than via operator[].  The array is zero-initialised
	// in the default constructor, so indices past `numCoef` read as zero.
	// The debug-mode assert in operator[] would otherwise misfire whenever
	// the result needs to carry more coefficients than either input.
	SmallPolynomial operator+(SmallPolynomial const &a, SmallPolynomial const &b) {
		SmallPolynomial ret;
		ret.numCoef = r_max(a.numCoef, b.numCoef);
		for (unsigned int i=0; i < ret.numCoef; i++) {
			ret.coef[i] = a.coef[i] + b.coef[i];
		}
		return ret;
	}

	SmallPolynomial operator-(SmallPolynomial const &a, SmallPolynomial const &b) {
		SmallPolynomial ret;
		ret.numCoef = r_max(a.numCoef, b.numCoef);
		for (unsigned int i=0; i < ret.numCoef; i++) {
			ret.coef[i] = a.coef[i] - b.coef[i];
		}
		return ret;
	}

	SmallPolynomial operator*(SmallPolynomial const &a, SmallPolynomial const &b) {
		SmallPolynomial ret;
		ret.numCoef = a.numCoef + b.numCoef;
		if (ret.numCoef > MAX_COEF) {
			assert(false);
			ret.numCoef = 0;
			return ret;
		}
		for (unsigned int i=0; i < ret.numCoef; i++) {
			Scalar sum = 0.0;
			for (unsigned int j=0; j <= i; j++) {
				const Scalar av = (j         < a.numCoef) ? a.coef[j]     : 0.0;
				const Scalar bv = ((i-j)     < b.numCoef) ? b.coef[i-j]   : 0.0;
				sum += av * bv;
			}
			ret.coef[i] = sum;
		}
		return ret;
	}

	SmallPolynomial operator*(Scalar a, SmallPolynomial const &b) {
		SmallPolynomial ret;
		ret.numCoef = b.numCoef;
		for (unsigned int i=0; i < ret.numCoef; i++) {
			ret.coef[i] = a * b.coef[i];
		}
		return ret;
	}
}

namespace RISE
{

SmallPolynomial Bezout3 (BiCubicPolynomial const &a, BiCubicPolynomial const &b) {
	SmallPolynomial c11 = b[0]*a[3]-a[0]*b[3];
	SmallPolynomial c12 = b[0]*a[2]-a[0]*b[2];
	SmallPolynomial c13 = b[0]*a[1]-a[0]*b[1];
	SmallPolynomial c21 = b[1]*a[3]-a[1]*b[3];
	SmallPolynomial c22 = c11+b[1]*a[2]-a[1]*b[2];
	SmallPolynomial c31 = b[2]*a[3]-a[2]*b[3];
	return Determinant3x3(
		c11, c12, c13,
		c21, c22, c12,
		c31, c21, c11);
}

SmallPolynomial Bezout2 (BiCubicPolynomial const &a, BiCubicPolynomial const &b) {
	SmallPolynomial c11 = b[0]*a[2]-a[0]*b[2];
	SmallPolynomial c12 = b[0]*a[1]-a[0]*b[1];
	SmallPolynomial c21 = b[1]*a[2]-a[1]*b[2];
	return Determinant2x2(
		c11, c12,
		c21, c11);
}

SmallPolynomial Bezout1 (BiCubicPolynomial const &a, BiCubicPolynomial const &b) {
	return b[0]*a[1]-a[0]*b[1];
}

SmallPolynomial Resultant (BiCubicPolynomial const &a, BiCubicPolynomial const &b, Scalar epsilon) {
	assert(epsilon >= 0.0);
	if (a[3].magnitude() > epsilon || b[3].magnitude() > epsilon) return Bezout3(a, b);
	if (a[2].magnitude() > epsilon || b[2].magnitude() > epsilon) return Bezout2(a, b);
	if (a[1].magnitude() > epsilon || b[1].magnitude() > epsilon) return Bezout1(a, b);
	return SmallPolynomial();
}

} // namespace RISE
