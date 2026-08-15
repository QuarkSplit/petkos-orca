#include "TriangleMeshDeal.hpp"

#include <igl/loop.h>
#undef NDEBUG
#include <assert.h>
#include <boost/log/trivial.hpp>

//The refusals below are read by a person in a dialog, so they are translated where they are
//written rather than reassembled from an enum at the call site.
#include "I18N.hpp"

namespace Slic3r {

//VALIDATE THE INPUT, BECAUSE igl::loop DOES NOT.
//
//This function used to set ok = true before doing anything, map Eigen views straight onto
//mesh.its and call igl::loop, with a comment asking whether validation was "really necessary".
//It is: loop() assumes a manifold, correctly indexed triangle soup. Given anything else it
//produces geometry that is not a subdivision of the input - the "nonsense" this feature was
//reported for - and given an empty mesh it dereferences its[0] before the first check.
//
//So each precondition is tested and named. A refusal here is a message the user can act on
//(repair the mesh, or subdivide something smaller); the alternative was a mangled model and a
//success return.
static bool validate_for_subdivision(const TriangleMesh &mesh, std::string &error)
{
    const size_t vertices_count = mesh.its.vertices.size();
    const size_t indices_count  = mesh.its.indices.size();

    if (vertices_count == 0 || indices_count == 0) {
        //Also the crash: the Eigen::Map below is built from &its.vertices[0], which is out of
        //bounds on an empty mesh long before anything asks whether the mesh is subdividable.
        error = _u8L("the mesh has no triangles");
        return false;
    }

    //The result is four triangles per input triangle. Refusing on the OUTPUT size is what
    //makes the limit mean something the user can predict from the number they can see.
    if (indices_count * 4 > TriangleMeshDeal::max_subdivided_facets) {
        error = _u8L("subdividing it would produce more than ") +
                std::to_string(TriangleMeshDeal::max_subdivided_facets / 1000000) +
                _u8L(" million triangles");
        return false;
    }

    //Loop subdivision is defined on a manifold surface. On an open or self-intersecting mesh
    //the edge-adjacency it builds is ambiguous, and what comes back is not the same shape.
    if (!mesh.stats().manifold()) {
        error = _u8L("the mesh is not watertight") + " (" + std::to_string(mesh.stats().open_edges) + " " +
                _u8L("open edge(s)") + ")";
        return false;
    }

    //An index outside the vertex array is an out-of-bounds read inside igl, not a bad result.
    for (const auto &triangle : mesh.its.indices)
        for (int i = 0; i < 3; ++i)
            if (triangle[i] < 0 || (size_t) triangle[i] >= vertices_count) {
                error = _u8L("the mesh has triangles that point at vertices it does not have");
                return false;
            }

    return true;
}

TriangleMesh TriangleMeshDeal::smooth_triangle_mesh(const TriangleMesh& mesh, bool& ok, std::string *error)
{
    ok = false;
    std::string reason;
    if (!validate_for_subdivision(mesh, reason)) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": refused to subdivide: " << reason;
        if (error != nullptr)
            *error = reason;
        return TriangleMesh();
    }

    using namespace igl;
    typedef Eigen::Matrix<float, Eigen::Dynamic, 3, Eigen::DontAlign | Eigen::RowMajor> RowMatrixX3f;
    typedef Eigen::Matrix<int, Eigen::Dynamic, 3, Eigen::DontAlign | Eigen::RowMajor>   RowMatrixX3i;

    const size_t vertices_count = mesh.its.vertices.size();
    const size_t indices_count  = mesh.its.indices.size();
    // Use Map to map the vertices and indicies into Matrixes without requiring a copy.
    const Eigen::Map<const RowMatrixX3f> OV(mesh.its.vertices[0].data(), vertices_count, 3);
    const Eigen::Map<const RowMatrixX3i> OF(mesh.its.indices[0].data(), indices_count, 3);
    Eigen::MatrixX3f                     V;
    Eigen::MatrixX3i                     F;

    //igl throws on the cases its own asserts catch. Letting that escape into a menu handler
    //takes the application down over a mesh the user could have been told about.
    try {
        loop(OV, OF, V, F);
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": subdivision failed: " << ex.what();
        if (error != nullptr)
            *error = _u8L("the subdivision could not be computed for this mesh");
        return TriangleMesh();
    }

    //THE ANSWER IS THE RESULT, NOT THE FACT THAT THE CALL RETURNED. A subdivision that produced
    //nothing, or that did not quadruple the facet count, is not a subdivision.
    if (V.rows() == 0 || F.rows() == 0 || (size_t) F.rows() != indices_count * 4) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": subdivision produced " << F.rows()
                                 << " triangle(s) from " << indices_count << ", which is not a refinement";
        if (error != nullptr)
            *error = _u8L("the subdivision did not produce a refinement of this mesh");
        return TriangleMesh();
    }

    indexed_triangle_set its;
    auto                 iterv = V.rowwise();
    auto                 iterf = F.rowwise();
    its.vertices.assign(iterv.cbegin(), iterv.cend());
    its.indices.assign(iterf.cbegin(), iterf.cend());
    TriangleMesh result_mesh(its);
    if (result_mesh.empty()) {
        if (error != nullptr)
            *error = _u8L("the subdivided mesh could not be built");
        return TriangleMesh();
    }

    ok = true;
    return result_mesh;
}
} // namespace Slic3r
