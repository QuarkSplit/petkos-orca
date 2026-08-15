#ifndef libslic3r_Timer_hpp_
#define libslic3r_Timer_hpp_

#include <string>

#include "TriangleMesh.hpp"

namespace Slic3r {
class TriangleMeshDeal
{
public:
    //PetkosOrca: the largest triangle count this is willing to PRODUCE. Loop subdivision
    //quadruples the facet count, so an input over a quarter of this is refused by name rather
    //than left to exhaust memory or to hand back a model nothing downstream can carry.
    static constexpr size_t max_subdivided_facets = 4000000;

    //Subdivide (Loop) and return the refined mesh.
    //
    //ok is the ANSWER, not a decoration. It used to be assigned true unconditionally before any
    //work happened, next to a comment wondering whether validation was necessary - so every
    //caller's failure branch was unreachable and a mesh igl::loop could not handle came back as
    //a success. It is now false whenever the returned mesh is not a valid subdivision of the
    //input, and error - when a caller supplies one - says which of the reasons it was, in the
    //user's terms, so the message can name the part and the limit rather than say "errors".
    static TriangleMesh smooth_triangle_mesh(const TriangleMesh &mesh, bool &ok, std::string *error = nullptr);
};
} // namespace Slic3r

#endif // libslic3r_Timer_hpp_
