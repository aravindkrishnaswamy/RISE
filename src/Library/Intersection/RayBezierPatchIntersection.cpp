//////////////////////////////////////////////////////////////////////
//
//  RayBezierPatchIntersection.cpp - 
//
//  Author: 
//  Date of Birth: May 17, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RayPrimitiveIntersections.h"
#include "../Functions/Polynomial.h"
#include "../Functions/Resultant.h"
#include "../Utilities/Plane.h"

using namespace RISE;

/*
void RayBezierPatchIntersection( 
	const Ray& ray, 
	BEZIER_HIT& hit,
	const BezierPatch& patch
	)
{
	hit.bHit = false;
	hit.dRange = INFINITY;

	Plane plane1, plane2; //Two planes whose intersection is the ray
	MakePlanes(plane1, plane2, ray);

    BiCubicPolynomial biCubicPoly1 = MakeBiCubicPoly(plane1, patch);
    BiCubicPolynomial biCubicPoly2 = MakeBiCubicPoly(plane2, patch);

	SmallPolynomial resultant = Resultant(biCubicPoly1, biCubicPoly2, NEARZERO);

	std::vector<Scalar> coef(resultant.coef, resultant.coef + resultant.numCoef);
	std::vector<Scalar> roots;
	int numroots = Polynomial::SolvePolynomialWithinRange( 
		coef,
		0,
		1,
		roots,
		0);

	for(int i = 0; i < numroots; i++) {
		Scalar u = roots[i];
		SmallPolynomial poly1 = biCubicPoly1(u);
		SmallPolynomial poly2 = biCubicPoly2(u);
		std::vector<Scalar> poly1prime(poly1.coef, poly1.coef+poly1.numCoef);
		std::vector<Scalar> poly2prime(poly2.coef, poly2.coef+poly2.numCoef);
		std::vector<Scalar> poly;
		if (Polynomial::PolynomialGCD(poly1prime, poly2prime, poly, NEARZERO)) {
			Scalar polyprime[4] = {};
			for (unsigned int j=0; j < 4; j++) {
				if (poly.size() > j) polyprime[j] = poly[j];
			}
			Scalar roots[3] = {};
			int moreRoots = Polynomial::SolveCubic(polyprime, roots);
			for (int j=0; j < moreRoots; j++) {
				Scalar v = roots[j];
				if (v >= 0 && v <= 1) {
					AccumulateRoot(hit, ray, u, v);
				}
			}
		}
	}
}
*/


