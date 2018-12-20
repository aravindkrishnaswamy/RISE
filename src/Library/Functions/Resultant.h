//////////////////////////////////////////////////////////////////////
//
//  Resultant.h - Finds the resultant polynomial of 2 bicubic polynomails
//
//  Author: Russell O'Connor
//  Date of Birth: May 17, 2004
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RESULTANT_
#define RESULTANT_

#include <assert.h>

#define MAX_COEF 37

namespace RISE
{
	struct SmallPolynomial {
		unsigned int numCoef;
		Scalar coef[MAX_COEF];

		SmallPolynomial() : numCoef(1) {
			memset(coef, 0, sizeof(coef));
		}

		Scalar & operator[](unsigned int index) {
			assert(index < numCoef);
			return coef[index];
		}

		Scalar const & operator[](unsigned int index) const {
			assert(index < numCoef);
			return coef[index];
		}

		Scalar magnitude() const {
			Scalar size = 0;
			for (unsigned int i=0; i < numCoef; i++) {
				size = r_max(size, coef[i]);
			}
			return size;
		}
	};

	SmallPolynomial operator+(SmallPolynomial const &a, SmallPolynomial const &b);
	SmallPolynomial operator*(Scalar a, SmallPolynomial const &b);

	struct BiCubicPolynomial {
		SmallPolynomial poly[4];
		SmallPolynomial const & operator[](unsigned int index) const {
			assert(index < 4);
			return poly[index];
		}
		SmallPolynomial operator()(Scalar u) const {
			return poly[0] + u*poly[1] + (u*u)*poly[2] + (u*u*u)*poly[3];
		}
	};

	SmallPolynomial Resultant (BiCubicPolynomial const &a, BiCubicPolynomial const &b, Scalar epsilon);
}

#endif //RESULTANT_
