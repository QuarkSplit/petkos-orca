#ifndef slic3r_CutUtils_hpp_
#define slic3r_CutUtils_hpp_

#include "enum_bitmask.hpp"
#include "Point.hpp"
#include "Model.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

using ModelObjectPtrs = std::vector<ModelObject*>;

enum class ModelObjectCutAttribute : int { KeepUpper, KeepLower, KeepAsParts, FlipUpper, FlipLower, PlaceOnCutUpper, PlaceOnCutLower, CreateDowels, InvalidateCutInfo, KeepPaint };
using ModelObjectCutAttributes = enum_bitmask<ModelObjectCutAttribute>;
ENABLE_ENUM_BITMASK_OPERATORS(ModelObjectCutAttribute);

//A cut plane that stops somewhere. Upstream's plane is infinite, which is why cutting an
//arm off a figure also slices the torso, and why both upstreams have twice answered the
//request with component-deselect - a mechanism that cannot cut an ATTACHED arm at all.
//
//The region is a closed contour in the cut plane's OWN frame: the plane is z = 0 there and
//the units are millimetres, so the same value means the same thing whatever the plane's
//orientation in the world. Unbounded is the historical infinite plane and takes the
//historical code path (TriangleMeshSlicer's cut_mesh) untouched, which is what keeps the
//existing flow identical rather than merely equivalent.
//
//Geometrically the bounded cut is one boolean pair: the region extruded along the plane
//normal is a cutter solid, the part above the plane inside it is one piece, everything else
//is the other. Material the region does not sweep is never separated, which is the whole
//point. The walls of that solid are visible on the plane while you draw them, because they
//are what you are choosing: draw them through air and only the plane cuts; draw them
//through material and the walls cut there too, which is what asking for it means.
struct CutBounds
{
    enum class Shape : int { Unbounded = 0, Rectangle, Disc, Lasso };

    Shape              shape{Shape::Unbounded};
    std::vector<Vec2d> contour; // closed, plane-local millimetres; empty when Unbounded

    bool bounded() const { return shape != Shape::Unbounded && contour.size() >= 3; }
    void clear() { shape = Shape::Unbounded; contour.clear(); }

    // Is a point of the cut plane (plane-local mm) inside the region? Unbounded contains everything.
    bool contains(const Vec2d &pt) const;

    static CutBounds make_rectangle(const Vec2d &corner_a, const Vec2d &corner_b);
    static CutBounds make_disc(const Vec2d &center, double radius, int segments = 72);
    static CutBounds make_lasso(std::vector<Vec2d> points);
};

//The cutter solid for a bounded region: the region swept from the plane (z = 0) up to
//z_top, welded, so a boolean backend is handed a manifold rather than a triangle soup.
//Returns an empty set when the region is degenerate.
indexed_triangle_set its_make_cut_prism(const CutBounds &bounds, double z_top);

//The geometry half of a bounded cut, split off so it can run on a worker thread. A 100 MB
//mesh boolean takes long enough that doing it on the UI thread is a frozen window, and the
//rest of a cut cannot follow it there: ModelConfig carries one global timestamp counter and
//Model is not thread-safe either. So the line is drawn exactly here. Everything below
//touches meshes and matrices only - no Model, no ModelConfig, no GUI - and the caller feeds
//the results back to Cut on the UI thread, where the cut is otherwise unchanged.
struct CutBoundedInput
{
    size_t                              volume_idx{0};
    std::shared_ptr<const TriangleMesh> mesh; // shared with the model and never written to
    Transform3d                         matrix{Transform3d::Identity()}; // instance (no offset) * volume
};

struct CutBoundedSplit
{
    indexed_triangle_set upper;
    indexed_triangle_set lower;
};
// Keyed by index into ModelObject::volumes, which is stable across Cut's own object copy.
using CutBoundedSplits = std::map<size_t, CutBoundedSplit>;

// The transform that takes a mesh into the cut plane's OWN frame, where the plane is z = 0.
// One definition, because anything that reasons about a cut in that frame - the split itself,
// the region, a check comparing it against the unbounded cut - has to mean the same plane.
Transform3d cut_space_transform(const Transform3d &cut_matrix);

// UI thread: snapshot the volumes a bounded cut will have to split.
std::vector<CutBoundedInput> collect_bounded_cut_inputs(const ModelObject &object, int instance);

// Worker thread: run the booleans. `canceled` is polled between volumes; a cancelled run
// returns false with an empty failure string, which is how a caller tells the two apart.
bool compute_bounded_splits(const std::vector<CutBoundedInput> &inputs, const Transform3d &cut_matrix,
                            const CutBounds &bounds, CutBoundedSplits &out, std::string &failure,
                            const std::function<bool()> &canceled = std::function<bool()>());


class Cut {

    Model                       m_model;
    int                         m_instance;
    const Transform3d           m_cut_matrix;
    ModelObjectCutAttributes    m_attributes;
    //The region the plane is allowed to cut, and what went wrong if it could not. A cut is
    //never silently downgraded to the unbounded one: an unexecutable bounded cut reports.
    CutBounds                   m_bounds;
    std::string                 m_failure;
    //Splits already computed off the UI thread. Empty means "compute them here", which is
    //what a test or a CLI caller wants and what keeps the two paths one code path.
    CutBoundedSplits            m_splits;

    void post_process(ModelObject* object, ModelObjectPtrs& objects, bool keep, bool place_on_cut, bool flip);
    void post_process(ModelObject* upper_object, ModelObject* lower_object, ModelObjectPtrs& objects);
    void finalize(const ModelObjectPtrs& objects, const std::vector<std::optional<TriangleSelector::SavedPainting>>& saved_paintings);

public:

    Cut(const ModelObject* object, int instance, const Transform3d& cut_matrix, 
        ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper |
                                              ModelObjectCutAttribute::KeepLower |
                                              ModelObjectCutAttribute::KeepAsParts );
    ~Cut() { m_model.clear_objects(); }

    struct Groove
    {
        float depth{ 0.f };
        float width{ 0.f };
        float flaps_angle{ 0.f };
        float angle{ 0.f };
        float depth_init{ 0.f };
        float width_init{ 0.f };
        float flaps_angle_init{ 0.f };
        float angle_init{ 0.f };
        float depth_tolerance{ 0.1f };
        float width_tolerance{ 0.1f };
    };

    struct Part
    {
        bool selected;
        bool is_modifier;
    };

    const ModelObjectPtrs& perform_with_plane();

    //The bounded planar cut. `bounds` is in the cut plane's own frame. An unbounded value
    //is not an error and not a special case: it runs perform_with_plane() unchanged.
    const ModelObjectPtrs& perform_with_bounded_plane(const CutBounds& bounds);

    //Hand over splits computed by compute_bounded_splits() on a worker. The cut then only
    //does the parts that must happen on the UI thread.
    void set_precomputed_splits(CutBoundedSplits splits) { m_splits = std::move(splits); }

    //Empty unless the cut could not be executed as asked. A caller shows it rather than
    //presenting a mesh that is not the cut that was requested.
    const std::string& failure() const { return m_failure; }

    const ModelObjectPtrs& perform_by_contour(const ModelObject* src_object, std::vector<Part> parts, int dowels_count);
    const ModelObjectPtrs& perform_with_groove(const Groove&      groove,
                                               const Transform3d& rotation_m,
                                               const int          groove_count,
                                               const float        groove_gap,
                                               const float        m_radius,
                                               bool               keep_as_parts = false);

    static float calculate_groove_width(const Cut::Groove& groove, const float m_radius);
    }; // namespace Cut

} // namespace Slic3r

#endif /* slic3r_CutUtils_hpp_ */
