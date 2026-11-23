//////////////////////////////////////////////////////////////////////
//
//  ClothMaterial.cpp - Cloth material records (simulation M3a)
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ClothMaterial.h"

#include <cmath>

namespace RISE { namespace Simulation { namespace Cloth {

namespace {
const char* Representative =
	"representative literature-range values, not a measured fabric (RISE M3a built-in; "
	"areal density, Kawabata-range bending rigidity and biaxial-tensile-range stretch stiffness)";

Material Make(const char* name, double density, double ew, double ef, double g, double nu,
	double bw, double bf, double alpha, double beta)
{
	Material m;
	m.name = name;
	m.source = Representative;
	m.density = density;
	m.stretchWarp = ew; m.stretchWeft = ef; m.shear = g; m.poisson = nu;
	m.bendWarp = bw; m.bendWeft = bf;
	m.dampingMass = alpha; m.dampingStiffness = beta;
	return m;
}
}

const std::vector<Material>& BuiltInMaterials()
{
	// Damping (alpha 1/s, beta s) is calibrated for plausible motion, not measured.
	static const std::vector<Material> records = {
		//               name                 kg/m2  E_warp  E_weft  G     nu    B_warp  B_weft  alpha beta
		Make("cotton-plain-weave",          0.15,  3000.0, 2000.0, 40.0, 0.20, 2.0e-5, 1.2e-5, 0.05, 2.0e-4),
		Make("denim-twill",                 0.33,  8000.0, 5000.0, 150.0, 0.20, 8.0e-5, 5.0e-5, 0.05, 2.0e-4),
		Make("silk-lightweight",            0.07,  800.0,  600.0,  10.0, 0.20, 2.0e-6, 1.5e-6, 0.05, 2.0e-4),
		Make("polyester-knit",              0.18,  150.0,  100.0,  5.0,  0.30, 4.0e-6, 3.0e-6, 0.05, 2.0e-4),
	};
	return records;
}

bool FindMaterial(const std::string& name, Material& material)
{
	for (const auto& m : BuiltInMaterials())
		if (m.name == name) { material = m; return true; }
	return false;
}

bool ValidateMaterial(const Material& m, std::string& error)
{
	const double values[] = {m.density, m.stretchWarp, m.stretchWeft, m.shear, m.poisson, m.bendWarp,
		m.bendWeft, m.dampingMass, m.dampingStiffness};
	for (double x : values)
		if (!std::isfinite(x)) { error = "material values must be finite"; return false; }
	if (m.density <= 0 || m.stretchWarp <= 0 || m.stretchWeft <= 0 || m.shear <= 0) {
		error = "material density, stretch and shear stiffness must be positive";
		return false;
	}
	if (m.bendWarp < 0 || m.bendWeft < 0 || m.dampingMass < 0 || m.dampingStiffness < 0) {
		error = "material bending rigidity and damping must not be negative";
		return false;
	}
	if ((m.bendWarp == 0) != (m.bendWeft == 0)) {
		error = "material bending rigidities must be both zero or both positive";
		return false;
	}
	if (m.poisson < 0 || m.poisson*m.poisson*m.stretchWeft >= m.stretchWarp) {
		error = "material Poisson ratio must satisfy 0 <= nu_12 and nu_12*nu_21 < 1";
		return false;
	}
	if (m.name.empty() || m.source.empty()) {
		error = "material name and source must not be empty";
		return false;
	}
	return true;
}

RISECBOR64::Value MaterialToValue(const Material& m)
{
	using V = RISECBOR64::Value;
	return V::MapValue({
		{"name", V::String(m.name)}, {"source", V::String(m.source)},
		{"density", V::Float(m.density)},
		{"stretch_warp", V::Float(m.stretchWarp)}, {"stretch_weft", V::Float(m.stretchWeft)},
		{"shear", V::Float(m.shear)}, {"poisson", V::Float(m.poisson)},
		{"bend_warp", V::Float(m.bendWarp)}, {"bend_weft", V::Float(m.bendWeft)},
		{"damping_mass", V::Float(m.dampingMass)}, {"damping_stiffness", V::Float(m.dampingStiffness)}});
}

RISECBOR64::Value MaterialUnits()
{
	using V = RISECBOR64::Value;
	return V::MapValue({
		{"density", V::String("kg/m^2")}, {"stretch_warp", V::String("N/m")}, {"stretch_weft", V::String("N/m")},
		{"shear", V::String("N/m")}, {"poisson", V::String("1")}, {"bend_warp", V::String("N*m")},
		{"bend_weft", V::String("N*m")}, {"damping_mass", V::String("1/s")}, {"damping_stiffness", V::String("s")}});
}

} } }
