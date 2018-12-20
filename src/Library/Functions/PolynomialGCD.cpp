//////////////////////////////////////////////////////////////////////
//
//  PolynomialGCD.cpp - Implementation of a polynomial GCD methd
//
//  Author: Russell O'Connor
//  Date of Birth: April 19, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

//J. Symbolic Computation (1997) 24, 667-682
//Detection and Validation of Clusters of Polynomial Zeros
//V. HRIBERNIG AND H. J. STETTER

#include "pch.h"
#include "Polynomial.h"
#include "assert.h"

using namespace RISE;

//! coeff1 = coeffDiv*coeff2 + coeffRem
void Polynomial::PolynomialDivRem(
		const std::vector<Scalar> & coeff1,		///< [in] Coefficients of first polynomial
		const std::vector<Scalar> & coeff2,		///< [in] Coefficients of second polynomial
		std::vector<Scalar> & coeffDiv,			///< [out] Coefficients of Divisor
		std::vector<Scalar> & coeffRem			///< [out] COefficients of Remainder
		) {
	coeffRem = coeff1;
	unsigned int len1 = coeffRem.size();
	const unsigned int len2 = coeff2.size();

	coeffDiv.clear();
	coeffDiv.resize(len1-len2+1, 0);

	while (coeffRem.size() >= len2) {
		Scalar d = coeffRem.back()/coeff2.back();
		coeffDiv[len1-len2] = d;
		coeffRem.pop_back();
		for (unsigned int i = 0; i < len2-1; i++) {
			coeffRem[len1-len2+i] -= d*coeff2[i];
		}
		len1 = coeffRem.size();
	}
}

static std::vector<Scalar> PolyMult(const std::vector<Scalar> & a,
									const std::vector<Scalar> & b) {
	std::vector<Scalar> ret(a.size()+b.size()-1,0);
	for (unsigned int i = 0; i < a.size(); i++) {
		for (unsigned int j = 0; j < b.size(); j++) {
			ret[i+j] += a[i]*b[j];
		}
	}
	return ret;
}

static void PolyAccumulate(std::vector<Scalar> & a,
		 			       const std::vector<Scalar> & b) {

	a.resize(r_max(a.size(), b.size()), 0);
	for(unsigned int i = 0; i < b.size(); i++) {
		a[i] += b[i];
	}
}										  

static Scalar PolyNorm(const std::vector<Scalar> & poly) {
	Scalar result = 0;
	for (unsigned int i = 0; i < poly.size(); i++) {
		result += fabs(poly[i]);
	}
	return result;
}

bool Polynomial::PolynomialGCD(
		std::vector<Scalar> coeff1,					///< [in] Coefficients of first polynomial
		std::vector<Scalar> coeff2,					///< [in] Coefficients of second polynomial
		std::vector<Scalar> & coeff,				///< [out] Coefficients of GCD polynomial
		const Scalar epsilon) {
	assert(epsilon > 0.0);
	if (coeff2.size() > coeff1.size()) {
		PolynomialGCD(coeff2, coeff1, coeff, epsilon);
	}

	if (PolyNorm(coeff2) < epsilon) {
		coeff = coeff1;
		return true;
	}

	std::vector<Scalar> s1a, s1b, s2a, s2b, div, rem;
	s2a.push_back(0);
	s2b.push_back(1);
	s1a.push_back(1);

	PolynomialDivRem(coeff1, coeff2, div, rem);
	s1b = div;
	while (PolyNorm(PolyMult(rem, s2b)) > epsilon ||
    	   PolyNorm(PolyMult(rem, s1b)) > epsilon) {
		coeff2.swap(coeff1);
		rem.swap(coeff2);
		PolynomialDivRem(coeff1, coeff2, div, rem);
		PolyAccumulate(s1a, PolyMult(s1b, div));
		s1a.swap(s1b);
		PolyAccumulate(s2a, PolyMult(s2b, div));
		s2a.swap(s2b);
	}
	coeff.swap(coeff2);
	return true;
}
