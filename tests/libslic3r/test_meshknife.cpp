#include <catch2/catch_all.hpp>

#include "libslic3r/MeshKnife.hpp"
#include "libslic3r/MeshSeparator.hpp" // signed_volume
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

TEST_CASE("A horizontal knife cut through a cube yields two watertight boxes", "[MeshKnife]") {
    const auto its = its_make_cube(10., 10., 10.);

    const auto result = MeshKnife::planar_cut(its, Vec3f(5.f, 5.f, 4.f), Vec3f(0.f, 0.f, 1.f));
    REQUIRE(result.cut);
    CHECK(its_num_open_edges(result.upper) == 0);
    CHECK(its_num_open_edges(result.lower) == 0);
    CHECK(MeshSeparator::signed_volume(result.upper) == Catch::Approx(600.0).epsilon(1e-4));
    CHECK(MeshSeparator::signed_volume(result.lower) == Catch::Approx(400.0).epsilon(1e-4));
}

TEST_CASE("An oblique knife cut conserves volume and stays watertight", "[MeshKnife]") {
    const auto its = its_make_cube(10., 10., 10.);

    const Vec3f normal = Vec3f(1.f, 1.f, 1.f).normalized();
    const auto  result = MeshKnife::planar_cut(its, Vec3f(5.f, 5.f, 5.f), normal);
    REQUIRE(result.cut);
    CHECK(its_num_open_edges(result.upper) == 0);
    CHECK(its_num_open_edges(result.lower) == 0);
    const double sum = MeshSeparator::signed_volume(result.upper) + MeshSeparator::signed_volume(result.lower);
    CHECK(sum == Catch::Approx(1000.0).epsilon(1e-4));
    // The plane through the centre along (1,1,1) halves the cube by symmetry.
    CHECK(MeshSeparator::signed_volume(result.upper) == Catch::Approx(500.0).epsilon(1e-3));
}

TEST_CASE("A plane that misses the mesh divides nothing", "[MeshKnife]") {
    const auto its = its_make_cube(10., 10., 10.);

    const auto result = MeshKnife::planar_cut(its, Vec3f(5.f, 5.f, 20.f), Vec3f(0.f, 0.f, 1.f));
    REQUIRE_FALSE(result.cut);
    // Everything sits below a plane at z=20, so the lower half holds the whole cube.
    CHECK(result.upper.indices.empty());
    CHECK(MeshSeparator::signed_volume(result.lower) == Catch::Approx(1000.0).epsilon(1e-6));
}

TEST_CASE("The contour lies on the plane and measures the section perimeter", "[MeshKnife]") {
    const auto its = its_make_cube(10., 10., 10.);

    const Vec3f point(5.f, 5.f, 5.f), normal(0.f, 0.f, 1.f);
    const auto  segments = MeshKnife::contour(its, point, normal);
    REQUIRE_FALSE(segments.empty());

    double perimeter = 0.;
    for (const auto &[a, b] : segments) {
        CHECK(std::abs(a.z() - 5.f) < 1e-5f);
        CHECK(std::abs(b.z() - 5.f) < 1e-5f);
        perimeter += (b - a).norm();
    }
    // The section of a 10 mm cube at half height is a 10x10 square.
    CHECK(perimeter == Catch::Approx(40.0).epsilon(1e-4));
}
