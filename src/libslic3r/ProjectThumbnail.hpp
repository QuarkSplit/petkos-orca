#pragma once

#include "Color.hpp"
#include "GCode/ThumbnailData.hpp"
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
class Model;

namespace ProjectThumbnail {
constexpr unsigned int image_size = 512;

struct Colours {
    std::vector<std::string> filaments;
    // Keys are zero-based object/instance and object/volume indices respectively.
    std::map<std::pair<size_t, size_t>, std::vector<std::string>> instances;
    std::map<std::pair<size_t, size_t>, std::vector<RGBA>> triangles;
};

// Pure CPU rendering of the supplied model. Does not access a GUI or change the model.
// Throws on empty, malformed or excessively large geometry.
ThumbnailData render(const Model &model, const Colours &colours = {});

// Imports must run in an isolated helper process: Model IDs and STEP state are shared
// within the application. All importer scratch files stay below scratch_directory.
bool generate(const std::string &input, const std::string &output,
              const std::string &scratch_directory, std::string &error);
} // namespace ProjectThumbnail
} // namespace Slic3r
