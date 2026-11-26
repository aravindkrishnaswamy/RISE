//////////////////////////////////////////////////////////////////////
//
//  ClothMaterial.h - Cloth material records (simulation M3a)
//
//  A record carries every parameter of the cloth model in SI units with
//  its source.  Stiffnesses are thickness-integrated (plane stress):
//  stretch and shear in N/m, bending rigidity per unit width in N*m.
//  "warp" is the material u direction, "weft" the material v direction.
//  The built-in records are representative literature-range values, not
//  measured fabrics; each says so in its source field.  Measured records
//  are added later with their citation.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_CLOTH_MATERIAL_H
#define RISE_SIMULATION_CLOTH_MATERIAL_H

#include "../../Utilities/RISECBOR64.h"
#include <string>
#include <vector>

namespace RISE { namespace Simulation { namespace Cloth {

struct Material {
	std::string name, source;
	double density = 0;          // areal density, kg/m^2
	double stretchWarp = 0;      // E_warp: uniaxial stretch stiffness along u, N/m
	double stretchWeft = 0;      // E_weft: uniaxial stretch stiffness along v, N/m
	double shear = 0;            // G: in-plane shear stiffness, N/m
	double poisson = 0;          // nu_12: weft contraction under warp tension (dimensionless)
	double bendWarp = 0;         // B_warp: rigidity for curvature along u (warp strip), N*m
	double bendWeft = 0;         // B_weft: rigidity for curvature along v (weft strip), N*m
	double dampingMass = 0;      // Rayleigh alpha, 1/s (calibrated, not measured)
	double dampingStiffness = 0; // Rayleigh beta, s (calibrated, not measured)
};

//! The built-in records, in a fixed order.
const std::vector<Material>& BuiltInMaterials();
//! Looks a built-in record up by name.
bool FindMaterial(const std::string& name, Material& material);
//! Positivity, finiteness and the orthotropic plane-stress stability
//! condition nu_12 * nu_21 < 1 (nu_21 = nu_12 * E_weft / E_warp).
bool ValidateMaterial(const Material& material, std::string& error);
//! Canonical map of the record (numbers as float64, name and source as text),
//! as it appears in the solver's effective parameters.
RISECBOR64::Value MaterialToValue(const Material& material);
//! Units of each numeric field, for the physical-records envelope.
RISECBOR64::Value MaterialUnits();

} } }

#endif
