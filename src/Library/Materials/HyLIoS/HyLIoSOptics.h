#pragma once

#include <optional>

namespace RISE::HyLIoS {

// Pure scalar primitives for the candidate hylios_reference_interp_v1 convention.
// Lengths are metres, rates inverse metres, angles radians, fractions in [0,1].
// Invalid/nonfinite inputs throw invalid_argument. Rates/distances that become
// nonfinite or round a positive value to zero throw overflow_error/underflow_error.
// Nonzero subnormal rounding is permitted. No spectra or physiological defaults.
double ProlateSurfaceToVolume(double semiMinorMetres, double semiMajorMetres);
double GeometricEncounterRate(double surfaceToVolumePerMetre, double volumeFraction);

// Eq. (5), density relative to d(cos(alpha)); chi1=pi*a*b, chi2=pi*a*a.
double OrientationCosinePdf(double cosine, double semiMinorMetres, double semiMajorMetres);
double SampleOrientationCosine(double uniform, double semiMinorMetres, double semiMajorMetres);

// Uniform variates everywhere are finite in [0,1). No RNG state is owned here.
// nullopt means no event (zero rate); U=0 at a positive rate gives distance 0.
std::optional<double> SampleExponentialFreePath(double ratePerMetre, double uniform);

// A zero scale denotes an exact forward delta, not a continuous angular PDF.
bool IsExponentialForwardDelta(double angularScaleRadians);
double ExponentialForwardNormalization(double angularScaleRadians);
// Relative to d(theta), with independent uniform azimuth. Continuous scale > 0.
// A positive tail below binary64 range rounds to zero; an overflowing PDF throws.
double ExponentialForwardPolarPdf(double thetaRadians, double angularScaleRadians);
struct PolarSample { double thetaRadians; bool isForwardDelta; };
// Exactly one Gamma(shape=2) proposal followed by sin(theta)/theta acceptance.
// nullopt is a rejected proposal: caller may resume, never count it as absorption.
std::optional<PolarSample> TrySampleExponentialForward(double angularScaleRadians,
	double uniform1, double uniform2, double uniformAccept);

// Paper Eq. (7): the contrast denominator is eta^2+1, deliberately not +2.
double RayleighRate(double fiberRadiusMetres, double wavelengthMetres,
	double fiberVolumeFraction, double relativeRefractiveIndex);
// Relative to d(cos(theta)), normalized over [-1,1].
double RayleighCosinePdf(double cosine);
// Exactly one uniform-cosine proposal; nullopt is rejection, not termination.
std::optional<double> TrySampleRayleighCosine(double uniformProposal, double uniformAccept);

} // namespace RISE::HyLIoS
