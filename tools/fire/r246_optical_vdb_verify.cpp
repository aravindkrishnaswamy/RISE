// Read-only independent lower-layer value/mask stream for the synthetic optical audit.
// Compile: c++ -std=c++17 -O2 -Wall -Wextra -I/opt/homebrew/include
//   -L/opt/homebrew/lib r246_optical_vdb_verify.cpp -lopenvdb -ltbb -o VERIFIER
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

int main(int argc, char** argv)
{
    try {
        const bool mutate = argc == 4 && std::strcmp(argv[1], "--mutation-control") == 0;
        if (argc != 2 && !mutate) throw std::runtime_error("expected VDB or --mutation-control INPUT NEW_OUTPUT");
        openvdb::initialize();
        openvdb::io::File file(argv[mutate ? 2 : 1]);
        file.open();
        if (mutate) {
            if (std::filesystem::exists(argv[3])) throw std::runtime_error("output already exists");
            auto grids = file.getGrids();
            bool changed = false;
            for (auto& grid : *grids) if (grid->getName() == "temperature") {
                auto temperature = openvdb::gridPtrCast<openvdb::FloatGrid>(grid);
                if (!temperature) throw std::runtime_error("wrong temperature type");
                temperature->tree().setValueOn(openvdb::Coord(51,51,35), 1501.0f);
                changed = true;
            }
            if (!changed) throw std::runtime_error("missing temperature");
            openvdb::io::File output(argv[3]); output.write(*grids); output.close();
            file.close(); return 0;
        }
        std::vector<char> bytes;
        bytes.reserve(104u * 104u * 80u * 43u);
        const auto append = [&](float value) {
            std::uint32_t bits; std::memcpy(&bits, &value, sizeof(bits));
            for (unsigned int b=0; b<4; ++b) bytes.push_back(static_cast<char>(bits >> (8*b)));
        };
        for (const char* name : {"carbon", "temperature", "reaction", "chem_CH", "chem_C2", "chem_CO2"}) {
            auto grid = openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid(name));
            if (!grid) throw std::runtime_error("missing/wrong scalar grid");
            for (int z=0; z<80; ++z) for (int y=0; y<104; ++y) for (int x=0; x<104; ++x) {
                const openvdb::Coord cell(x,y,z);
                append(grid->tree().getValue(cell));
                bytes.push_back(grid->tree().isValueOn(cell) ? 1 : 0);
            }
        }
        auto grid = openvdb::gridPtrCast<openvdb::Vec3fGrid>(file.readGrid("velocity"));
        if (!grid) throw std::runtime_error("missing/wrong velocity grid");
        for (int z=1; z<=80; ++z) for (int y=1; y<=104; ++y) for (int x=1; x<=104; ++x) {
            const openvdb::Coord cell(x,y,z); const auto value = grid->tree().getValue(cell);
            for (unsigned int axis=0; axis<3; ++axis) append(value[axis]);
            bytes.push_back(grid->tree().isValueOn(cell) ? 1 : 0);
        }
        file.close();
        std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!std::cout) throw std::runtime_error("failed value stream output");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 91;
    }
}
