#include <catch2/catch_all.hpp>

#include <libslic3r/BuildVolume.hpp>
#include <libslic3r/TriangleMesh.hpp>

using namespace Slic3r;

// PETKO'S ORCA: "hangs over the plate edge" and "taller than the machine prints" are two faults
// with two different fixes, and the slicer used to report them as one sentence because one test
// answered both. The split is made by asking the same test twice, the second time with the
// height ceiling lifted: whatever it still rejects is a footprint fault. These cases pin that
// down, because the whole message the user reads depends on it being exact.

static BuildVolume rectangular_bed(double width, double depth, double height)
{
    const std::vector<Vec2d> area { { 0., 0. }, { width, 0. }, { width, depth }, { 0., depth } };
    return BuildVolume(area, height, {}, {});
}

// A cube of the given size with its minimum corner at (x, y, z).
static BoundingBoxf3 box_at(const Vec3d& min, const Vec3d& size)
{
    return BoundingBoxf3(min, min + size);
}

TEST_CASE("Build volume tells a footprint fault from a height fault", "[BuildVolume]")
{
    const BuildVolume bed = rectangular_bed(200., 200., 100.);

    SECTION("an object that fits is inside either way")
    {
        const BoundingBoxf3 bb = box_at({ 50., 50., 0. }, { 20., 20., 20. });
        REQUIRE(bed.volume_state_bbox(bb) == BuildVolume::ObjectState::Inside);
        REQUIRE(bed.volume_state_bbox(bb, true, true) == BuildVolume::ObjectState::Inside);
    }

    SECTION("an object over the edge stays rejected when the ceiling is lifted")
    {
        // Half on, half off the right hand edge, and nowhere near the height limit.
        const BoundingBoxf3 bb = box_at({ 190., 50., 0. }, { 20., 20., 20. });
        REQUIRE(bed.volume_state_bbox(bb) == BuildVolume::ObjectState::Colliding);
        REQUIRE(bed.volume_state_bbox(bb, true, true) == BuildVolume::ObjectState::Colliding);
    }

    SECTION("an object that is only too tall is accepted when the ceiling is lifted")
    {
        // Comfortably within the plate, but 20 mm above a 100 mm limit.
        const BoundingBoxf3 bb = box_at({ 50., 50., 0. }, { 20., 20., 120. });
        REQUIRE(bed.volume_state_bbox(bb) == BuildVolume::ObjectState::Colliding);
        REQUIRE(bed.volume_state_bbox(bb, true, true) == BuildVolume::ObjectState::Inside);
    }

    SECTION("an object that is both keeps failing both tests")
    {
        const BoundingBoxf3 bb = box_at({ 190., 50., 0. }, { 20., 20., 120. });
        REQUIRE(bed.volume_state_bbox(bb) == BuildVolume::ObjectState::Colliding);
        REQUIRE(bed.volume_state_bbox(bb, true, true) == BuildVolume::ObjectState::Colliding);
    }

    SECTION("a bed with no declared height already ignores the ceiling")
    {
        const BuildVolume     unlimited = rectangular_bed(200., 200., 0.);
        const BoundingBoxf3   bb        = box_at({ 50., 50., 0. }, { 20., 20., 5000. });
        REQUIRE(unlimited.volume_state_bbox(bb) == BuildVolume::ObjectState::Inside);
        REQUIRE(unlimited.volume_state_bbox(bb, true, true) == BuildVolume::ObjectState::Inside);
    }
}

TEST_CASE("The same split holds for the mesh test", "[BuildVolume]")
{
    const BuildVolume bed = rectangular_bed(200., 200., 100.);

    auto state_of = [&bed](const Vec3f& translation, const Vec3d& size, bool ignore_height) {
        const indexed_triangle_set its   = its_make_cube(size.x(), size.y(), size.z());
        Transform3f                trafo = Transform3f::Identity();
        trafo.translate(translation);
        return bed.object_state(its, trafo, false, true, ignore_height);
    };

    SECTION("over the edge is a footprint fault whether or not the ceiling is there")
    {
        REQUIRE(state_of({ 190.f, 50.f, 0.f }, { 20., 20., 20. }, false) == BuildVolume::ObjectState::Colliding);
        REQUIRE(state_of({ 190.f, 50.f, 0.f }, { 20., 20., 20. }, true) == BuildVolume::ObjectState::Colliding);
    }

    SECTION("too tall is a height fault only")
    {
        REQUIRE(state_of({ 50.f, 50.f, 0.f }, { 20., 20., 120. }, false) == BuildVolume::ObjectState::Colliding);
        REQUIRE(state_of({ 50.f, 50.f, 0.f }, { 20., 20., 120. }, true) == BuildVolume::ObjectState::Inside);
    }

    SECTION("a fitting object is untouched by the extra argument")
    {
        REQUIRE(state_of({ 50.f, 50.f, 0.f }, { 20., 20., 20. }, false) == BuildVolume::ObjectState::Inside);
        REQUIRE(state_of({ 50.f, 50.f, 0.f }, { 20., 20., 20. }, true) == BuildVolume::ObjectState::Inside);
    }
}
