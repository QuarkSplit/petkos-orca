#ifndef libslic3r_MeshBoolean_hpp_
#define libslic3r_MeshBoolean_hpp_

#include <memory>
#include <exception>

#include <libslic3r/TriangleMesh.hpp>
#include <Eigen/Geometry>

namespace Slic3r {

namespace MeshBoolean {

using EigenMesh = std::pair<Eigen::MatrixXd, Eigen::MatrixXi>;

TriangleMesh eigen_to_triangle_mesh(const EigenMesh &emesh);
EigenMesh triangle_mesh_to_eigen(const TriangleMesh &mesh);

void minus(EigenMesh &A, const EigenMesh &B);
void self_union(EigenMesh &A);
    
void minus(TriangleMesh& A, const TriangleMesh& B);
void self_union(TriangleMesh& mesh);

namespace cgal {

struct CGALMesh;
struct CGALMeshDeleter { void operator()(CGALMesh *ptr); };
using CGALMeshPtr = std::unique_ptr<CGALMesh, CGALMeshDeleter>;

CGALMeshPtr clone(const CGALMesh &m);

void save_CGALMesh(const std::string& fname, const CGALMesh& cgal_mesh);

CGALMeshPtr triangle_mesh_to_cgal(
    const std::vector<stl_vertex> &V,
    const std::vector<stl_triangle_vertex_indices> &F);

inline CGALMeshPtr triangle_mesh_to_cgal(const indexed_triangle_set &M)
{
    return triangle_mesh_to_cgal(M.vertices, M.indices);
}
inline CGALMeshPtr triangle_mesh_to_cgal(const TriangleMesh &M)
{
    return triangle_mesh_to_cgal(M.its);
}

TriangleMesh cgal_to_triangle_mesh(const CGALMesh &cgalmesh);
indexed_triangle_set cgal_to_indexed_triangle_set(const CGALMesh &cgalmesh);

// Do boolean mesh difference with CGAL bypassing igl.
void minus(TriangleMesh &A, const TriangleMesh &B);
void plus(TriangleMesh &A, const TriangleMesh &B);
void intersect(TriangleMesh &A, const TriangleMesh &B);

void minus(indexed_triangle_set &A, const indexed_triangle_set &B);
void plus(indexed_triangle_set &A, const indexed_triangle_set &B);
void intersect(indexed_triangle_set &A, const indexed_triangle_set &B);

void minus(CGALMesh &A, CGALMesh &B);
void plus(CGALMesh &A, CGALMesh &B);
void intersect(CGALMesh &A, CGALMesh &B);

bool does_self_intersect(const TriangleMesh &mesh);
bool does_self_intersect(const CGALMesh &mesh);

//BBS
std::vector<TriangleMesh> segment(const TriangleMesh& src, double smoothing_alpha = 0.5, int segment_number = 5);
TriangleMesh merge(std::vector<TriangleMesh> meshes);

bool does_bound_a_volume(const CGALMesh &mesh);
bool empty(const CGALMesh &mesh);

// Repair a mesh using CGAL. Returns true on success. Optionally returns a summary of repairs and an error string.
bool repair(TriangleMesh &mesh, RepairedMeshErrors *repaired_errors = nullptr, std::string *error = nullptr);
}

namespace mcut {
struct McutMesh;
struct McutMeshDeleter
{
    void operator()(McutMesh *ptr);
};
using McutMeshPtr = std::unique_ptr<McutMesh, McutMeshDeleter>;
bool empty(const McutMesh &mesh);

McutMeshPtr  triangle_mesh_to_mcut(const indexed_triangle_set &M);
TriangleMesh mcut_to_triangle_mesh(const McutMesh &mcutmesh);

//What one MCUT dispatch actually did. A backend that cannot compute the operation it was
//asked for must say so: answering with a mesh that is not that operation is the failure
//mode this type exists to end, because the caller cannot tell it from a result.
//DisjointUnion is the one degradation that is a theorem rather than a guess - two solids
//whose bounding boxes do not overlap cannot intersect, so their union IS the two meshes
//side by side, and concatenating them is exact.
enum class Status : int {
    Success,        // MCUT computed the requested operation
    DisjointUnion,  // the inputs provably do not overlap, so the union is their concatenation
    EmptyInput,     // one side was empty; the identity is the answer and srcMesh holds it
    UnsupportedOp,  // boolean_opts names no operation this backend has
    DispatchFailed, // MCUT refused the input
    NoFragments,    // MCUT produced nothing and the inputs may overlap, so nothing is known
};
const char *to_string(Status s);
inline bool succeeded(Status s)
{
    return s == Status::Success || s == Status::DisjointUnion || s == Status::EmptyInput;
}

// do boolean and save result to srcMesh. srcMesh is left untouched unless the status succeeded.
Status do_boolean_single(McutMesh& srcMesh, const McutMesh& cutMesh, const std::string& boolean_opts);
// do boolean of mesh with multiple volumes and save result to srcMesh
// Both srcMesh and cutMesh may have multiple volumes.
Status do_boolean(McutMesh &srcMesh, const McutMesh &cutMesh, const std::string &boolean_opts);


// do boolean and convert result to TriangleMesh
Status make_boolean(const TriangleMesh &src_mesh, const TriangleMesh &cut_mesh, std::vector<TriangleMesh> &dst_mesh, const std::string &boolean_opts);
} // namespace mcut

//The robustness ladder. Dirty downloaded meshes are the substrate, not the edge case, so
//one boolean is several backends and a named outcome rather than one backend and a throw.
//Rung 1 is CGAL (exact where it works, and it now leaves its input intact when it does
//not); rung 2 is MCUT, which tolerates input CGAL refuses. A caller gets either a result
//or a reason, never a mesh that is not the operation it asked for.
enum class Op : int { Difference, Union, Intersection };

struct LadderResult
{
    bool        ok{false};
    bool        empty_result{false}; // the operation succeeded and its answer is the empty set
    const char *backend{"none"};     // the rung that answered
    std::string reason;              // why the rungs before it did not
};

//A is replaced by the result on success and left exactly as it was handed over on failure.
LadderResult execute(Op op, indexed_triangle_set &A, const indexed_triangle_set &B);

} // namespace MeshBoolean
} // namespace Slic3r
#endif // libslic3r_MeshBoolean_hpp_
