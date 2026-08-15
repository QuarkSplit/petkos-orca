//Stage 1 of Podslicer's cutting work: the plane that stops somewhere.
//
//Every fixture here is a solid whose exact volume is known by construction, because the
//claim being tested is arithmetic - the parts add up to the source - and a claim about
//arithmetic checked against a mesh nobody can compute the volume of is not checked at all.
//The discriminating fixture is the U: an unbounded cut through it produces THREE pieces and
//a bounded one produces two, so a test that passes for the wrong reason cannot pass here.
#include <catch2/catch_all.hpp>

#include <libslic3r/CutUtils.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleMeshSlicer.hpp>

using namespace Slic3r;

// ------------------------------------------------------------------------------------------
// fixtures
// ------------------------------------------------------------------------------------------

static indexed_triangle_set box(const Vec3d &lo, const Vec3d &hi)
{
    indexed_triangle_set its = its_make_cube(hi.x() - lo.x(), hi.y() - lo.y(), hi.z() - lo.z());
    for (Vec3f &v : its.vertices)
        v += lo.cast<float>();
    return its;
}

static indexed_triangle_set fused(const indexed_triangle_set &a, const indexed_triangle_set &b)
{
    indexed_triangle_set out = a;
    const MeshBoolean::LadderResult res = MeshBoolean::execute(MeshBoolean::Op::Union, out, b);
    REQUIRE(res.ok);
    return out;
}

// A U: a base slab with two prongs standing on it. One horizontal plane above the base
// crosses BOTH prongs, so the unbounded cut cannot help splitting the top into two.
static indexed_triangle_set make_u()
{
    indexed_triangle_set u = box({0, 0, 0}, {30, 10, 5});
    u = fused(u, box({0, 0, 4}, {10, 10, 25}));
    u = fused(u, box({20, 0, 4}, {30, 10, 25}));
    return u;
}

static double volume_of(const indexed_triangle_set &its) { return double(its_volume(its)); }

static bool run_split(const indexed_triangle_set &src, const CutBounds &bounds, const Transform3d &cut_matrix,
                      CutBoundedSplits &out, std::string &failure)
{
    CutBoundedInput in;
    in.volume_idx = 0;
    in.mesh       = std::make_shared<const TriangleMesh>(src);
    in.matrix     = Transform3d::Identity();
    return compute_bounded_splits({in}, cut_matrix, bounds, out, failure);
}

// ------------------------------------------------------------------------------------------
// the region and its cutter solid
// ------------------------------------------------------------------------------------------

TEST_CASE("A cut region knows what is inside it", "[CutBounds]")
{
    const CutBounds unbounded;
    CHECK(!unbounded.bounded());
    CHECK(unbounded.contains(Vec2d(1e6, -1e6))); // an unbounded plane contains everything

    const CutBounds rect = CutBounds::make_rectangle(Vec2d(-5, -5), Vec2d(5, 5));
    CHECK(rect.bounded());
    CHECK(rect.contains(Vec2d(0, 0)));
    CHECK(!rect.contains(Vec2d(6, 0)));

    const CutBounds disc = CutBounds::make_disc(Vec2d(0, 0), 10.0);
    CHECK(disc.contains(Vec2d(9.0, 0)));
    CHECK(!disc.contains(Vec2d(0, 10.5)));

    // A hand-drawn stroke crosses itself. union_ex() is what makes that a region rather than
    // a defect, so the point test has to answer for it too.
    const CutBounds lasso = CutBounds::make_lasso({{0, 0}, {10, 0}, {0, 10}, {10, 10}});
    CHECK(lasso.bounded());
    CHECK(lasso.contains(Vec2d(5.0, 2.0)));
}

TEST_CASE("The cutter solid is watertight for every region shape", "[CutBounds]")
{
    for (const CutBounds &b : {CutBounds::make_rectangle(Vec2d(-10, -10), Vec2d(10, 10)),
                               CutBounds::make_disc(Vec2d(0, 0), 8.0),
                               CutBounds::make_lasso({{0, 0}, {10, 0}, {0, 10}, {10, 10}})}) {
        const indexed_triangle_set prism = its_make_cut_prism(b, 20.0);
        CHECK(!prism.empty());
        CHECK(its_num_open_edges(prism) == 0);
        CHECK(volume_of(prism) > 0.0);
    }

    // A degenerate region produces nothing rather than a sliver nobody asked for.
    CHECK(its_make_cut_prism(CutBounds(), 20.0).empty());
}

// ------------------------------------------------------------------------------------------
// the bounded cut
// ------------------------------------------------------------------------------------------

TEST_CASE("A bounded rectangle cut gives two watertight parts and conserves volume", "[CutBounds]")
{
    //A 40 x 10 x 10 bar, cut at z = 5, bounded to x in [10, 30] across the full width.
    const indexed_triangle_set bar = box({0, 0, 0}, {40, 10, 10});
    const CutBounds bounds = CutBounds::make_rectangle(Vec2d(10, -5), Vec2d(30, 15));
    const Transform3d cut_matrix = Geometry::translation_transform(Vec3d(0, 0, 5));

    CutBoundedSplits splits;
    std::string      failure;
    REQUIRE(run_split(bar, bounds, cut_matrix, splits, failure));
    REQUIRE(failure.empty());
    REQUIRE(splits.size() == 1);

    const indexed_triangle_set &upper = splits.at(0).upper;
    const indexed_triangle_set &lower = splits.at(0).lower;

    CHECK(its_num_open_edges(upper) == 0);
    CHECK(its_num_open_edges(lower) == 0);

    // Volume conservation. The parts come out of one cutter solid by intersection and
    // difference, so this is exact up to tessellation.
    CHECK(volume_of(upper) + volume_of(lower) == Catch::Approx(volume_of(bar)).epsilon(1e-4));

    // And the region is what decides which side gets what: 20 x 10 x 5 above the plane.
    CHECK(volume_of(upper) == Catch::Approx(1000.0).epsilon(1e-3));
    CHECK(volume_of(lower) == Catch::Approx(3000.0).epsilon(1e-3));

    //The unbounded plane at the same height would have taken the whole slab. That it does not
    //is the entire feature, so it is asserted rather than assumed.
    indexed_triangle_set u_up, u_lo;
    indexed_triangle_set in_cut_space = bar;
    for (Vec3f &v : in_cut_space.vertices)
        v.z() -= 5.0f;
    cut_mesh(in_cut_space, 0.0f, &u_up, &u_lo);
    CHECK(volume_of(u_up) == Catch::Approx(2000.0).epsilon(1e-3));
}

TEST_CASE("A bounded cut leaves unswept geometry attached", "[CutBounds]")
{
    const indexed_triangle_set u = make_u();
    const Transform3d cut_matrix = Geometry::translation_transform(Vec3d(0, 0, 15));

    //The unbounded cut through both prongs: the top is TWO pieces, so the object becomes
    //three. This is the behaviour the bounded cut exists to replace, and it is measured here
    //rather than described.
    {
        indexed_triangle_set in_cut_space = u;
        for (Vec3f &v : in_cut_space.vertices)
            v.z() -= 15.0f;
        indexed_triangle_set up, lo;
        cut_mesh(in_cut_space, 0.0f, &up, &lo);
        CHECK(its_split(up).size() == 2);
        CHECK(its_split(lo).size() == 1);
    }

    //The same plane, bounded to the left prong only: one piece comes off, the rest of the
    //object stays whole. Two parts, not three.
    const CutBounds bounds = CutBounds::make_rectangle(Vec2d(-5, -5), Vec2d(15, 15));
    CutBoundedSplits splits;
    std::string      failure;
    REQUIRE(run_split(u, bounds, cut_matrix, splits, failure));
    REQUIRE(splits.size() == 1);

    const indexed_triangle_set &upper = splits.at(0).upper;
    const indexed_triangle_set &lower = splits.at(0).lower;

    CHECK(its_split(upper).size() == 1);
    CHECK(its_split(lower).size() == 1);
    CHECK(its_num_open_edges(upper) == 0);
    CHECK(its_num_open_edges(lower) == 0);
    CHECK(volume_of(upper) + volume_of(lower) == Catch::Approx(volume_of(u)).epsilon(1e-4));

    // 10 x 10 x 10 of prong above z = 15 is what the region swept.
    CHECK(volume_of(upper) == Catch::Approx(1000.0).epsilon(1e-3));
}

TEST_CASE("A disc region cuts a disc-shaped plug", "[CutBounds]")
{
    const indexed_triangle_set slab = box({0, 0, 0}, {40, 40, 20});
    const CutBounds bounds = CutBounds::make_disc(Vec2d(20, 20), 8.0, 128);
    const Transform3d cut_matrix = Geometry::translation_transform(Vec3d(0, 0, 10));

    CutBoundedSplits splits;
    std::string      failure;
    REQUIRE(run_split(slab, bounds, cut_matrix, splits, failure));
    REQUIRE(splits.size() == 1);

    const double expected_plug = PI * 8.0 * 8.0 * 10.0;
    CHECK(volume_of(splits.at(0).upper) == Catch::Approx(expected_plug).epsilon(0.01));
    CHECK(volume_of(splits.at(0).upper) + volume_of(splits.at(0).lower) == Catch::Approx(volume_of(slab)).epsilon(1e-4));
    CHECK(its_num_open_edges(splits.at(0).upper) == 0);
    CHECK(its_num_open_edges(splits.at(0).lower) == 0);
}

TEST_CASE("An unbounded region is refused by the precompute rather than guessed at", "[CutBounds]")
{
    const indexed_triangle_set bar = box({0, 0, 0}, {10, 10, 10});
    CutBoundedSplits splits;
    std::string      failure;
    CHECK(!run_split(bar, CutBounds(), Geometry::translation_transform(Vec3d(0, 0, 5)), splits, failure));
    CHECK(!failure.empty());
    CHECK(splits.empty());
}

TEST_CASE("A bounded cut that misses the model reports rather than deleting it", "[CutBounds]")
{
    const indexed_triangle_set bar = box({0, 0, 0}, {10, 10, 10});
    // A region far off to one side: nothing above the plane is inside it.
    const CutBounds bounds = CutBounds::make_rectangle(Vec2d(500, 500), Vec2d(510, 510));

    CutBoundedSplits splits;
    std::string      failure;
    REQUIRE(run_split(bar, bounds, Geometry::translation_transform(Vec3d(0, 0, 5)), splits, failure));
    REQUIRE(splits.size() == 1);
    // Nothing was taken, and nothing was lost: the whole bar is still on the lower side.
    CHECK(volume_of(splits.at(0).upper) == Catch::Approx(0.0).margin(1e-3));
    CHECK(volume_of(splits.at(0).lower) == Catch::Approx(volume_of(bar)).epsilon(1e-4));
}

TEST_CASE("The cut region is expressed in the plane's own frame", "[CutBounds]")
{
    //The plane is tilted, so a region in world coordinates would cut the wrong material.
    //Everything downstream reads the region in the plane's frame, and this is what says so.
    const indexed_triangle_set bar = box({-20, -5, -5}, {20, 5, 5});
    const Transform3d cut_matrix = Geometry::rotation_transform(Vec3d(0, PI / 4.0, 0));

    CutBoundedSplits splits;
    std::string      failure;
    REQUIRE(run_split(bar, CutBounds::make_rectangle(Vec2d(-50, -50), Vec2d(50, 50)), cut_matrix, splits, failure));
    REQUIRE(splits.size() == 1);
    // A region larger than the whole bar cuts exactly what the infinite plane would.
    CHECK(volume_of(splits.at(0).upper) + volume_of(splits.at(0).lower) == Catch::Approx(volume_of(bar)).epsilon(1e-4));
    CHECK(volume_of(splits.at(0).upper) > 0.0);
    CHECK(volume_of(splits.at(0).lower) > 0.0);
}
