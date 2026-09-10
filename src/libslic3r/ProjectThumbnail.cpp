#include "ProjectThumbnail.hpp"

#include "Model.hpp"
#include "Preset.hpp"
#include "Format/3mf.hpp"
#include "Format/bbs_3mf.hpp"
#include "Format/OBJ.hpp"
#include "Format/STEP.hpp"
#include "Utils.hpp"
#include "miniz_extension.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

namespace Slic3r::ProjectThumbnail {
namespace {
constexpr unsigned int render_size = image_size * 2;
constexpr size_t max_triangles = 20'000'000;
const Vec3d view = Vec3d(1., 1., 1.).normalized();
const Vec3d right = Vec3d(1., -1., 0.).normalized();
const Vec3d up = Vec3d(-1., -1., 2.).normalized();
const Vec3d light = Vec3d(0.3, 0.6, 1.).normalized();

struct Part {
    const indexed_triangle_set *mesh;
    Transform3d transform;
    ColorRGB colour;
    const std::vector<RGBA> *triangle_colours = nullptr;
};

ColorRGB filament_colour(const std::vector<std::string> &palette, int extruder)
{
    ColorRGB colour = ColorRGB::ORCA();
    if (extruder > 0 && size_t(extruder) <= palette.size())
        decode_color(palette[extruder - 1], colour);
    return colour;
}

Vec3d project(const Vec3d &point)
{
    return {right.dot(point), -up.dot(point), view.dot(point)};
}

double edge(const Vec3d &a, const Vec3d &b, double x, double y)
{
    return (b.x() - a.x()) * (y - a.y()) - (b.y() - a.y()) * (x - a.x());
}

std::vector<std::string> archive_colour_groups(mz_zip_archive &archive)
{
    struct Groups {
        std::map<int, std::string> colours;
        int current = -1;
    };
    std::map<int, std::string> colour_groups;
    for (mz_uint entry = 0; entry < mz_zip_reader_get_num_files(&archive); ++entry) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive, entry, &stat) ||
            !boost::iends_with(stat.m_filename, ".model")) continue;
        XML_Parser parser = XML_ParserCreate(nullptr);
        if (parser == nullptr) throw std::bad_alloc();
        ScopeGuard close([&] { XML_ParserFree(parser); });
        Groups groups;
        XML_SetUserData(parser, &groups);
        XML_SetElementHandler(parser, [](void *data, const char *name, const char **attributes) {
            auto &state = *static_cast<Groups *>(data);
            auto attribute = [&](const char *key) -> const char * {
                for (size_t i = 0; attributes[i] != nullptr; i += 2)
                    if (std::strcmp(attributes[i], key) == 0) return attributes[i + 1];
                return "";
            };
            if (std::strcmp(name, "m:colorgroup") == 0) state.current = std::atoi(attribute("id"));
            else if (std::strcmp(name, "m:color") == 0 && state.current >= 0)
                state.colours[state.current] = attribute("color");
        }, [](void *data, const char *name) {
            if (std::strcmp(name, "m:colorgroup") == 0) static_cast<Groups *>(data)->current = -1;
        });
        const bool extracted = mz_zip_reader_extract_to_callback(&archive, entry,
            [](void *data, mz_uint64, const void *bytes, size_t size) -> size_t {
                return XML_Parse(static_cast<XML_Parser>(data), static_cast<const char *>(bytes), int(size), false) == XML_STATUS_OK ? size : 0;
            }, parser, 0);
        if (extracted && XML_Parse(parser, nullptr, 0, true) == XML_STATUS_OK) {
            for (const auto &[id, colour] : groups.colours) {
                if (boost::iequals(stat.m_filename, "3D/3dmodel.model")) colour_groups[id] = colour;
                else colour_groups.emplace(id, colour);
            }
        }
    }
    // The model reader numbers material groups by ID and coalesces equal colours.
    std::vector<std::string> colours;
    for (const auto &[id, colour] : colour_groups)
        if (std::find(colours.begin(), colours.end(), colour) == colours.end()) colours.push_back(colour);
    return colours;
}

// Read appearance alone; importing project profiles or G-code is unnecessary for a preview.
std::vector<std::string> archive_palette(const std::string &path)
{
    mz_zip_archive archive{};
    if (!open_zip_reader(&archive, path))
        throw std::runtime_error("Could not open the 3MF archive");
    ScopeGuard close([&] { close_zip_reader(&archive); });
    const int index = mz_zip_reader_locate_file(&archive, "Metadata/project_settings.config", nullptr, 0);
    if (index < 0) return archive_colour_groups(archive);
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&archive, index, &stat) || stat.m_uncomp_size > 16 * 1024 * 1024)
        return {};
    size_t size = 0;
    std::unique_ptr<void, decltype(&mz_free)> contents(
        mz_zip_reader_extract_to_heap(&archive, index, &size, 0), &mz_free);
    if (!contents) return {};
    const auto config = nlohmann::json::parse(static_cast<const char *>(contents.get()),
        static_cast<const char *>(contents.get()) + size, nullptr, false);
    if (!config.is_object()) return {};
    const auto entry = config.find("filament_colour");
    if (entry == config.end()) return {};
    std::vector<std::string> colours;
    if (entry->is_string()) colours.push_back(entry->get<std::string>());
    else if (entry->is_array())
        for (const auto &colour : *entry)
            colours.push_back(colour.is_string() ? colour.get<std::string>() : "");
    return colours;
}

void plate_colours(const Model &model, const PlateDataPtrs &plates, Colours &colours)
{
    for (const PlateData *plate : plates) {
        if (plate == nullptr || (plate->slicing_context.filament_colours.empty() &&
            plate->slicing_context.filament_preset_names.empty())) continue;
        for (size_t object = 0; object < model.objects.size(); ++object) {
            const auto &instances = model.objects[object]->instances;
            for (size_t instance = 0; instance < instances.size(); ++instance) {
                const size_t loaded_id = instances[instance]->loaded_id;
                if (loaded_id == 0) continue;
                const bool belongs = std::any_of(plate->obj_inst_map.begin(), plate->obj_inst_map.end(),
                    [&](const auto &entry) { return entry.second.second > 0 && size_t(entry.second.second) == loaded_id; });
                if (belongs)
                    colours.instances[{object, instance}] = plate->slicing_context.filament_colours;
            }
        }
    }
}

bool same_path(const boost::filesystem::path &a, const boost::filesystem::path &b)
{
    const auto left = boost::filesystem::weakly_canonical(a).generic_string();
    const auto right_path = boost::filesystem::weakly_canonical(b).generic_string();
#ifdef _WIN32
    return boost::iequals(left, right_path);
#else
    return left == right_path;
#endif
}
} // namespace

ThumbnailData render(const Model &model, const Colours &colours)
{
    std::map<const ModelVolume *, std::vector<indexed_triangle_set>> painted;
    std::vector<Part> parts;
    size_t triangle_count = 0;
    for (size_t object_index = 0; object_index < model.objects.size(); ++object_index) {
        const ModelObject &object = *model.objects[object_index];
        for (size_t volume_index = 0; volume_index < object.volumes.size(); ++volume_index) {
            const ModelVolume &volume = *object.volumes[volume_index];
            if (!volume.is_model_part() || volume.mesh().empty()) continue;
            const bool has_paint = !volume.mmu_segmentation_facets.empty();
            if (has_paint)
                volume.mmu_segmentation_facets.get_facets(volume, painted[&volume]);
            for (size_t instance = 0; instance < std::max(size_t(1), object.instances.size()); ++instance) {
                const auto palette = colours.instances.find({object_index, instance});
                const auto &filaments = palette == colours.instances.end() ? colours.filaments : palette->second;
                const Transform3d transform = (object.instances.empty() ? Transform3d::Identity() :
                    object.instances[instance]->get_matrix()) * volume.get_matrix();
                auto append = [&](const indexed_triangle_set &mesh, int extruder, const std::vector<RGBA> *per_triangle) {
                    if (mesh.indices.size() > max_triangles - triangle_count)
                        throw std::runtime_error("The project contains too many triangles for a library preview");
                    triangle_count += mesh.indices.size();
                    parts.push_back({&mesh, transform, filament_colour(filaments, extruder), per_triangle});
                };
                if (has_paint) {
                    const auto &meshes = painted.at(&volume);
                    for (size_t material = 0; material < meshes.size(); ++material)
                        if (!meshes[material].indices.empty())
                            append(meshes[material], material == 0 ? volume.extruder_id() : int(material), nullptr);
                } else {
                    const auto per_triangle = colours.triangles.find({object_index, volume_index});
                    append(volume.mesh().its, volume.extruder_id(),
                        per_triangle == colours.triangles.end() ? nullptr : &per_triangle->second);
                }
            }
        }
    }
    if (triangle_count == 0)
        throw std::runtime_error("The file contains no solid mesh to preview");

    Vec3d minimum = Vec3d::Constant(std::numeric_limits<double>::infinity());
    Vec3d maximum = -minimum;
    for (const Part &part : parts) {
        if (!part.transform.matrix().allFinite())
            throw std::runtime_error("The model contains an invalid transformation");
        for (const auto &triangle : part.mesh->indices) {
            for (int corner = 0; corner < 3; ++corner) {
                const auto index = triangle[corner];
                if (index < 0 || size_t(index) >= part.mesh->vertices.size())
                    throw std::runtime_error("The mesh contains an invalid triangle index");
                const Vec3d point = project(part.transform * part.mesh->vertices[index].cast<double>());
                if (!point.allFinite()) throw std::runtime_error("The mesh contains a non-finite coordinate");
                minimum = minimum.cwiseMin(point);
                maximum = maximum.cwiseMax(point);
            }
        }
    }
    const Vec3d extent = maximum - minimum;
    const double span = std::max(extent.x(), extent.y());
    if (!std::isfinite(span) || span <= 1e-12)
        throw std::runtime_error("The mesh has no visible surface");
    const Vec3d centre = (minimum + maximum) * 0.5;
    const double scale = (render_size * 0.88) / span;
    std::vector<float> depth(size_t(render_size) * render_size, -std::numeric_limits<float>::infinity());
    std::vector<unsigned char> pixels(size_t(render_size) * render_size * 4, 0);
    size_t drawn = 0;
    for (const Part &part : parts) {
        for (size_t face = 0; face < part.mesh->indices.size(); ++face) {
            const auto &triangle = part.mesh->indices[face];
            std::array<Vec3d, 3> world, screen;
            for (int corner = 0; corner < 3; ++corner) {
                world[corner] = part.transform * part.mesh->vertices[triangle[corner]].cast<double>();
                screen[corner] = (project(world[corner]) - centre) * scale;
                screen[corner].x() += render_size * 0.5;
                screen[corner].y() += render_size * 0.5;
            }
            const double area = edge(screen[0], screen[1], screen[2].x(), screen[2].y());
            if (std::abs(area) < 1e-10) continue;
            Vec3d normal = (world[1] - world[0]).cross(world[2] - world[0]);
            const double normal_length = normal.norm();
            if (!std::isfinite(normal_length) || normal_length <= 1e-15) continue;
            normal /= normal_length;
            if (normal.dot(view) < 0.) normal = -normal;
            const double shade = 0.38 + 0.62 * std::max(0., normal.dot(light));
            ColorRGB colour = part.colour;
            if (part.triangle_colours != nullptr && face < part.triangle_colours->size()) {
                const auto &rgb = (*part.triangle_colours)[face];
                colour = ColorRGB(rgb[0], rgb[1], rgb[2]);
            }
            const std::array<unsigned char, 3> rgb = {
                static_cast<unsigned char>(std::clamp((colour.r() * 0.88 + 0.08) * shade * 255., 0., 255.)),
                static_cast<unsigned char>(std::clamp((colour.g() * 0.88 + 0.08) * shade * 255., 0., 255.)),
                static_cast<unsigned char>(std::clamp((colour.b() * 0.88 + 0.08) * shade * 255., 0., 255.))};
            const int xmin = std::max(0, int(std::floor(std::min({screen[0].x(), screen[1].x(), screen[2].x()}))));
            const int xmax = std::min(int(render_size) - 1, int(std::ceil(std::max({screen[0].x(), screen[1].x(), screen[2].x()}))));
            const int ymin = std::max(0, int(std::floor(std::min({screen[0].y(), screen[1].y(), screen[2].y()}))));
            const int ymax = std::min(int(render_size) - 1, int(std::ceil(std::max({screen[0].y(), screen[1].y(), screen[2].y()}))));
            for (int y = ymin; y <= ymax; ++y) {
                for (int x = xmin; x <= xmax; ++x) {
                    const double w0 = edge(screen[1], screen[2], x + 0.5, y + 0.5) / area;
                    const double w1 = edge(screen[2], screen[0], x + 0.5, y + 0.5) / area;
                    const double w2 = 1. - w0 - w1;
                    if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9) continue;
                    const float z = float(w0 * screen[0].z() + w1 * screen[1].z() + w2 * screen[2].z());
                    const size_t pixel = size_t(y) * render_size + x;
                    if (z <= depth[pixel]) continue;
                    depth[pixel] = z;
                    for (size_t channel = 0; channel < 3; ++channel) pixels[4 * pixel + channel] = rgb[channel];
                    pixels[4 * pixel + 3] = 255;
                    ++drawn;
                }
            }
        }
    }
    if (drawn == 0) throw std::runtime_error("The mesh has no visible surface");
    ThumbnailData image;
    image.set(image_size, image_size);
    for (size_t y = 0; y < image_size; ++y) {
        for (size_t x = 0; x < image_size; ++x) {
            std::array<unsigned int, 4> sum{};
            unsigned int covered = 0;
            for (size_t dy = 0; dy < 2; ++dy) {
                for (size_t dx = 0; dx < 2; ++dx) {
                    const size_t source = 4 * ((2 * y + dy) * render_size + 2 * x + dx);
                    if (pixels[source + 3] == 0) continue;
                    ++covered;
                    for (size_t channel = 0; channel < 4; ++channel) sum[channel] += pixels[source + channel];
                }
            }
            const size_t target = 4 * (y * image_size + x);
            if (covered != 0)
                for (size_t channel = 0; channel < 3; ++channel) image.pixels[target + channel] = sum[channel] / covered;
            image.pixels[target + 3] = sum[3] / 4;
        }
    }
    return image;
}

bool generate(const std::string &input, const std::string &output,
              const std::string &scratch_directory, std::string &error)
{
    error.clear();
    try {
        namespace fs = boost::filesystem;
        const fs::path source(input), target(output), scratch(scratch_directory);
        if (!fs::is_regular_file(source)) throw std::runtime_error("The source file is unavailable");
        if (same_path(source, target)) throw std::runtime_error("The thumbnail output must differ from its source");
        if (scratch.empty() || same_path(source, scratch)) throw std::runtime_error("The thumbnail scratch folder is invalid");
        if (fs::exists(target)) throw std::runtime_error("The thumbnail output already exists");
        fs::create_directories(scratch);
        if (!target.parent_path().empty()) fs::create_directories(target.parent_path());
        const std::string extension = boost::to_lower_copy(source.extension().string());
        Model model;
        Colours colours;
        PlateDataPtrs plates;
        std::vector<Preset *> presets;
        ScopeGuard release([&] {
            release_PlateData_list(plates);
            for (Preset *preset : presets) delete preset;
            model.set_backup_path("detach");
        });
        if (extension == ".3mf") {
            model.set_backup_path((scratch / "model").string());
            DynamicPrintConfig config;
            ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::EnableSilent);
            PrusaFileParser prusa;
            bool loaded = false;
            if (prusa.check_3mf_from_prusa(input)) {
                loaded = load_3mf(input.c_str(), config, substitutions, &model, false);
                if (const auto *palette = config.option<ConfigOptionStrings>("filament_colour"))
                    colours.filaments = palette->values;
            } else {
                bool is_bbl = false, is_orca = false;
                Semver version;
                loaded = load_bbs_3mf(input.c_str(), &config, &substitutions, &model, &plates, &presets,
                    &is_bbl, &is_orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::Silence);
                colours.filaments = archive_palette(input);
                plate_colours(model, plates, colours);
            }
            if (!loaded) throw std::runtime_error("The 3MF geometry could not be read");
        } else if (extension == ".stl") {
            TriangleMesh mesh;
            if (!mesh.ReadSTLFile(input.c_str(), false)) throw std::runtime_error("The STL geometry could not be read");
            model.add_object(source.stem().string().c_str(), input.c_str(), std::move(mesh));
        } else if (extension == ".obj") {
            TriangleMesh mesh;
            ObjInfo info;
            std::string message;
            if (!load_obj(input.c_str(), &mesh, info, message))
                throw std::runtime_error("The OBJ geometry could not be read");
            if (info.face_colors.size() == mesh.its.indices.size()) colours.triangles[{0, 0}] = std::move(info.face_colors);
            else if (info.vertex_colors.size() == mesh.its.vertices.size()) {
                auto &face_colours = colours.triangles[{0, 0}];
                face_colours.reserve(mesh.its.indices.size());
                for (const auto &triangle : mesh.its.indices) {
                    RGBA colour{0.f, 0.f, 0.f, 1.f};
                    for (int corner = 0; corner < 3; ++corner)
                        for (size_t channel = 0; channel < 3; ++channel)
                            colour[channel] += info.vertex_colors[triangle[corner]][channel] / 3.f;
                    face_colours.push_back(colour);
                }
            }
            model.add_object(source.stem().string().c_str(), input.c_str(), std::move(mesh));
        } else if (extension == ".step" || extension == ".stp") {
            model = Model::read_from_step(input, LoadStrategy::AddDefaultInstances,
                nullptr, nullptr, nullptr, 0.15, 0.5, false);
        } else throw std::runtime_error("This file format has no library preview renderer");
        model.add_default_instances();
        const ThumbnailData image = render(model, colours);
        size_t png_size = 0;
        std::unique_ptr<void, decltype(&mz_free)> png(tdefl_write_image_to_png_file_in_memory_ex(
            image.pixels.data(), image.width, image.height, 4, &png_size, MZ_DEFAULT_COMPRESSION, 0), &mz_free);
        if (!png || png_size == 0) throw std::runtime_error("The thumbnail PNG could not be encoded");
        boost::nowide::ofstream out(output, std::ios::binary);
        out.write(static_cast<const char *>(png.get()), png_size);
        out.close();
        if (!out) throw std::runtime_error("The thumbnail PNG could not be written");
        return true;
    } catch (const std::exception &failure) {
        error = failure.what();
        std::replace(error.begin(), error.end(), '\n', ' ');
        std::replace(error.begin(), error.end(), '\r', ' ');
        if (error.size() > 300) error.resize(300);
        return false;
    }
}
} // namespace Slic3r::ProjectThumbnail
