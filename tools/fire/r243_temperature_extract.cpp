// Read-only scalar extraction for a NON-PHYSICAL DIAGNOSTIC, never a sim producer.
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv)
{
    try {
        if (argc != 2) throw std::runtime_error("expected one VDB path");
        openvdb::initialize();
        openvdb::io::File file(argv[1]);
        file.open();
        auto grid = openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("temperature"));
        if (!grid) throw std::runtime_error("temperature must be FloatGrid");
        const auto box = grid->evalActiveVoxelBoundingBox();
        const auto shape = box.dim();
        const auto spacing = grid->voxelSize();
        const auto origin = grid->indexToWorld(box.min());
        if (box.min() != openvdb::Coord(0) || shape != openvdb::Coord(69,69,106) ||
            !grid->transform().isLinear() || spacing.x() != spacing.y() || spacing.y() != spacing.z() ||
            origin != openvdb::Vec3d(0) || !std::isfinite(spacing.x()) || spacing.x() <= 0)
            throw std::runtime_error("not the pinned r241 scalar lattice");
        // JSON header, followed by native little-endian float32, x-fastest.
        std::cout.precision(17);
        std::cout << "{\"shape_xyz\":[69,69,106],\"dx_m\":" << spacing.x()
                  << ",\"index_origin_world\":[0,0,0],\"dtype\":\"<f4\"}\n";
        const auto accessor = grid->getConstAccessor();
        for (int z=0; z<shape.z(); ++z)
            for (int y=0; y<shape.y(); ++y)
                for (int x=0; x<shape.x(); ++x) {
                    const float value = accessor.getValue(openvdb::Coord(x,y,z));
                    if (!std::isfinite(value) || value<0) throw std::runtime_error("invalid temperature");
                    std::cout.write(reinterpret_cast<const char*>(&value), sizeof(value));
                }
        if (!std::cout) throw std::runtime_error("output failed");
        file.close();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
