#ifndef libslic3r_MeshSeparator_hpp_
#define libslic3r_MeshSeparator_hpp_

#include <string>
#include <vector>

#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r { namespace MeshSeparator {

//A paint bucket for solids. Click a face, and every face reachable from it without crossing a
//sharp edge is the region; detach the region and close both openings with one shared patch, so
//the two parts mate exactly and each is watertight on its own.
//
//No boolean is involved anywhere. The surface triangles are the ones that went in, bit for bit;
//the only new geometry is the seam patch, and both parts carry the same patch wound opposite
//ways. The whole operation is one breadth-first pass over face adjacency plus one triangulation
//per seam loop.

struct FillParams
{
    // An edge is crossable while the faces meeting at it disagree by no more than this.
    float angle_threshold_deg{45.f};
    //When set, a CONVEX crease never bounds the region - only a valley does. Every edge of a
    //cube is sharp, but a body does not end at its own corner; the join between an arm and a
    //torso is concave. Off, the rule is the plain paint bucket: any sharp edge is a wall.
    bool  concave_only{false};
};

// Faces reachable from `seed_face`, one flag per face. Crossability is a property of the edge
// alone, so the region is the connected component containing the seed: any seed inside a
// region reproduces that region.
std::vector<char> flood_fill(const indexed_triangle_set &its,
                             const std::vector<Vec3i32> &face_neighbors,
                             const std::vector<Vec3f>   &face_normals,
                             int                         seed_face,
                             const FillParams           &params);

struct Part
{
    indexed_triangle_set mesh;
    size_t               surface_faces{0}; // faces carried over from the source, unchanged
    size_t               cap_faces{0};     // faces added to seal the seam
};

struct Result
{
    Part   region; // the selection
    Part   rest;   // everything else
    size_t seam_loops{0};    // closed seam loops, each sealed with one shared patch
    size_t open_chains{0};   // seam runs that met a hole or non-manifold edge; left open
    size_t fallback_fans{0}; // loops whose ear clipping stalled and finished as a centre fan
    double volume_source{0.};
    double volume_region{0.};
    double volume_rest{0.};
    std::string error; // set when separate() returns false
};

//Split the mesh into the selected region and the rest, sealing the seam. Returns false with
//`error` set when there is nothing meaningful to detach: an empty or total selection, a flat
//selection (a region with no thickness is a sticker, not a part), or a failed conservation
//check - the parts must add up to the source, and a split that cannot prove that is refused
//rather than shipped.
bool separate(const indexed_triangle_set &its,
              const std::vector<Vec3i32> &face_neighbors,
              const std::vector<char>    &selected,
              Result                     &out);

// Signed volume in double. its_volume() accumulates in float, which on a large mesh loses
// more than a cut does; conservation is only checkable in double.
double signed_volume(const indexed_triangle_set &its);

}} // namespace Slic3r::MeshSeparator

#endif // libslic3r_MeshSeparator_hpp_
