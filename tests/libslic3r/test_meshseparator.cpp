#include <catch2/catch_all.hpp>

#include "libslic3r/MeshSeparator.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

//A boss on a plinth: 20x20x4 plate with an 8x8x8 block standing on it, one closed manifold
//mesh. The square where the boss meets the plate is the only concave seam, which makes this
//the canonical separation case: volumes 1600 + 512 = 2112.
static indexed_triangle_set make_boss_on_plinth()
{
    indexed_triangle_set its;
    its.vertices = {
        // plinth bottom (z=0)
        {0.f, 0.f, 0.f}, {20.f, 0.f, 0.f}, {20.f, 20.f, 0.f}, {0.f, 20.f, 0.f},
        // plinth top outer rim (z=4)
        {0.f, 0.f, 4.f}, {20.f, 0.f, 4.f}, {20.f, 20.f, 4.f}, {0.f, 20.f, 4.f},
        // boss base = plinth top inner rim (z=4)
        {6.f, 6.f, 4.f}, {14.f, 6.f, 4.f}, {14.f, 14.f, 4.f}, {6.f, 14.f, 4.f},
        // boss top (z=12)
        {6.f, 6.f, 12.f}, {14.f, 6.f, 12.f}, {14.f, 14.f, 12.f}, {6.f, 14.f, 12.f},
    };
    its.indices = {
        // plinth bottom, facing -Z                              faces 0-1
        {0, 3, 2}, {0, 2, 1},
        // plinth sides                                          faces 2-9
        {0, 1, 5}, {0, 5, 4}, {1, 2, 6}, {1, 6, 5},
        {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7},
        // plinth top annulus around the boss, facing +Z         faces 10-17
        {4, 5, 9}, {4, 9, 8}, {5, 6, 10}, {5, 10, 9},
        {6, 7, 11}, {6, 11, 10}, {7, 4, 8}, {7, 8, 11},
        // boss sides                                            faces 18-25
        {8, 9, 13}, {8, 13, 12}, {9, 10, 14}, {9, 14, 13},
        {10, 11, 15}, {10, 15, 14}, {11, 8, 12}, {11, 12, 15},
        // boss top, facing +Z                                   faces 26-27
        {12, 13, 14}, {12, 14, 15},
    };
    return its;
}

TEST_CASE("The plain paint bucket stops at every sharp edge of a cube", "[MeshSeparator]") {
    const auto its       = its_make_cube(10., 10., 10.);
    const auto neighbors = its_face_neighbors(its);
    const auto normals   = its_face_normals(its);

    MeshSeparator::FillParams params;
    params.angle_threshold_deg = 45.f;
    auto sel = MeshSeparator::flood_fill(its, neighbors, normals, 0, params);
    // The only crossable edge from any cube facet is the coplanar diagonal of its own quad.
    const size_t count = std::count(sel.begin(), sel.end(), char(1));
    REQUIRE(count == 2);

    // Open the threshold past 90 degrees and the fill takes the whole cube.
    params.angle_threshold_deg = 91.f;
    sel = MeshSeparator::flood_fill(its, neighbors, normals, 0, params);
    REQUIRE(std::count(sel.begin(), sel.end(), char(1)) == long(its.indices.size()));
}

TEST_CASE("Concave-only mode walks over convex corners", "[MeshSeparator]") {
    const auto its       = its_make_cube(10., 10., 10.);
    const auto neighbors = its_face_neighbors(its);
    const auto normals   = its_face_normals(its);

    MeshSeparator::FillParams params;
    params.angle_threshold_deg = 45.f;
    params.concave_only        = true;
    // Every edge of a cube is sharp, and every one is convex: a body does not end at its own
    // corner, so the fill must take the whole cube.
    const auto sel = MeshSeparator::flood_fill(its, neighbors, normals, 0, params);
    REQUIRE(std::count(sel.begin(), sel.end(), char(1)) == long(its.indices.size()));
}

TEST_CASE("The boss is a region and any seed inside it finds the same region", "[MeshSeparator]") {
    const auto its       = make_boss_on_plinth();
    const auto neighbors = its_face_neighbors(its);
    const auto normals   = its_face_normals(its);
    REQUIRE(its_num_open_edges(neighbors) == 0);

    MeshSeparator::FillParams params;
    params.angle_threshold_deg = 45.f;
    params.concave_only        = true;

    const auto from_side = MeshSeparator::flood_fill(its, neighbors, normals, 18, params);
    // 8 side triangles + 2 top triangles; the concave seam at the base is the wall.
    REQUIRE(std::count(from_side.begin(), from_side.end(), char(1)) == 10);
    for (int f = 18; f < 28; ++f)
        REQUIRE(from_side[f] == 1);

    // Crossability belongs to the edge, so the region is an equivalence class of faces.
    const auto from_top = MeshSeparator::flood_fill(its, neighbors, normals, 27, params);
    REQUIRE(from_top == from_side);
}

TEST_CASE("Separating the boss yields two watertight parts that add up", "[MeshSeparator]") {
    const auto its       = make_boss_on_plinth();
    const auto neighbors = its_face_neighbors(its);
    const auto normals   = its_face_normals(its);

    MeshSeparator::FillParams params;
    params.angle_threshold_deg = 45.f;
    params.concave_only        = true;
    const auto sel = MeshSeparator::flood_fill(its, neighbors, normals, 18, params);

    MeshSeparator::Result result;
    REQUIRE(MeshSeparator::separate(its, neighbors, sel, result));

    CHECK(result.seam_loops == 1);
    CHECK(result.open_chains == 0);
    CHECK(result.fallback_fans == 0);
    CHECK(result.region.surface_faces == 10);
    // One square loop, spanned once, worn by both parts: two triangles each.
    CHECK(result.region.cap_faces == 2);
    CHECK(result.rest.cap_faces == 2);

    // Watertight by construction - and since the neighbor index only pairs edges running in
    // opposite directions, zero open edges also proves the caps are wound consistently.
    CHECK(its_num_open_edges(result.region.mesh) == 0);
    CHECK(its_num_open_edges(result.rest.mesh) == 0);

    //The volume of the region must be the BOSS volume, not merely a number that makes the sum
    //work: the sum is conserved under any cap winding (the shared patch cancels), so this is
    //the assertion that actually pins the orientation down.
    CHECK(result.volume_region == Catch::Approx(512.0).epsilon(1e-9));
    CHECK(result.volume_rest == Catch::Approx(1600.0).epsilon(1e-9));
    CHECK(result.volume_source == Catch::Approx(2112.0).epsilon(1e-9));
    CHECK(std::abs(result.volume_region + result.volume_rest - result.volume_source) < 1e-6);
}

TEST_CASE("A flat selection is refused - a sticker is not a part", "[MeshSeparator]") {
    const auto its       = make_boss_on_plinth();
    const auto neighbors = its_face_neighbors(its);

    // The plinth-top annulus: eight coplanar faces around the boss.
    std::vector<char> sel(its.indices.size(), 0);
    for (int f = 10; f < 18; ++f)
        sel[f] = 1;

    MeshSeparator::Result result;
    REQUIRE_FALSE(MeshSeparator::separate(its, neighbors, sel, result));
    CHECK(result.error.find("flat") != std::string::npos);
}

TEST_CASE("Empty and total selections are refused", "[MeshSeparator]") {
    const auto its       = its_make_cube(10., 10., 10.);
    const auto neighbors = its_face_neighbors(its);

    MeshSeparator::Result result;
    std::vector<char> none(its.indices.size(), 0);
    REQUIRE_FALSE(MeshSeparator::separate(its, neighbors, none, result));

    std::vector<char> all(its.indices.size(), 1);
    REQUIRE_FALSE(MeshSeparator::separate(its, neighbors, all, result));
}

TEST_CASE("A smooth sphere floods whole and so has nothing to detach", "[MeshSeparator]") {
    const auto its       = its_make_sphere(10., 2. * PI / 90.);
    const auto neighbors = its_face_neighbors(its);
    const auto normals   = its_face_normals(its);

    MeshSeparator::FillParams params;
    params.angle_threshold_deg = 15.f;
    const auto sel = MeshSeparator::flood_fill(its, neighbors, normals, 0, params);
    REQUIRE(std::count(sel.begin(), sel.end(), char(1)) == long(its.indices.size()));

    MeshSeparator::Result result;
    REQUIRE_FALSE(MeshSeparator::separate(its, neighbors, sel, result));
}
