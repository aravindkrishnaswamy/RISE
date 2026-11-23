// Versioned native simulation grid payloads. See docs/SIMULATION_GRID_FORMAT.md.
#ifndef RISE_SIMULATION_GRID_H
#define RISE_SIMULATION_GRID_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RISE { namespace Simulation {

struct GridBlock {
	std::array<std::int32_t,3> origin{};
	std::array<std::uint32_t,3> dimensions{};
	bool constant = false;
	// Constant: one activity byte and one tuple. Dense: packed activity bits
	// followed by one tuple per cell, x fastest; values are binary32 bits.
	std::vector<std::uint8_t> active;
	std::vector<std::uint32_t> values;
};

struct GridChannel {
	std::string name, units;
	std::uint8_t components = 1;
	std::array<std::int32_t,3> origin{};
	std::array<std::uint32_t,3> dimensions{};
	std::array<double,16> indexToWorld{{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
	std::array<std::uint32_t,3> background{};
	std::vector<GridBlock> blocks;
};

struct GridFrame {
	std::vector<GridChannel> channels;
};

// Supply channels sorted by name and blocks by xyz origin. The budget includes
// input, old output, candidate, validation scratch and logical expansion.
// Subtract the rest of the process working set before calling. Failure leaves
// output untouched. No dependency on an external volume library.
bool EncodeGrid(const GridFrame& frame, std::size_t budget,
	std::vector<std::uint8_t>& out, std::string& error);
bool DecodeGrid(const std::vector<std::uint8_t>& bytes, std::size_t budget,
	GridFrame& out, std::string& error);

} }
#endif
