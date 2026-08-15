#include <catch2/catch_all.hpp>
#include "test_utils.hpp"

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/Model.hpp>

using namespace Slic3r;

TEST_CASE("CGAL and TriangleMesh conversions", "[MeshBoolean]") {
    TriangleMesh sphere = make_sphere(1.);
    
    auto cgalmesh_ptr = MeshBoolean::cgal::triangle_mesh_to_cgal(sphere);
    
    REQUIRE(cgalmesh_ptr);
    REQUIRE(! MeshBoolean::cgal::does_self_intersect(*cgalmesh_ptr));
    
    TriangleMesh M = MeshBoolean::cgal::cgal_to_triangle_mesh(*cgalmesh_ptr);
    
    REQUIRE(M.its.vertices.size() == sphere.its.vertices.size());
    REQUIRE(M.its.indices.size() == sphere.its.indices.size());
    
    REQUIRE(M.volume() == Catch::Approx(sphere.volume()));

    REQUIRE(! MeshBoolean::cgal::does_self_intersect(M));
}

// ------------------------------------------------------------------------------------------
// The robustness ladder, and the two backends' habit of answering with something that is not
// the operation they were asked for.
// ------------------------------------------------------------------------------------------

static indexed_triangle_set cube_at(double x, double size = 10.0)
{
    indexed_triangle_set its = its_make_cube(size, size, size);
    for (Vec3f &v : its.vertices)
        v.x() += float(x);
    return its;
}

// A solid built by dropping one mesh on top of another without a boolean: the two shells
// interpenetrate, so the result self-intersects. CGAL is configured with
// throw_on_self_intersection(true) and refuses it, which is exactly the input needed to ask
// what a refusal costs the caller.
static indexed_triangle_set self_intersecting()
{
    indexed_triangle_set a = cube_at(0.0);
    its_merge(a, cube_at(5.0));
    return a;
}

TEST_CASE("A failed CGAL boolean leaves its input alone", "[MeshBoolean]")
{
    const indexed_triangle_set bad = self_intersecting();
    REQUIRE(MeshBoolean::cgal::does_self_intersect(TriangleMesh(bad)));

    //The CGALMesh level is where the input used to be destroyed: the result was moved into A
    //before success was ever checked, so a boolean that failed handed the next rung of the
    //ladder an empty mesh and there was nothing left to retry with.
    auto A = MeshBoolean::cgal::triangle_mesh_to_cgal(bad);
    auto B = MeshBoolean::cgal::triangle_mesh_to_cgal(cube_at(2.0));
    REQUIRE(A);
    REQUIRE(B);
    REQUIRE_THROWS(MeshBoolean::cgal::minus(*A, *B));
    CHECK(!MeshBoolean::cgal::empty(*A));

    //And the same contract one level up, where the callers actually live.
    indexed_triangle_set kept = bad;
    const size_t vertices_before = kept.vertices.size();
    const size_t indices_before  = kept.indices.size();
    REQUIRE_THROWS(MeshBoolean::cgal::minus(kept, cube_at(2.0)));
    CHECK(kept.vertices.size() == vertices_before);
    CHECK(kept.indices.size() == indices_before);
}

TEST_CASE("The boolean ladder answers with a result or a reason", "[MeshBoolean]")
{
    indexed_triangle_set a = cube_at(0.0);
    const float source_volume = its_volume(a);

    // Two overlapping cubes: the exact rung answers, and it says which one it was.
    MeshBoolean::LadderResult res = MeshBoolean::execute(MeshBoolean::Op::Union, a, cube_at(5.0));
    CHECK(res.ok);
    CHECK(!res.empty_result);
    CHECK(std::string(res.backend) == "cgal");
    CHECK(its_volume(a) > source_volume);

    // An empty operand is a refusal with a reason, and the caller's mesh survives it.
    indexed_triangle_set b = cube_at(0.0);
    const size_t before = b.indices.size();
    MeshBoolean::LadderResult none = MeshBoolean::execute(MeshBoolean::Op::Difference, b, indexed_triangle_set());
    CHECK(!none.ok);
    CHECK(!none.reason.empty());
    CHECK(b.indices.size() == before);

    // An intersection that is genuinely nothing is a RESULT, not a failure - the ladder says
    // so rather than sending a correct answer down to a less exact backend.
    indexed_triangle_set c = cube_at(0.0);
    MeshBoolean::LadderResult apart = MeshBoolean::execute(MeshBoolean::Op::Intersection, c, cube_at(1000.0));
    CHECK(apart.ok);
    CHECK(apart.empty_result);
}

TEST_CASE("MCUT refuses an operation it does not have, instead of guessing", "[MeshBoolean]")
{
    //The op string used to be looked up and dereferenced without checking the lookup found
    //anything, so an unknown operation read past the end of the table and the flags that came
    //back decided which boolean the user got.
    auto a = MeshBoolean::mcut::triangle_mesh_to_mcut(cube_at(0.0));
    auto b = MeshBoolean::mcut::triangle_mesh_to_mcut(cube_at(5.0));
    REQUIRE(a);
    REQUIRE(b);

    const TriangleMesh before = MeshBoolean::mcut::mcut_to_triangle_mesh(*a);
    const MeshBoolean::mcut::Status s = MeshBoolean::mcut::do_boolean_single(*a, *b, "NOT_AN_OPERATION");
    CHECK(s == MeshBoolean::mcut::Status::UnsupportedOp);
    CHECK(!MeshBoolean::mcut::succeeded(s));
    CHECK(std::string(MeshBoolean::mcut::to_string(s)).size() > 0);

    const TriangleMesh after = MeshBoolean::mcut::mcut_to_triangle_mesh(*a);
    CHECK(after.its.indices.size() == before.its.indices.size());
}

TEST_CASE("MCUT never answers a union with a concatenation it did not name", "[MeshBoolean]")
{
    //A dispatch failure used to be answered, for UNION only, by standing the two meshes next
    //to each other and returning true. Two interpenetrating shells are not a union, and the
    //caller had no way to find out. The only concatenation left is the one that is a theorem:
    //bounding boxes that do not overlap cannot intersect, so their union IS the two meshes
    //side by side - and even that case is reported as DisjointUnion rather than as Success.
    const indexed_triangle_set near_cube = cube_at(0.0);
    const indexed_triangle_set far_cube  = cube_at(1000.0);

    auto a = MeshBoolean::mcut::triangle_mesh_to_mcut(near_cube);
    auto b = MeshBoolean::mcut::triangle_mesh_to_mcut(far_cube);
    const MeshBoolean::mcut::Status s = MeshBoolean::mcut::do_boolean_single(*a, *b, "UNION");

    //Whichever way MCUT decides to answer disjoint input, the answer is named. What must not
    //happen is a plain Success carrying a mesh that is two shells stapled together.
    if (s == MeshBoolean::mcut::Status::DisjointUnion) {
        const TriangleMesh out = MeshBoolean::mcut::mcut_to_triangle_mesh(*a);
        CHECK(out.its.vertices.size() == near_cube.vertices.size() + far_cube.vertices.size());
    }
    else {
        CHECK(s != MeshBoolean::mcut::Status::Success);
    }

    // And an unsupported op reaches the assembled entry point as a refusal that produces no mesh.
    std::vector<TriangleMesh> dst;
    const MeshBoolean::mcut::Status bogus =
        MeshBoolean::mcut::make_boolean(TriangleMesh(near_cube), TriangleMesh(cube_at(5.0)), dst, "NOT_AN_OPERATION");
    CHECK(bogus == MeshBoolean::mcut::Status::UnsupportedOp);
    CHECK(dst.empty());
}

TEST_CASE("A refused model boolean does not cost the object its geometry", "[MeshBoolean]")
{
    //ModelObject::make_boolean cleared its volumes before it ever looked at the result, so a
    //refused MCUT dispatch emptied the object and reported success.
    Model model;
    ModelObject *src = model.add_object();
    src->add_volume(TriangleMesh(cube_at(0.0)));
    ModelObject *tool = model.add_object();
    tool->add_volume(TriangleMesh(cube_at(5.0)));

    REQUIRE(src->volumes.size() == 1);
    CHECK(!src->make_boolean(tool, "NOT_AN_OPERATION"));
    CHECK(src->volumes.size() == 1);
    CHECK(!src->volumes.front()->mesh().empty());
}
