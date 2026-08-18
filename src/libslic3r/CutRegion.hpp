#ifndef libslic3r_CutRegion_hpp_
#define libslic3r_CutRegion_hpp_

#include <string>
#include <vector>

#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

namespace Slic3r {

//Podslicer: a cut is a partition of FACES, not a boolean of solids.
//
//The mesh already says where the parts are. A model that reads as an arm on a torso has a
//valley of triangles where the two meet; a model that has been painted has the colours the
//user chose sitting on the faces. Both are a labelling of the existing faces, and once the
//faces are labelled the split is bookkeeping: move each face to its part, then close the
//openings that leaves.
//
//Everything good follows from not resampling the geometry. The surface is bit-for-bit the
//surface that went in, so nothing needs repairing afterwards; face identity survives, so
//painted colour is carried by an index remap rather than searched for again by proximity;
//and the cost is one pass over the faces rather than an exact-arithmetic corefinement, which
//is the difference between a cut you wait for and a cut that has already happened.
//
//What this cannot do is cut where there is no edge - a plane through the middle of a
//triangle is a different operation and belongs to the plane cutter.

// ---- labelling the faces -----------------------------------------------------------------

struct RegionFillParams
{
    // Stop the fill where two neighbouring faces turn away from each other by more than this.
    float crease_angle_deg{30.f};
    //Only a CONCAVE crease is a seam. A convex sharp edge is the corner of a box, and stopping
    //there gives you one face of a cube instead of the cube - which is right for painting a
    //surface and wrong for separating a body. The valley where an arm meets a torso is
    //concave, and so is every other join a model pretends is a join.
    bool  concave_only{true};
    // Never cross a hole in the surface: an open edge is already a boundary.
    bool  stop_at_open_edges{true};
};

// Faces reachable from `seed_face` without crossing a seam. Result is one flag per face.
std::vector<char> fill_region_from_face(const indexed_triangle_set &its,
                                        const std::vector<Vec3i32> &face_neighbors,
                                        int                         seed_face,
                                        const RegionFillParams     &params);

//The whole surface cut into regions at once, by the same rule: every face gets a label and
//no face is left over. This is what "show me the parts this model is pretending to have"
//means, and it is a single pass of the fill above repeated from unlabelled seeds.
int label_regions_by_crease(const indexed_triangle_set &its,
                            const std::vector<Vec3i32> &face_neighbors,
                            const RegionFillParams     &params,
                            std::vector<int>           &labels_out);

//The user has already told us where the parts are if the model is painted: the colours are
//the labelling. Label 0 is the unpainted default. `states_out`, when given, receives the
//painted state that each label stands for, so a caller can name the parts by filament.
int label_regions_by_paint(const indexed_triangle_set                    &its,
                           const TriangleSelector::TriangleSplittingData &painting,
                           std::vector<int>                              &labels_out,
                           std::vector<EnforcerBlockerType>              *states_out = nullptr);

//The painted surface, re-expressed as a triangulation the paint boundary is an EDGE of.
//
//Painting subdivides facets, and this hands back the result of that subdivision as one
//conforming mesh - same surface, same geometry, more triangles - where every triangle wears a
//single colour. The line between two colours is then a loop of real edges, so cutting along it
//is a choice of triangles rather than an intersection problem. `source_face` maps each face
//back to the face of the original mesh it was subdivided from, which is what carries the other
//painted channels across.
struct PaintedRegions
{
    indexed_triangle_set             mesh;
    std::vector<int>                 labels;       // one per face of `mesh`
    std::vector<int>                 source_face;  // one per face of `mesh`, into the original
    std::vector<EnforcerBlockerType> label_states; // what colour each label stands for
    int                              label_count{0};
};

// False when the volume carries no paint, which is not an error - there is simply nothing to
// separate by colour.
bool regions_from_paint(const indexed_triangle_set                    &its,
                        const TriangleSelector::TriangleSplittingData &painting,
                        PaintedRegions                                &out);

// ---- performing the split ----------------------------------------------------------------

struct CutRegionPart
{
    indexed_triangle_set mesh;
    //For each face of `mesh`, the index of the face it came from in the source, or -1 for a
    //face of a cap. This is what carries paint, supports, seams and fuzzy skin across.
    std::vector<int>     src_face;
    size_t               cap_faces{0};
    // Openings the source mesh already had, which are not this cut's to close.
    size_t               inherited_open_edges{0};
};

struct CutRegionResult
{
    std::vector<CutRegionPart> parts;
    size_t                     cut_loops{0};
    // Loops that could not be spanned; their parts are left open rather than closed wrongly.
    size_t                     unspanned_loops{0};
    //Loops whose minimal lid turned out to be made of the model's own skin, and were fanned
    //from their centre instead. Worth counting: a rise here means the regions being asked for
    //are ones the loop cannot decide about on its own.
    size_t                     fanned_loops{0};
    // The arithmetic that says the cut is a cut: the parts have to add up to what went in.
    double                     source_volume{0.};
    double                     parts_volume{0.};
    bool                       volume_conserved{true};
};

//Split the mesh into one part per label, closing each cut opening with a patch shared by the
//two sides that meet there - the same triangles, wound opposite ways - so the parts mate
//exactly and their volumes add up to the source's.
//
//Returns false only when the input is unusable; a loop that cannot be spanned is reported in
//the result and leaves that opening open, because a wrong lid is worse than a visible hole.
bool split_by_labels(const indexed_triangle_set &src,
                     const std::vector<int>     &labels,
                     int                         label_count,
                     bool                        cap,
                     CutRegionResult            &out,
                     std::string                &failure);

} // namespace Slic3r

#endif // libslic3r_CutRegion_hpp_
