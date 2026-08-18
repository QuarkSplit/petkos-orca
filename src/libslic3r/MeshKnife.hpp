#ifndef libslic3r_MeshKnife_hpp_
#define libslic3r_MeshKnife_hpp_

#include <utility>
#include <vector>

#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r { namespace MeshKnife {

//A knife stroke is a plane. The loop the user sees is where that plane crosses the surface,
//and the cut itself is the slicer's own horizontal cut - the code that has split meshes at a
//layer boundary for a decade - reached by rotating the world until the knife lies flat.
//Nothing here corefines, repairs or booleans; a plane through a manifold mesh yields two
//manifold meshes and there is nothing left over to fix.

// Where the plane {x : normal . (x - point) = 0} crosses the surface: one segment per crossed
// triangle, ready to draw. Unordered - a drawing does not need the loop chained.
std::vector<std::pair<Vec3f, Vec3f>> contour(const indexed_triangle_set &its,
                                             const Vec3f                &point,
                                             const Vec3f                &normal);

struct CutResult
{
    indexed_triangle_set upper; // the side the normal points to, capped
    indexed_triangle_set lower; // the other side, capped
    // True when the plane actually divided the mesh; false leaves one side holding
    // everything and the other empty.
    bool cut{false};
};

CutResult planar_cut(const indexed_triangle_set &its, const Vec3f &point, const Vec3f &normal);

}} // namespace Slic3r::MeshKnife

#endif // libslic3r_MeshKnife_hpp_
