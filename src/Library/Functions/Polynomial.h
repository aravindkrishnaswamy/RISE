//////////////////////////////////////////////////////////////////////
//
//  Polynomial.h - Declaration of polynomial solving functions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 21, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../Utilities/Math3D/Math3D.h"
#include <vector>

namespace RISE
{
	//! Polynomial solving functions
	class Polynomial
	{
	public:
		//! Solves a quadratic function
		/// \return Number of solutions
		static int SolveQuadric( 
			const Scalar (&coeff)[ 3 ],				///< [in] Coefficients
			Scalar (&sol)[ 2 ]						///< [out] Solutions
			);

		//! Solves a quadratic function within the given range
		/// \return Number of solutions
		static int SolveQuadricWithinRange( 
			const Scalar (&coeff)[ 3 ],				///< [in] Coefficients
			Scalar (&sol)[ 2 ],						///< [out] Solutions
			const Scalar min,						///< [in] Minimum value
			const Scalar max						///< [in] Maximum value
			);

		//! Solves a cubic function
		/// \return Number of solutions
		static int SolveCubic( 
			const Scalar (&coeff)[ 4 ],				///< [in] Coefficients
			Scalar (&sol)[ 3 ]						///< [out] Solutions
			);

		//! Solves a quartic function
		/// \return Number of solutions
		static int SolveQuartic( 
			const Scalar (&coeff)[ 5 ],				///< [in] Coefficients 
			Scalar (&sol)[ 4 ]						///< [out] Solutions
			);

		//! Finds the root of a given polynomial within a given range
		/// \return TRUE if there is a solution, FALSE otherwise
		static bool SolvePolynomialWithinRange( 
			const std::vector<Scalar> & coeff,		///< [in] Coefficients
			const Scalar a,
			const Scalar b,							///< [in] Search for a root between a and b.
			Scalar& solution,						///< [out] Solution
			const Scalar epsilon
			);

		//! Finds the roots of a given polynomial within a given range
		/// \return Number of solutions
		static int SolvePolynomialWithinRange( 
			const std::vector<Scalar> & coeff,		///< [in] Coefficients
			const Scalar a,
			const Scalar b,							///< [in] Search for roots between a and b.
			std::vector<Scalar> & solutions,		///< [out] Solutions
			const Scalar epsilon
			);

		//! coeff1 = coeffDiv*coeff2 + coeffRem
		static void PolynomialDivRem(
			const std::vector<Scalar> & coeff1,		///< [in] Coefficients of first polynomial
			const std::vector<Scalar> & coeff2,		///< [in] Coefficients of second polynomial
			std::vector<Scalar> & coeffDiv,			///< [out] Coefficients of Divisor
			std::vector<Scalar> & coeffRem			///< [out] COefficients of Remainder
			);

		//! Appoximates the GCD of two polynomials
		/// \return TRUE if successful.
		static bool PolynomialGCD(
			std::vector<Scalar> coeff1,				///< [in] Coefficients of first polynomial
			std::vector<Scalar> coeff2,				///< [in] Coefficients of second polynomial
			std::vector<Scalar> & coeff,			///< [out] Coefficients of GCD polynomial
			const Scalar epsilon
			);

		//! Polynomial approximation of the zeroth order modified Bessel function
		//! From Numerical Recipes in C p. 237
		/// \return 0th order Bessel function approximation
		static Scalar bessi0( Scalar x );
	};
}


