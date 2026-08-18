//Podslicer: separating the parts a model is only pretending to have.
//
//The claim under test is not "it produced something" but three arithmetic facts, because the
//whole argument for cutting this way rather than with a boolean is that the arithmetic is
//exact: both parts close, their volumes add up to the source's, and every painted facet lands
//on the part its face landed on. A test that only checked for output would pass on a cut that
//quietly loses a fifth of the model, which is the failure this replaces.
#include <catch2/catch_all.hpp>

#include <libslic3r/CutRegion.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleSelector.hpp>

using namespace Slic3r;

// ------------------------------------------------------------------------------------------
// fixtures
// ------------------------------------------------------------------------------------------

// Two cubes far apart in one mesh: one surface, two bodies, no cut needed to separate them.
static indexed_triangle_set two_loose_cubes()
{
    indexed_triangle_set a = its_make_cube(10., 10., 10.);
    indexed_triangle_set b = its_make_cube(10., 10., 10.);
    const int            base = int(a.vertices.size());
    for (Vec3f &v : b.vertices)
        v.x() += 100.f;
    a.vertices.insert(a.vertices.end(), b.vertices.begin(), b.vertices.end());
    for (const stl_triangle_vertex_indices &t : b.indices)
        a.indices.emplace_back(t[0] + base, t[1] + base, t[2] + base);
    return a;
}

// its_volume accumulates in float; the claim here is about millimetres of material, so it is
// computed the way the claim is made.
static double exact_volume(const indexed_triangle_set &its)
{
    double v = 0.;
    for (const stl_triangle_vertex_indices &t : its.indices) {
        const Vec3d a = its.vertices[size_t(t[0])].cast<double>();
        const Vec3d b = its.vertices[size_t(t[1])].cast<double>();
        const Vec3d c = its.vertices[size_t(t[2])].cast<double>();
        v += a.dot(b.cross(c));
    }
    return v / 6.;
}

// Split a mesh by which side of a plane each face's centre falls on. An arbitrary labelling on
// purpose: the split has to be right for any labelling, not only for ones it can rationalise.
static std::vector<int> label_by_height(const indexed_triangle_set &its, float z)
{
    std::vector<int> labels(its.indices.size(), 0);
    for (size_t f = 0; f < its.indices.size(); ++f) {
        const stl_triangle_vertex_indices &t = its.indices[f];
        const float mid = (its.vertices[size_t(t[0])].z() + its.vertices[size_t(t[1])].z() + its.vertices[size_t(t[2])].z()) / 3.f;
        labels[f] = mid > z ? 1 : 0;
    }
    return labels;
}

// ------------------------------------------------------------------------------------------

TEST_CASE("A face partition closes both parts and conserves volume", "[CutRegion]")
{
    const indexed_triangle_set cube   = its_make_cube(10., 10., 10.);
    const double               source = exact_volume(cube);
    REQUIRE(source == Catch::Approx(1000.).epsilon(1e-9));

    const std::vector<int> labels = label_by_height(cube, 5.f);
    // The labelling has to actually divide something, or the test proves nothing.
    REQUIRE(std::count(labels.begin(), labels.end(), 1) > 0);
    REQUIRE(std::count(labels.begin(), labels.end(), 0) > 0);

    CutRegionResult result;
    std::string     failure;
    REQUIRE(split_by_labels(cube, labels, 2, true, result, failure));
    REQUIRE(failure.empty());
    REQUIRE(result.parts.size() == 2);
    REQUIRE(result.cut_loops > 0);
    CHECK(result.unspanned_loops == 0);

    double sum = 0.;
    for (const CutRegionPart &part : result.parts) {
        REQUIRE_FALSE(part.mesh.indices.empty());
        // Watertight, by construction rather than by repair.
        CHECK(its_num_open_edges(part.mesh) == 0);
        CHECK(part.cap_faces > 0);
        const double v = exact_volume(part.mesh);
        INFO("faces=" << part.mesh.indices.size() << " caps=" << part.cap_faces << " volume=" << v
             << " loops=" << result.cut_loops);
        CHECK(v > 0.);
        sum += v;
    }
    // The two lids are the same surface wound opposite ways, so this is exact, not close.
    CHECK(sum == Catch::Approx(source).epsilon(1e-9));
    CHECK(result.volume_conserved);
    CHECK(result.parts_volume == Catch::Approx(result.source_volume).epsilon(1e-9));

    //This fixture is the awkward case on purpose: every vertex of the cube lies on the cut
    //outline, so the cheapest surface spanning that outline is the cube's own upper skin. A
    //lid made of the model's own faces is the failure that produces one part holding the
    //whole model and one holding nothing, and the fan is what refuses it.
    CHECK(result.fanned_loops == 1);
}

TEST_CASE("Loose shells separate with no lid at all", "[CutRegion]")
{
    const indexed_triangle_set two = two_loose_cubes();
    const std::vector<Vec3i32> nb  = its_face_neighbors_par(two);

    RegionFillParams params;
    std::vector<int> labels;
    const int        count = label_regions_by_crease(two, nb, params, labels);
    REQUIRE(count == 2);

    CutRegionResult result;
    std::string     failure;
    REQUIRE(split_by_labels(two, labels, count, true, result, failure));
    REQUIRE(result.parts.size() == 2);
    // Nothing was joined, so nothing had to be closed.
    CHECK(result.cut_loops == 0);
    for (const CutRegionPart &part : result.parts) {
        CHECK(part.cap_faces == 0);
        CHECK(its_num_open_edges(part.mesh) == 0);
        CHECK(exact_volume(part.mesh) == Catch::Approx(1000.).epsilon(1e-9));
    }
}

TEST_CASE("A convex corner is not a seam", "[CutRegion]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    const std::vector<Vec3i32> nb   = its_face_neighbors_par(cube);

    // Every edge of a cube is a 90 degree convex corner. A body does not end at a corner, so
    // asking for parts should find one part - the cube.
    RegionFillParams concave;
    concave.crease_angle_deg = 30.f;
    concave.concave_only     = true;
    std::vector<int> labels;
    CHECK(label_regions_by_crease(cube, nb, concave, labels) == 1);

    // With the concavity test off it is the paint bucket's behaviour instead: one flat face.
    RegionFillParams sharp = concave;
    sharp.concave_only     = false;
    const std::vector<char> face = fill_region_from_face(cube, nb, 0, sharp);
    CHECK(std::count(face.begin(), face.end(), 1) == 2);
}

TEST_CASE("Painted facets follow their faces exactly", "[CutRegion]")
{
    indexed_triangle_set cube = its_make_cube(10., 10., 10.);

    // Paint the upper half one filament, by whole facets, the way a bucket fill would.
    std::vector<EnforcerBlockerType> states(cube.indices.size(), EnforcerBlockerType::NONE);
    const std::vector<int>           labels = label_by_height(cube, 5.f);
    for (size_t f = 0; f < states.size(); ++f)
        if (labels[f] == 1)
            states[f] = EnforcerBlockerType::Extruder3;
    const auto painting = TriangleSelector::painting_from_facet_states(states);
    REQUIRE_FALSE(painting.bitstream.empty());

    // What the split hands back: which source face became which face of each part.
    CutRegionResult result;
    std::string     failure;
    REQUIRE(split_by_labels(cube, labels, 2, true, result, failure));

    for (int label = 0; label < 2; ++label) {
        const CutRegionPart &part = result.parts[size_t(label)];
        std::vector<int>     src_to_dst(cube.indices.size(), -1);
        for (size_t f = 0; f < part.src_face.size(); ++f)
            if (part.src_face[f] >= 0)
                src_to_dst[size_t(part.src_face[f])] = int(f);

        const auto carried = TriangleSelector::remap_painting_by_facet_map(painting, src_to_dst);

        if (label == 0) {
            // Nothing painted was in this part, so nothing arrives - not "nearly nothing".
            CHECK(carried.bitstream.empty());
            continue;
        }

        REQUIRE_FALSE(carried.bitstream.empty());
        // Read it back through the same door the model reads it through.
        TriangleMesh     part_mesh(part.mesh);
        TriangleSelector sel(part_mesh);
        sel.deserialize(carried, true);
        const std::vector<EnforcerBlockerType> back = sel.get_facet_states();
        REQUIRE(back.size() == part.mesh.indices.size());

        size_t painted = 0, wrong = 0;
        for (size_t f = 0; f < back.size(); ++f) {
            const bool is_cap = part.src_face[f] < 0;
            if (is_cap) {
                // A lid is new material, and new material is not painted.
                if (back[f] != EnforcerBlockerType::NONE)
                    ++wrong;
                continue;
            }
            if (back[f] == EnforcerBlockerType::Extruder3)
                ++painted;
            else
                ++wrong;
        }
        CHECK(wrong == 0);
        CHECK(painted == part.src_face.size() - part.cap_faces);
    }
}

TEST_CASE("A whole mesh survives a round trip through the facet-state encoding", "[CutRegion]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);

    std::vector<EnforcerBlockerType> states(cube.indices.size(), EnforcerBlockerType::NONE);
    // Every representable filament, including the ones that need the long encoding.
    for (size_t f = 0; f < states.size(); ++f)
        states[f] = EnforcerBlockerType(1 + int(f % size_t(EnforcerBlockerType::ExtruderMax)));

    const auto       data = TriangleSelector::painting_from_facet_states(states);
    TriangleMesh     mesh(cube);
    TriangleSelector sel(mesh);
    sel.deserialize(data, true);
    CHECK(sel.get_facet_states() == states);
}

TEST_CASE("Repairing a mesh that is not broken costs nothing and changes nothing", "[CutRegion]")
{
    TriangleMesh mesh(its_make_cube(10., 10., 10.));
    const size_t faces  = mesh.its.indices.size();
    const double before = exact_volume(mesh.its);

    std::string        error;
    RepairedMeshErrors errs;
    REQUIRE(MeshBoolean::cgal::repair(mesh, &errs, &error));
    CHECK(error.empty());
    CHECK(mesh.its.indices.size() == faces);
    CHECK(exact_volume(mesh.its) == Catch::Approx(before).epsilon(1e-9));
    CHECK_FALSE(errs.repaired());
}

TEST_CASE("A cancelled repair leaves the mesh alone and says nothing went wrong", "[CutRegion]")
{
    // A mesh with a hole, so the fast path does not answer for it.
    indexed_triangle_set open_cube = its_make_cube(10., 10., 10.);
    open_cube.indices.pop_back();
    open_cube.indices.pop_back();

    TriangleMesh mesh(open_cube);
    const size_t faces = mesh.its.indices.size();

    std::string error = "not empty to start with";
    CHECK_FALSE(MeshBoolean::cgal::repair(mesh, nullptr, &error, [](const char *, int) { return false; }));
    // An empty error is what tells the caller this was a cancel rather than a failure.
    CHECK(error.empty());
    CHECK(mesh.its.indices.size() == faces);
}

TEST_CASE("A hole gets closed and the solid comes back", "[CutRegion]")
{
    indexed_triangle_set open_cube = its_make_cube(10., 10., 10.);
    // Both triangles of one face, so the hole is a square rather than a sliver.
    open_cube.indices.pop_back();
    open_cube.indices.pop_back();
    REQUIRE(its_num_open_edges(open_cube) > 0);

    TriangleMesh mesh(open_cube);
    std::string  error;
    REQUIRE(MeshBoolean::cgal::repair(mesh, nullptr, &error));
    CHECK(error.empty());
    CHECK(its_num_open_edges(mesh.its) == 0);
    CHECK(std::abs(exact_volume(mesh.its)) == Catch::Approx(1000.).epsilon(1e-6));
}
