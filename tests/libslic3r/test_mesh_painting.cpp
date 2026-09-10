#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/QuadricEdgeCollapse.hpp"
#include "libslic3r/TriangleMeshDeal.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <algorithm>
#include <numeric>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

TriangleSelector::TriangleSplittingData detailed_painting(size_t faces)
{
    auto data = TriangleSelector::painting_from_facet_states(
        std::vector<EnforcerBlockerType>(faces, EnforcerBlockerType::Extruder3));
    // Four children on the first facet, including unpainted material and a long-encoded slot.
    std::vector<bool> detail {true, true, false, false};
    for (EnforcerBlockerType state : {EnforcerBlockerType::Extruder16, EnforcerBlockerType::NONE,
                                      EnforcerBlockerType::Extruder2, EnforcerBlockerType::Extruder4}) {
        if (state == EnforcerBlockerType::NONE)
            detail.insert(detail.end(), {false, false, false, false});
        else {
            const auto leaf = TriangleSelector::painting_from_facet_states({state});
            detail.insert(detail.end(), leaf.bitstream.begin(), leaf.bitstream.end());
        }
    }
    const int extra = int(detail.size()) - 8;
    data.bitstream.erase(data.bitstream.begin(), data.bitstream.begin() + 8);
    data.bitstream.insert(data.bitstream.begin(), detail.begin(), detail.end());
    for (size_t i = 1; i < data.triangles_to_split.size(); ++i)
        data.triangles_to_split[i].bitstream_start_idx += extra;
    data.reset_used_states();
    data.update_used_states(0);
    return data;
}

void check_same_paint(const TriangleSelector::TriangleSplittingData &actual,
                      const TriangleSelector::TriangleSplittingData &expected)
{
    REQUIRE(actual.bitstream == expected.bitstream);
    REQUIRE(actual.triangles_to_split.size() == expected.triangles_to_split.size());
    for (size_t i = 0; i < actual.triangles_to_split.size(); ++i) {
        CHECK(actual.triangles_to_split[i].triangle_idx == expected.triangles_to_split[i].triangle_idx);
        CHECK(actual.triangles_to_split[i].bitstream_start_idx == expected.triangles_to_split[i].bitstream_start_idx);
    }
}

double area(const indexed_triangle_set &mesh)
{
    double total = 0.;
    for (const Vec3i32 &face : mesh.indices)
        total += 0.5 * (mesh.vertices[face[1]] - mesh.vertices[face[0]]).cross(mesh.vertices[face[2]] - mesh.vertices[face[0]]).norm();
    return total;
}

TriangleSelector::TriangleSplittingData split_painting(int sides, int special,
                                                     std::initializer_list<EnforcerBlockerType> states)
{
    TriangleSelector::TriangleSplittingData result;
    result.triangles_to_split.emplace_back(0, 0);
    const int code = sides | (special << 2);
    for (int bit = 0; bit < 4; ++bit) result.bitstream.push_back((code & (1 << bit)) != 0);
    for (EnforcerBlockerType state : states) {
        if (state == EnforcerBlockerType::NONE)
            result.bitstream.insert(result.bitstream.end(), 4, false);
        else {
            const auto leaf = TriangleSelector::painting_from_facet_states({state});
            result.bitstream.insert(result.bitstream.end(), leaf.bitstream.begin(), leaf.bitstream.end());
        }
    }
    result.reset_used_states();
    result.update_used_states(0);
    return result;
}

Vec3d surface_moment(const indexed_triangle_set &mesh)
{
    Vec3d moment = Vec3d::Zero();
    for (const Vec3i32 &face : mesh.indices) {
        const Vec3d a = mesh.vertices[face[0]].cast<double>();
        const Vec3d b = mesh.vertices[face[1]].cast<double>();
        const Vec3d c = mesh.vertices[face[2]].cast<double>();
        moment += 0.5 * (b - a).cross(c - a).norm() * (a + b + c) / 3.;
    }
    return moment;
}

} // namespace

TEST_CASE("Repeated face reordering keeps the complete paint bitstream", "[MeshPainting]")
{
    const auto source = detailed_painting(12);
    std::vector<int> reverse(12);
    std::iota(reverse.rbegin(), reverse.rend(), 0);
    const auto once = TriangleSelector::remap_painting_by_facet_map(source, reverse);
    for (size_t i = 1; i < once.triangles_to_split.size(); ++i)
        CHECK(once.triangles_to_split[i - 1].bitstream_start_idx < once.triangles_to_split[i].bitstream_start_idx);
    check_same_paint(TriangleSelector::remap_painting_by_facet_map(once, reverse), source);

    SECTION("Already saved out-of-order bit ranges are recovered") {
        auto legacy = source;
        for (auto &entry : legacy.triangles_to_split)
            entry.triangle_idx = reverse[entry.triangle_idx];
        std::sort(legacy.triangles_to_split.begin(), legacy.triangles_to_split.end(), [](const auto &a, const auto &b) {
            return a.triangle_idx < b.triangle_idx;
        });
        check_same_paint(TriangleSelector::remap_painting_by_facet_map(legacy, reverse), source);
    }
}

TEST_CASE("Saving a boolean input includes its material without replacing painted detail", "[MeshPainting]")
{
    Model model;
    ModelObject *object = model.add_object();
    object->config.set("extruder", 5);
    ModelVolume *part = object->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    const auto paint = detailed_painting(part->mesh().its.indices.size());
    part->mmu_segmentation_facets.set_data(TriangleSelector::TriangleSplittingData(paint));
    part->seam_facets.set_data(TriangleSelector::painting_from_facet_states({EnforcerBlockerType::ENFORCER}));

    const auto saved = part->save_painting(true);
    REQUIRE(saved.has_value());
    TriangleSelector selector(saved->mesh);
    selector.deserialize(saved->mmu);
    CHECK(selector.has_facets(EnforcerBlockerType::Extruder5));
    CHECK(selector.has_facets(EnforcerBlockerType::Extruder16));
    CHECK(selector.has_facets(EnforcerBlockerType::Extruder4));
    CHECK_FALSE(selector.has_facets(EnforcerBlockerType::NONE));
    check_same_paint(part->mmu_segmentation_facets.get_data(), paint);
    check_same_paint(saved->seam, part->seam_facets.get_data());
}

TEST_CASE("Merge keeps transformed parts paint material and negative volumes", "[MeshPainting]")
{
    Model model;
    ModelObject *object = model.add_object();
    object->config.set("extruder", 2);
    object->add_instance();
    ModelVolume *first = object->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    ModelVolume *second = object->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    second->translate(Vec3d(30., 2., 0.));
    second->config.set("extruder", 5);
    ModelVolume *negative = object->add_volume(TriangleMesh(its_make_cube(2., 2., 2.)), ModelVolumeType::NEGATIVE_VOLUME);
    const auto negative_id = negative->id();
    const auto paint = detailed_painting(first->mesh().its.indices.size());
    first->mmu_segmentation_facets.set_data(TriangleSelector::TriangleSplittingData(paint));
    first->supported_facets.set_data(TriangleSelector::painting_from_facet_states({EnforcerBlockerType::BLOCKER}));
    const auto support = first->supported_facets.get_data();
    const auto bounds = object->raw_mesh().bounding_box();

    object->merge();

    REQUIRE(object->volumes.size() == 2);
    CHECK(negative->id() == negative_id);
    CHECK(negative->type() == ModelVolumeType::NEGATIVE_VOLUME);
    const ModelVolume *merged = *std::find_if(object->volumes.begin(), object->volumes.end(), [](const ModelVolume *v) {
        return v->is_model_part();
    });
    const auto after = object->raw_mesh().bounding_box();
    for (int axis = 0; axis < 3; ++axis) {
        CHECK_THAT(after.min[axis], WithinAbs(bounds.min[axis], 1e-5));
        CHECK_THAT(after.max[axis], WithinAbs(bounds.max[axis], 1e-5));
    }
    REQUIRE(merged->mesh().its.indices.size() == 24);
    std::vector<int> first_faces(24, -1);
    std::iota(first_faces.begin(), first_faces.begin() + 12, 0);
    check_same_paint(TriangleSelector::remap_painting_by_facet_map(merged->mmu_segmentation_facets.get_data(), first_faces), paint);
    check_same_paint(merged->supported_facets.get_data(), support);
    TriangleSelector selector(merged->mesh());
    selector.deserialize(merged->mmu_segmentation_facets.get_data());
    const auto states = selector.get_facet_states();
    for (size_t face = 12; face < states.size(); ++face)
        CHECK(states[face] == EnforcerBlockerType::Extruder5);
}

TEST_CASE("Simplification carries annotations onto displaced surfaces", "[MeshPainting]")
{
    Model model;
    ModelVolume *volume = model.add_object()->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    const auto paint = TriangleSelector::painting_from_facet_states(
        std::vector<EnforcerBlockerType>(12, EnforcerBlockerType::Extruder3));
    volume->mmu_segmentation_facets.set_data(TriangleSelector::TriangleSplittingData(paint));
    const auto saved = volume->save_painting();
    auto simplified = volume->mesh().its;
    for (auto &vertex : simplified.vertices)
        vertex *= 0.9f;
    volume->set_mesh(std::move(simplified));
    volume->restore_painting(saved, false, TriangleSelector::PaintingRemapMode::NearestSurface);
    check_same_paint(volume->mmu_segmentation_facets.get_data(), paint);
}

TEST_CASE("Merge preserves outward winding and intersecting paint channels on mirrored parts", "[MeshPainting]")
{
    Model model;
    ModelObject *object = model.add_object();
    object->config.set("extruder", 1);
    object->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    ModelVolume *reflected = object->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    reflected->translate(Vec3d(30., 0., 0.));
    reflected->mirror(X);
    reflected->config.set("extruder", 5);
    reflected->supported_facets.set_data(split_painting(2, 0,
        {EnforcerBlockerType::ENFORCER, EnforcerBlockerType::NONE, EnforcerBlockerType::BLOCKER}));
    reflected->seam_facets.set_data(split_painting(1, 1,
        {EnforcerBlockerType::BLOCKER, EnforcerBlockerType::ENFORCER}));
    reflected->mmu_segmentation_facets.set_data(split_painting(3, 0,
        {EnforcerBlockerType::Extruder16, EnforcerBlockerType::NONE,
         EnforcerBlockerType::Extruder2, EnforcerBlockerType::Extruder4}));
    reflected->fuzzy_skin_facets.set_data(split_painting(2, 1,
        {EnforcerBlockerType::BLOCKER, EnforcerBlockerType::ENFORCER, EnforcerBlockerType::NONE}));
    const auto saved = reflected->save_painting(true);
    REQUIRE(saved);
    const Transform3d original_transform = reflected->get_matrix();
    const std::array<const TriangleSelector::TriangleSplittingData *, 4> before {
        &saved->supported, &saved->seam, &saved->mmu, &saved->fuzzy };

    object->merge();

    REQUIRE(object->volumes.size() == 1);
    const ModelVolume *merged = object->volumes[0];
    CHECK_THAT(its_volume(merged->mesh().its), WithinAbs(2000., 0.01));
    CHECK_THAT(area(merged->mesh().its), WithinAbs(1200., 0.01));
    CHECK(its_num_open_edges(merged->mesh().its) == 0);
    const std::array<const FacetsAnnotation *, 4> after {
        &merged->supported_facets, &merged->seam_facets, &merged->mmu_segmentation_facets, &merged->fuzzy_skin_facets };
    for (size_t channel = 0; channel < before.size(); ++channel) {
        INFO("channel " << channel);
        TriangleSelector source(saved->mesh), target(merged->mesh());
        source.deserialize(*before[channel]);
        target.deserialize(after[channel]->get_data());
        for (int slot = 1; slot <= int(EnforcerBlockerType::ExtruderMax); ++slot) {
            INFO("state " << slot);
            const auto state = static_cast<EnforcerBlockerType>(slot);
            auto expected = source.get_facets(state);
            auto actual = target.get_facets(state);
            its_transform(expected, original_transform);
            its_transform(actual, merged->get_matrix());
            CHECK_THAT(area(actual), WithinAbs(area(expected), 0.001));
            const Vec3d actual_moment = surface_moment(actual);
            const Vec3d expected_moment = surface_moment(expected);
            for (int axis = 0; axis < 3; ++axis)
                CHECK_THAT(actual_moment[axis], WithinAbs(expected_moment[axis], 0.01));
        }
    }
}

TEST_CASE("Simplification retains painted and unpainted areas across a new triangulation", "[MeshPainting]")
{
    indexed_triangle_set source;
    source.vertices = {{0.f, 0.f, 0.f}, {4.f, 0.f, 0.f}, {4.f, 4.f, 0.f}, {0.f, 4.f, 0.f}};
    source.indices = {{0, 1, 2}, {0, 2, 3}};
    auto target = source;
    target.indices = {{0, 1, 3}, {1, 2, 3}};
    for (Vec3f &vertex : target.vertices)
        vertex.z() = 0.5f;
    const auto paint = TriangleSelector::painting_from_facet_states({EnforcerBlockerType::Extruder3, EnforcerBlockerType::NONE});
    const auto projected = TriangleSelector::remap_painting(source, paint, target, Transform3d::Identity(), {},
        TriangleSelector::PaintingRemapMode::NearestSurface);
    TriangleMesh mesh(target);
    TriangleSelector selector(mesh);
    selector.deserialize(projected);
    CHECK_THAT(area(selector.get_facets(EnforcerBlockerType::Extruder3)), WithinAbs(8., 0.2));
    CHECK_THAT(area(selector.get_facets(EnforcerBlockerType::NONE)), WithinAbs(8., 0.2));
}

TEST_CASE("An unchanged mesh retains subfacet painting exactly", "[MeshPainting]")
{
    const auto mesh = its_make_cube(10., 10., 10.);
    const auto paint = detailed_painting(mesh.indices.size());
    const auto projected = TriangleSelector::remap_painting(mesh, paint, mesh, Transform3d::Identity(), {},
        TriangleSelector::PaintingRemapMode::NearestSurface);
    check_same_paint(projected, paint);
}

TEST_CASE("In-place mesh replacement does not apply the import centering shift to paint twice", "[MeshPainting]")
{
    Model model;
    ModelVolume *volume = model.add_object()->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    REQUIRE_FALSE(volume->mesh().get_init_shift().isZero());
    const auto paint = detailed_painting(volume->mesh().its.indices.size());
    volume->mmu_segmentation_facets.set_data(TriangleSelector::TriangleSplittingData(paint));
    volume->set_mesh_preserving_paint(volume->mesh());
    check_same_paint(volume->mmu_segmentation_facets.get_data(), paint);
}

TEST_CASE("A replacement volume maps paint through the relative part placement", "[MeshPainting]")
{
    Model model;
    ModelObject *object = model.add_object();
    ModelVolume *source = object->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    const auto paint = detailed_painting(source->mesh().its.indices.size());
    source->mmu_segmentation_facets.set_data(TriangleSelector::TriangleSplittingData(paint));
    TriangleMesh replacement = source->mesh();
    replacement.translate(30.f, 0.f, 0.f);
    ModelVolume *target = object->add_volume(std::move(replacement), ModelVolumeType::MODEL_PART, false);
    target->set_offset(source->get_offset() - Vec3d(30., 0., 0.));
    target->restore_painting(*source);
    check_same_paint(target->mmu_segmentation_facets.get_data(), paint);
}

TEST_CASE("Real edge collapse and Loop subdivision keep surface colour", "[MeshPainting]")
{
    Model model;
    ModelVolume *volume = model.add_object()->add_volume(make_sphere(10., PI / 8));
    volume->mmu_segmentation_facets.set_data(TriangleSelector::painting_from_facet_states(
        std::vector<EnforcerBlockerType>(volume->mesh().its.indices.size(), EnforcerBlockerType::Extruder4)));
    const size_t original_faces = volume->mesh().its.indices.size();

    SECTION("Quadric edge collapse") {
        auto simplified = volume->mesh().its;
        its_quadric_edge_collapse(simplified, 40);
        REQUIRE(simplified.indices.size() < original_faces);
        volume->set_mesh_preserving_paint(TriangleMesh(std::move(simplified)), TriangleSelector::PaintingRemapMode::NearestSurface);
    }
    SECTION("Loop subdivision") {
        bool ok = false;
        std::string error;
        auto subdivided = TriangleMeshDeal::smooth_triangle_mesh(volume->mesh(), ok, &error);
        INFO(error);
        REQUIRE(ok);
        REQUIRE(subdivided.its.indices.size() == original_faces * 4);
        volume->set_mesh_preserving_paint(std::move(subdivided), TriangleSelector::PaintingRemapMode::NearestSurface);
    }

    TriangleSelector selector(volume->mesh());
    selector.deserialize(volume->mmu_segmentation_facets.get_data());
    const auto states = selector.get_facet_states();
    REQUIRE_FALSE(states.empty());
    for (EnforcerBlockerType state : states)
        CHECK(state == EnforcerBlockerType::Extruder4);
}

TEST_CASE("Material slot remapping carries object part layer and painted assignments together", "[MeshPainting]")
{
    Model model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    object->config.set("support_filament", 2);
    volume->config.set("extruder", 0);
    volume->config.set("inner_wall_filament_id", 3);
    auto &layer = object->layer_config_ranges[{0., 2.}];
    layer.set("extruder", 4);
    layer.set("support_interface_filament", 0);
    volume->mmu_segmentation_facets.set_data(detailed_painting(12));
    volume->supported_facets.set_data(TriangleSelector::painting_from_facet_states({EnforcerBlockerType::BLOCKER}));
    const auto support = volume->supported_facets.get_data();
    CHECK(object->used_filament_ids() == std::vector<int>{1, 2, 3, 4, 16});

    std::vector<int> mapping(17);
    std::iota(mapping.begin(), mapping.end(), 0);
    mapping[1] = 5;
    mapping[2] = 3;
    mapping[3] = 2;
    mapping[4] = 6;
    mapping[16] = 7;
    object->remap_filament_ids(mapping);

    CHECK(object->config.get().option<ConfigOptionInt>("extruder")->value == 5);
    CHECK(object->config.get().option<ConfigOptionInt>("support_filament")->value == 3);
    CHECK(volume->config.get().option<ConfigOptionInt>("extruder")->value == 0);
    CHECK(volume->config.get().option<ConfigOptionInt>("inner_wall_filament_id")->value == 2);
    CHECK(layer.get().option<ConfigOptionInt>("extruder")->value == 6);
    CHECK(layer.get().option<ConfigOptionInt>("support_interface_filament")->value == 0);
    CHECK(object->used_filament_ids() == std::vector<int>{2, 3, 5, 6, 7});
    TriangleSelector selector(volume->mesh());
    selector.deserialize(volume->mmu_segmentation_facets.get_data());
    CHECK(selector.has_facets(EnforcerBlockerType::Extruder7));
    CHECK(selector.has_facets(EnforcerBlockerType::NONE));
    check_same_paint(volume->supported_facets.get_data(), support);
}

TEST_CASE("An incomplete material translation changes no assignments", "[MeshPainting]")
{
    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    object->config.set("extruder", 2);
    REQUIRE_THROWS_AS(object->remap_filament_ids({0, 3}), std::invalid_argument);
    CHECK(object->config.get().option<ConfigOptionInt>("extruder")->value == 2);
}
