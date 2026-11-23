//////////////////////////////////////////////////////////////////////
//
//  ClothValidation.h - Analytic validation cases for the cloth model
//  (simulation M3a, docs/SIMULATION_M3A_RESULTS.md)
//
//  Each runner builds a structured sheet with the given material,
//  triangulation and integrator, runs the model and compares it with a
//  reference computed here:
//    catenary     strip hanging between two supports at equal height, no
//                 bending; reference: the elastic catenary for the model's
//                 own 1-D stretch law (Irvine, Cable Structures, 1981, ch. 2,
//                 with Hooke's law replaced by St. Venant-Kirchhoff),
//                 integrated by composite Simpson quadrature.
//    cantilever   strip clamped at one end under its own weight; reference:
//                 the elastica B theta'' = -q (L - s) lambda cos theta,
//                 theta(0) = 0, theta'(L) = 0 (lambda: the model's axial
//                 stretch, about 1 + 1e-5), solved by shooting with RK4.
//    membrane     pretensioned rectangle, fixed edges, free (m,n) mode;
//                 reference: f = 1/2 sqrt((T_u (m/a)^2 + T_v (n/b)^2)/sigma).
//                 Frequency measured from zero crossings of the modal
//                 coordinate (cubic interpolation).
//    plate        square plate, simply supported or clamped, free
//                 fundamental mode; reference omega = lambda^2/a^2 sqrt(D/sigma)
//                 with lambda^2 = 2 pi^2 (simply supported) or 35.985
//                 (clamped; Leissa 1969 and later Ritz/superposition
//                 values 35.985-35.992), D = B (the model has no Poisson
//                 coupling in bending; see ClothModel.h).
//    energy       undamped membrane mode: energy change per period against
//                 the integrator's exact linear amplification factor.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_CLOTH_VALIDATION_H
#define RISE_SIMULATION_CLOTH_VALIDATION_H

#include "ClothModel.h"
#include <string>
#include <vector>

namespace RISE { namespace Simulation { namespace Cloth {

struct CaseResult {
	bool ok = false;              // the run completed
	std::string failure;          // why not
	double error = 0;             // relative error against the reference
	double measured = 0, reference = 0;
	double secondary = 0;         // case-specific (shape error, predicted value, ...)
	int resolution = 0;           // segments or cells across
	std::string detail;
	StepStats stats;
};

// ---------------------------------------------------------------- references

//! Elastica of a cantilever (clamped at s = 0, free at L) under a uniform
//! load q per unit area (weight) and rigidity B per unit width; axial
//! stretch from the model's St. Venant-Kirchhoff law with stiffness E (N/m),
//! or inextensible when E = 0.  Positions (x along the clamp direction, drop
//! downwards) at the requested reference arc lengths (ascending, in [0, L]).
bool ElasticaCantilever(double q, double rigidity, double length, double stretchStiffness, const std::vector<double>& s,
	std::vector<double>& x, std::vector<double>& drop, std::string& error);

//! Elastic catenary of unstretched length L0 between supports at equal
//! height, span S, weight w0 per unstretched length, axial law
//! T = EA lambda (lambda^2 - 1)/2.  Positions (x from the left support,
//! sag downwards) at material coordinates s in [0, L0].  Also returns H.
bool ElasticCatenary(double L0, double span, double w0, double EA, const std::vector<double>& s,
	std::vector<double>& x, std::vector<double>& sag, double& horizontal, std::string& error);

//! Energy ratio per oscillation period predicted by the linear analysis of
//! a scheme at omega*dt with N steps per period (implicit midpoint: 1).
double PredictedEnergyRatio(Scheme scheme, double omegaDt, double stepsPerPeriod);
//! Clamped square plate fundamental frequency parameter used as reference.
constexpr double ClampedSquarePlateLambda2 = 35.985;

// ---------------------------------------------------------------- cases

struct CaseSettings {
	Material material;
	Triangulation triangulation = Triangulation::Alternating;
	BendingModel bending = BendingModel::Lines;
	Scheme scheme = Scheme::BDF2;
	NewtonOptions newton;        // tolerance is overridden per case where the scale demands it
	int resolution = 20;         // segments along the strip or cells across the sheet
	double stepsPerPeriod = 0;   // 0: 2.5 * resolution
	double periods = 3;
};

CaseResult RunCatenary(const CaseSettings& settings);
CaseResult RunCantilever(const CaseSettings& settings, bool weft);
CaseResult RunMembraneMode(const CaseSettings& settings, int m, int n);
CaseResult RunPlate(const CaseSettings& settings, bool clamped);
//! Energy drift per period of the undamped (1,1) membrane mode: measured
//! = 1 - (energy ratio per period), reference = predicted loss; error is
//! |measured - reference| (absolute, in units of the oscillation energy).
CaseResult RunEnergyDrift(const CaseSettings& settings);

//! Observed order from errors at successive refinements by a factor 2.
double ObservedOrder(double coarse, double fine);

} } }

#endif
