
#include "CutUtils.hpp"
#include "ClipperUtils.hpp"
#include "ExPolygon.hpp"
#include "Geometry.hpp"
#include "libslic3r.h"
#include "MeshBoolean.hpp"
#include "Model.hpp"
#include "Tesselate.hpp"
#include "TriangleMeshSlicer.hpp"
#include "TriangleSelector.hpp"
#include "ObjectID.hpp"

#include <cmath>
#include <map>
#include <tuple>

#include <boost/log/trivial.hpp>

namespace Slic3r {

using namespace Geometry;

// ----------------------------------------------------------------------------------------
// The bounded region
// ----------------------------------------------------------------------------------------

static Polygon cut_bounds_to_polygon(const std::vector<Vec2d> &contour)
{
    Points pts;
    pts.reserve(contour.size());
    for (const Vec2d &p : contour)
        pts.emplace_back(scaled(p.x()), scaled(p.y()));
    return Polygon(std::move(pts));
}

//A lasso is drawn by hand, so it crosses itself; a rectangle dragged backwards comes out
//clockwise. Both are answered by the same union: it returns simple, correctly wound
//ExPolygons with holes where the stroke wrapped twice, which is what a tessellator and a
//point-in-region test both need. Doing this once, here, is why neither of them has a
//special case for the shape that produced the region.
static ExPolygons cut_bounds_regions(const CutBounds &bounds)
{
    if (!bounds.bounded())
        return {};
    Polygon poly = cut_bounds_to_polygon(bounds.contour);
    if (poly.points.size() < 3)
        return {};
    return union_ex(Polygons{std::move(poly)});
}

bool CutBounds::contains(const Vec2d &pt) const
{
    if (!bounded())
        return true;
    const Point p(scaled(pt.x()), scaled(pt.y()));
    for (const ExPolygon &ep : cut_bounds_regions(*this))
        if (ep.contains(p))
            return true;
    return false;
}

CutBounds CutBounds::make_rectangle(const Vec2d &corner_a, const Vec2d &corner_b)
{
    CutBounds b;
    const double x0 = std::min(corner_a.x(), corner_b.x());
    const double x1 = std::max(corner_a.x(), corner_b.x());
    const double y0 = std::min(corner_a.y(), corner_b.y());
    const double y1 = std::max(corner_a.y(), corner_b.y());
    b.shape   = Shape::Rectangle;
    b.contour = {Vec2d(x0, y0), Vec2d(x1, y0), Vec2d(x1, y1), Vec2d(x0, y1)};
    return b;
}

CutBounds CutBounds::make_disc(const Vec2d &center, double radius, int segments)
{
    CutBounds b;
    segments = std::max(segments, 8);
    b.shape  = Shape::Disc;
    b.contour.reserve(size_t(segments));
    for (int i = 0; i < segments; ++i) {
        const double a = 2.0 * PI * double(i) / double(segments);
        b.contour.emplace_back(center.x() + radius * std::cos(a), center.y() + radius * std::sin(a));
    }
    return b;
}

CutBounds CutBounds::make_lasso(std::vector<Vec2d> points)
{
    CutBounds b;
    b.shape   = Shape::Lasso;
    b.contour = std::move(points);
    return b;
}

//Welded by construction, because a triangle soup is not a manifold and every boolean backend
//in the ladder refuses one. A cap tessellated by GLU and a wall built from the same contour
//meet only if they agree on a vertex twice over: the same millimetre value, and the same
//quantisation of it. Both are arranged below, and both had to be.
indexed_triangle_set its_make_cut_prism(const CutBounds &bounds, double z_top)
{
    indexed_triangle_set out;
    const ExPolygons regions = cut_bounds_regions(bounds);
    if (regions.empty() || z_top <= EPSILON)
        return out;

    //The weld key is ONE rounding of the millimetre coordinate, applied identically to the cap
    //and to the wall. It used to be the scaled integer, recovered from the tessellator's output
    //by scaling back up - and unscale()/scale() do not round-trip, because 1e-6 is not exact in
    //binary and the inverse truncates. A rectangle survived that (whole millimetres round-trip
    //fine) and a 72-segment disc did not: 64 of its 72 wall edges found no cap edge to meet, so
    //the cutter was a mesh that renders perfectly and that every boolean backend refuses.
    auto key_of = [](double x, double y, int level) {
        return std::make_tuple(std::llround(x / SCALING_FACTOR), std::llround(y / SCALING_FACTOR), level);
    };
    std::map<std::tuple<long long, long long, int>, int> index_of;
    auto vertex = [&out, &index_of, &key_of](double x, double y, int level, double z) {
        const auto key = key_of(x, y, level);
        auto it = index_of.find(key);
        if (it != index_of.end())
            return it->second;
        const int id = int(out.vertices.size());
        out.vertices.emplace_back(float(x), float(y), float(z));
        index_of.emplace(key, id);
        return id;
    };
    auto vertex_at = [&vertex](const Vec3d &p, int level) { return vertex(p.x(), p.y(), level, p.z()); };

    //A cap triangle's winding is not something to take from the tessellator. GLU answers in
    //fans and strips, and a cap whose triangles disagree with the walls about which way is out
    //leaves the solid open along every edge the two share - which is a mesh that renders
    //perfectly and that every boolean backend refuses. Signed area decides it here, so the
    //orientation is a property of the geometry rather than of whoever produced the triangles.
    auto add_cap = [&out, &vertex_at](const std::vector<Vec3d> &tris, int level, bool upward) {
        for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            const Vec3d &a = tris[i], &b = tris[i + 1], &c = tris[i + 2];
            const double area2 = (b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y());
            if (std::abs(area2) < 1e-12)
                continue; // a zero-area triangle is not a face, and welding turns it into a seam
            const int ia = vertex_at(a, level), ib = vertex_at(b, level), ic = vertex_at(c, level);
            if ((area2 > 0.0) == upward)
                out.indices.emplace_back(ia, ib, ic);
            else
                out.indices.emplace_back(ia, ic, ib);
        }
    };

    for (const ExPolygon &ep : regions) {
        // Caps: bottom sits ON the cut plane and faces away from the solid, top closes it.
        add_cap(triangulate_expolygon_3d(ep, 0.0,   NORMALS_UP), 0, false);
        add_cap(triangulate_expolygon_3d(ep, z_top, NORMALS_UP), 1, true);

        // Walls. union_ex() leaves the contour counter-clockwise and every hole clockwise,
        // so one winding rule covers both and the solid comes out consistently oriented.
        //unscale<double>() is the SAME conversion Tesselate.cpp feeds GLU, so a wall vertex and
        //the cap vertex it must meet are the same number before they are the same key.
        auto add_wall = [&out, &vertex, z_top](const Points &pts) {
            const size_t n = pts.size();
            for (size_t i = 0; i < n; ++i) {
                const double ax = unscaled<double>(pts[i].x()),           ay = unscaled<double>(pts[i].y());
                const double bx = unscaled<double>(pts[(i + 1) % n].x()), by = unscaled<double>(pts[(i + 1) % n].y());
                const int a0 = vertex(ax, ay, 0, 0.0),   b0 = vertex(bx, by, 0, 0.0);
                const int a1 = vertex(ax, ay, 1, z_top), b1 = vertex(bx, by, 1, z_top);
                out.indices.emplace_back(a0, b0, b1);
                out.indices.emplace_back(a0, b1, a1);
            }
        };
        add_wall(ep.contour.points);
        for (const Polygon &hole : ep.holes)
            add_wall(hole.points);
    }
    return out;
}

static void apply_tolerance(ModelVolume* vol)
{
    ModelVolume::CutInfo& cut_info = vol->cut_info;

    assert(cut_info.is_connector);
    if (!cut_info.is_processed)
        return;

    Vec3d sf = vol->get_scaling_factor();

    // make a "hole" wider
    sf[X] += double(cut_info.radius_tolerance);
    sf[Y] += double(cut_info.radius_tolerance);

    // make a "hole" dipper
    sf[Z] += double(cut_info.height_tolerance);

    vol->set_scaling_factor(sf);

    // correct offset in respect to the new depth
    Vec3d rot_norm = rotation_transform(vol->get_rotation()) * Vec3d::UnitZ();
    if (rot_norm.norm() != 0.0)
        rot_norm.normalize();

    double z_offset = 0.5 * static_cast<double>(cut_info.height_tolerance);
    if (cut_info.connector_type == CutConnectorType::Plug || 
        cut_info.connector_type == CutConnectorType::Snap)
        z_offset -= 0.05; // add small Z offset to better preview

    vol->set_offset(vol->get_offset() + rot_norm * z_offset);
}

static void add_cut_volume(TriangleMesh& mesh, ModelObject* object, const ModelVolume* src_volume, const Transform3d& cut_matrix, const std::string& suffix = {}, ModelVolumeType type = ModelVolumeType::MODEL_PART)
{
    if (mesh.empty())
        return;

    mesh.transform(cut_matrix);
    ModelVolume* vol = object->add_volume(mesh);
    vol->set_type(type);

    vol->name = src_volume->name + suffix;
    // Don't copy the config's ID.
    vol->config.assign_config(src_volume->config);
    assert(vol->config.id().valid());
    assert(vol->config.id() != src_volume->config.id());
    vol->set_material(src_volume->material_id(), *src_volume->material());
    vol->cut_info = src_volume->cut_info;
}

//The bounded split, in cut space: the plane is z = 0 and the region is drawn on it. The
//cutter is that region swept upwards until it has left the mesh, so what is above the plane
//AND inside the region is one part and everything else is the other. Both come out of the
//same solid, which is why they agree on the cut face and why their volumes add up to the
//source's.
//
//A wall of the cutter that runs through air cuts nothing, which is what makes the arm
//separable from the torso; a wall run through material cuts there too, and that is the
//instruction rather than a defect. Nothing here decides which the user meant.
static bool bounded_volume_split(const indexed_triangle_set &src, const CutBounds &bounds,
                                 indexed_triangle_set &upper_its, indexed_triangle_set &lower_its,
                                 std::string &failure)
{
    upper_its = indexed_triangle_set();
    lower_its = indexed_triangle_set();

    const BoundingBoxf3 bb = bounding_box(src);
    if (!bb.defined) {
        failure = "the volume has no geometry to cut";
        return false;
    }
    //Nothing of this volume reaches above the plane, so there is nothing for the region to
    //take. That is an answer, not a failure, and it matches what the unbounded cut does.
    if (bb.max.z() <= EPSILON) {
        lower_its = src;
        return true;
    }

    const double z_top  = bb.max.z() + std::max(1.0, 0.01 * bb.size().norm());
    indexed_triangle_set cutter = its_make_cut_prism(bounds, z_top);
    if (cutter.empty()) {
        failure = "the bounded region is degenerate";
        return false;
    }

    upper_its = src;
    MeshBoolean::LadderResult up = MeshBoolean::execute(MeshBoolean::Op::Intersection, upper_its, cutter);
    if (!up.ok) {
        failure = "the bounded region could not be intersected with the model (" + up.reason + ")";
        upper_its = indexed_triangle_set();
        return false;
    }

    lower_its = src;
    MeshBoolean::LadderResult down = MeshBoolean::execute(MeshBoolean::Op::Difference, lower_its, cutter);
    if (!down.ok) {
        failure = "the bounded region could not be subtracted from the model (" + down.reason + ")";
        upper_its = indexed_triangle_set();
        lower_its = indexed_triangle_set();
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "bounded cut: intersection via " << up.backend << ", difference via " << down.backend;
    return true;
}

//The one place a mesh is put into cut space, shared by the cut itself, by the worker that
//precomputes for it and by anything checking it, so none of them can drift apart about what
//"the cut plane" means.
Transform3d cut_space_transform(const Transform3d &cut_matrix)
{
    const Transformation cut_transformation = Transformation(cut_matrix);
    return cut_transformation.get_rotation_matrix().inverse() * translation_transform(-1 * cut_transformation.get_offset());
}

std::vector<CutBoundedInput> collect_bounded_cut_inputs(const ModelObject &object, int instance)
{
    std::vector<CutBoundedInput> out;
    if (instance < 0 || instance >= int(object.instances.size()))
        return out;
    const Transform3d instance_matrix = object.instances[instance]->get_transformation().get_matrix_no_offset();
    for (size_t i = 0; i < object.volumes.size(); ++i) {
        const ModelVolume *v = object.volumes[i];
        if (v == nullptr || !v->is_model_part() || v->mesh().empty())
            continue;
        CutBoundedInput in;
        in.volume_idx = i;
        //shared_ptr, not a copy: the mesh is immutable in the model and this is what makes
        //snapshotting a 100 MB object on the UI thread free.
        in.mesh   = v->get_mesh_shared_ptr();
        in.matrix = instance_matrix * v->get_matrix();
        out.push_back(std::move(in));
    }
    return out;
}

bool compute_bounded_splits(const std::vector<CutBoundedInput> &inputs, const Transform3d &cut_matrix,
                            const CutBounds &bounds, CutBoundedSplits &out, std::string &failure,
                            const std::function<bool()> &canceled)
{
    out.clear();
    failure.clear();
    if (!bounds.bounded()) {
        failure = "the cut region is unbounded, so there is nothing to precompute";
        return false;
    }

    const Transform3d invert_cut_matrix = cut_space_transform(cut_matrix);
    for (const CutBoundedInput &in : inputs) {
        if (canceled && canceled())
            return false;
        if (!in.mesh)
            continue;

        TriangleMesh mesh(*in.mesh);
        mesh.transform(invert_cut_matrix * in.matrix, true);

        CutBoundedSplit split;
        if (!bounded_volume_split(mesh.its, bounds, split.upper, split.lower, failure))
            return false;
        out.emplace(in.volume_idx, std::move(split));
    }
    return true;
}

static void process_volume_cut( const ModelVolume* volume, const Transform3d& instance_matrix, const Transform3d& cut_matrix,
                                ModelObjectCutAttributes attributes, TriangleMesh& upper_mesh, TriangleMesh& lower_mesh,
                                const CutBounds& bounds = CutBounds(), std::string* failure = nullptr,
                                const CutBoundedSplit* precomputed = nullptr)
{
    const auto volume_matrix = volume->get_matrix();

    indexed_triangle_set upper_its, lower_its;
    if (bounds.bounded() && precomputed != nullptr) {
        //Already computed off the UI thread against the same matrices; recomputing it here
        //would be the whole cost of the cut, done twice.
        upper_its = precomputed->upper;
        lower_its = precomputed->lower;
    }
    else {
        // Transform the mesh by the combined transformation matrix.
        // Flip the triangles in case the composite transformation is left handed.
        TriangleMesh mesh(volume->mesh());
        mesh.transform(cut_space_transform(cut_matrix) * instance_matrix * volume_matrix, true);

        if (bounds.bounded()) {
            std::string why;
            if (!bounded_volume_split(mesh.its, bounds, upper_its, lower_its, why)) {
                //A bounded cut that cannot be executed is reported and abandoned. Falling
                //back to the infinite plane would cut the geometry the user drew a boundary
                //around to protect.
                BOOST_LOG_TRIVIAL(error) << "bounded cut refused on volume '" << volume->name << "': " << why;
                if (failure && failure->empty())
                    *failure = why;
                return;
            }
        }
        else
            cut_mesh(mesh.its, 0.0f, &upper_its, &lower_its);
    }

    if (attributes.has(ModelObjectCutAttribute::KeepUpper))
        upper_mesh = TriangleMesh(upper_its);
    if (attributes.has(ModelObjectCutAttribute::KeepLower))
        lower_mesh = TriangleMesh(lower_its);
}

static void process_connector_cut(  ModelVolume* volume, const Transform3d& instance_matrix, const Transform3d& cut_matrix,
                                    ModelObjectCutAttributes attributes, ModelObject* upper, ModelObject* lower,
                                    std::vector<ModelObject*>& dowels)
{
    assert(volume->cut_info.is_connector);
    volume->cut_info.set_processed();

    const auto volume_matrix = volume->get_matrix();

    // ! Don't apply instance transformation for the conntectors.
    // This transformation is already there
    if (volume->cut_info.connector_type != CutConnectorType::Dowel) {
        if (attributes.has(ModelObjectCutAttribute::KeepUpper)) {
            ModelVolume* vol = nullptr;
            if (volume->cut_info.connector_type == CutConnectorType::Snap) {
                TriangleMesh mesh = TriangleMesh(its_make_cylinder(1.0, 1.0, PI / 180.));

                vol = upper->add_volume(std::move(mesh));
                vol->set_transformation(volume->get_transformation());
                vol->set_type(ModelVolumeType::NEGATIVE_VOLUME);

                vol->cut_info = volume->cut_info;
                vol->name = volume->name;
            }
            else
                vol = upper->add_volume(*volume);

            vol->set_transformation(volume_matrix);
            apply_tolerance(vol);
        }
        if (attributes.has(ModelObjectCutAttribute::KeepLower)) {
            ModelVolume* vol = lower->add_volume(*volume);
            vol->set_transformation(volume_matrix);
            // for lower part change type of connector from NEGATIVE_VOLUME to MODEL_PART if this connector is a plug
            vol->set_type(ModelVolumeType::MODEL_PART);
        }
    }
    else {
        if (attributes.has(ModelObjectCutAttribute::CreateDowels)) {
            ModelObject* dowel{ nullptr };
            // Clone the object to duplicate instances, materials etc.
            volume->get_object()->clone_for_cut(&dowel);

            // add one more solid part same as connector if this connector is a dowel
            ModelVolume* vol = dowel->add_volume(*volume);
            vol->set_type(ModelVolumeType::MODEL_PART);

            // But discard rotation and Z-offset for this volume
            vol->set_rotation(Vec3d::Zero());
            vol->set_offset(Z, 0.0);

            dowels.push_back(dowel);
        }

        // Cut the dowel
        apply_tolerance(volume);

        // Perform cut
        TriangleMesh upper_mesh, lower_mesh;
        process_volume_cut(volume, Transform3d::Identity(), cut_matrix, attributes, upper_mesh, lower_mesh);

        // add small Z offset to better preview
        upper_mesh.translate((-0.05 * Vec3d::UnitZ()).cast<float>());
        lower_mesh.translate((0.05 * Vec3d::UnitZ()).cast<float>());

        // Add cut parts to the related objects
        add_cut_volume(upper_mesh, upper, volume, cut_matrix, "_A", volume->type());
        add_cut_volume(lower_mesh, lower, volume, cut_matrix, "_B", volume->type());
    }
}

static void process_modifier_cut(ModelVolume* volume, const Transform3d& instance_matrix, const Transform3d& inverse_cut_matrix,
                                 ModelObjectCutAttributes attributes, ModelObject* upper, ModelObject* lower)
{
    const auto volume_matrix = instance_matrix * volume->get_matrix();

    // Modifiers are not cut, but we still need to add the instance transformation
    // to the modifier volume transformation to preserve their shape properly.
    volume->set_transformation(Transformation(volume_matrix));

    if (attributes.has(ModelObjectCutAttribute::KeepAsParts)) {
        upper->add_volume(*volume);
        return;
    }

    // Some logic for the negative volumes/connectors. Add only needed modifiers
    auto bb = volume->mesh().transformed_bounding_box(inverse_cut_matrix * volume_matrix);
    bool is_crossed_by_cut = bb.min[Z] <= 0 && bb.max[Z] >= 0;
    if (attributes.has(ModelObjectCutAttribute::KeepUpper) && (bb.min[Z] >= 0 || is_crossed_by_cut))
        upper->add_volume(*volume);
    if (attributes.has(ModelObjectCutAttribute::KeepLower) && (bb.max[Z] <= 0 || is_crossed_by_cut))
        lower->add_volume(*volume);
}

static void process_solid_part_cut(const ModelVolume* volume, const Transform3d& instance_matrix, const Transform3d& cut_matrix,
                            ModelObjectCutAttributes attributes, ModelObject* upper, ModelObject* lower,
                            const CutBounds& bounds = CutBounds(), std::string* failure = nullptr,
                            const CutBoundedSplit* precomputed = nullptr)
{
    // Perform cut
    TriangleMesh upper_mesh, lower_mesh;
    process_volume_cut(volume, instance_matrix, cut_matrix, attributes, upper_mesh, lower_mesh, bounds, failure, precomputed);

    // Add required cut parts to the objects

    if (attributes.has(ModelObjectCutAttribute::KeepAsParts)) {
        add_cut_volume(upper_mesh, upper, volume, cut_matrix, "_A");
        if (!lower_mesh.empty()) {
            add_cut_volume(lower_mesh, upper, volume, cut_matrix, "_B");
            upper->volumes.back()->cut_info.is_from_upper = false;
        }
        return;
    }

    if (attributes.has(ModelObjectCutAttribute::KeepUpper))
        add_cut_volume(upper_mesh, upper, volume, cut_matrix);

    if (attributes.has(ModelObjectCutAttribute::KeepLower) && !lower_mesh.empty())
        add_cut_volume(lower_mesh, lower, volume, cut_matrix);
}

static void reset_instance_transformation(ModelObject* object, size_t src_instance_idx, 
                                          const Transform3d& cut_matrix = Transform3d::Identity(),
                                          bool place_on_cut = false, bool flip = false)
{
    // Reset instance transformation except offset and Z-rotation

    for (size_t i = 0; i < object->instances.size(); ++i) {
        auto& obj_instance = object->instances[i];
        const double rot_z = obj_instance->get_rotation().z();
        
        Transformation inst_trafo = Transformation(obj_instance->get_transformation().get_matrix_no_scaling_factor());
        // add respect to mirroring
        if (obj_instance->is_left_handed())
            inst_trafo = inst_trafo * Transformation(scale_transform(Vec3d(-1, 1, 1)));

        obj_instance->set_transformation(inst_trafo);

        Vec3d rotation = Vec3d::Zero();
        if (!flip && !place_on_cut) {
            if ( i != src_instance_idx)
            rotation[Z] = rot_z;
        }
        else {
            Transform3d rotation_matrix = Transform3d::Identity();
            if (flip)
                rotation_matrix = rotation_transform(PI * Vec3d::UnitX());

            if (place_on_cut)
                rotation_matrix = rotation_matrix * Transformation(cut_matrix).get_rotation_matrix().inverse();

            if (i != src_instance_idx)
                rotation_matrix = rotation_transform(rot_z * Vec3d::UnitZ()) * rotation_matrix;

            rotation = Transformation(rotation_matrix).get_rotation();
        }

        obj_instance->set_rotation(rotation);
    }
}


Cut::Cut(const ModelObject* object, int instance, const Transform3d& cut_matrix,
         ModelObjectCutAttributes attributes/*= ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepAsParts*/)
    : m_instance(instance), m_cut_matrix(cut_matrix), m_attributes(attributes)
{
    m_model = Model();
    if (object)
        m_model.add_object(*object);
}

void Cut::post_process(ModelObject* object, ModelObjectPtrs& cut_object_ptrs, bool keep, bool place_on_cut, bool flip)
{
    if (!object) return;

    if (keep && !object->volumes.empty()) {
        reset_instance_transformation(object, m_instance, m_cut_matrix, place_on_cut, flip);
        cut_object_ptrs.push_back(object);
    }
    else
        m_model.objects.push_back(object); // will be deleted in m_model.clear_objects();
}

void Cut::post_process(ModelObject* upper, ModelObject* lower, ModelObjectPtrs& cut_object_ptrs)
{
    post_process(upper, cut_object_ptrs,
        m_attributes.has(ModelObjectCutAttribute::KeepUpper),
        m_attributes.has(ModelObjectCutAttribute::PlaceOnCutUpper),
        m_attributes.has(ModelObjectCutAttribute::FlipUpper));

    post_process(lower, cut_object_ptrs,
        m_attributes.has(ModelObjectCutAttribute::KeepLower),
        m_attributes.has(ModelObjectCutAttribute::PlaceOnCutLower),
        m_attributes.has(ModelObjectCutAttribute::PlaceOnCutLower) || m_attributes.has(ModelObjectCutAttribute::FlipLower));
}


void Cut::finalize(const ModelObjectPtrs& objects, const std::vector<std::optional<TriangleSelector::SavedPainting>>& saved_paintings)
{
    // Paint volumes
    for (const auto& saved_painting : saved_paintings) {
        if (saved_painting) {
            for (const auto object : objects) {
                for (const auto volume : object->volumes) {
                    if (volume->is_model_part() && !volume->is_cut_connector()) {
                    volume->restore_painting(saved_painting, true);
                    }
                }
            }
        }
    }

    //clear model from temporary objects
    m_model.clear_objects();

    // add to model result objects
    m_model.objects = objects;
}


const ModelObjectPtrs& Cut::perform_with_bounded_plane(const CutBounds& bounds)
{
    //An unbounded region is not a degenerate bounded cut; it is the historical cut, and it
    //runs the historical code path so that turning bounds off leaves nothing changed.
    m_bounds = bounds;
    m_failure.clear();
    return perform_with_plane();
}

const ModelObjectPtrs& Cut::perform_with_plane()
{
    if (!m_attributes.has(ModelObjectCutAttribute::KeepUpper) && !m_attributes.has(ModelObjectCutAttribute::KeepLower)) {
        m_model.clear_objects();
        return m_model.objects;
    }

    ModelObject* mo = m_model.objects.front();

    BOOST_LOG_TRIVIAL(trace) << "ModelObject::cut - start";

    // Clone the object to duplicate instances, materials etc.
    ModelObject* upper{ nullptr };
    if (m_attributes.has(ModelObjectCutAttribute::KeepUpper))
        mo->clone_for_cut(&upper);

    ModelObject* lower{ nullptr };
    if (m_attributes.has(ModelObjectCutAttribute::KeepLower) && !m_attributes.has(ModelObjectCutAttribute::KeepAsParts))
        mo->clone_for_cut(&lower);

    std::vector<ModelObject*> dowels;

    // Because transformations are going to be applied to meshes directly,
    // we reset transformation of all instances and volumes,
    // except for translation and Z-rotation on instances, which are preserved
    // in the transformation matrix and not applied to the mesh transform.

    const auto              instance_matrix = mo->instances[m_instance]->get_transformation().get_matrix_no_offset();
    const Transformation    cut_transformation = Transformation(m_cut_matrix);
    const Transform3d       inverse_cut_matrix = cut_transformation.get_rotation_matrix().inverse() * translation_transform(-1. * cut_transformation.get_offset());

    std::vector<std::optional<TriangleSelector::SavedPainting>> saved_paintings;
    size_t volume_idx = 0;
    for (ModelVolume* volume : mo->volumes) {
        // Save painting data before reset_extra_facets() discards it.
        if (m_attributes.has(ModelObjectCutAttribute::KeepPaint)) {
            saved_paintings.emplace_back(volume->save_painting());
            if (saved_paintings.back()) {
                // Transform mesh to cut space (same transform as process_volume_cut applies)
                saved_paintings.back()->mesh.transform(instance_matrix * volume->get_matrix(), true);
            }
        }

        volume->reset_extra_facets();

        if (!volume->is_model_part()) {
            if (volume->cut_info.is_processed)
                process_modifier_cut(volume, instance_matrix, inverse_cut_matrix, m_attributes, upper, lower);
            else
                process_connector_cut(volume, instance_matrix, m_cut_matrix, m_attributes, upper, lower, dowels);
        }
        else if (!volume->mesh().empty()) {
            //The index is into this object's volume list, which is the same list
            //collect_bounded_cut_inputs() walked, so a precomputed split lands on the volume
            //it was computed from.
            const auto it = m_splits.find(volume_idx);
            process_solid_part_cut(volume, instance_matrix, m_cut_matrix, m_attributes, upper, lower, m_bounds, &m_failure,
                                   it == m_splits.end() ? nullptr : &it->second);
        }
        ++volume_idx;
    }

    //A bounded cut that could not be executed produces no parts and says why. Returning the
    //objects built so far would hand the caller a model that is missing the geometry the
    //boolean refused, which is the one outcome worse than refusing.
    if (!m_failure.empty()) {
        if (upper) m_model.objects.push_back(upper);
        if (lower) m_model.objects.push_back(lower);
        for (ModelObject* dowel : dowels)
            m_model.objects.push_back(dowel);
        m_model.clear_objects();
        return m_model.objects;
    }

    // Post-process cut parts

    if (m_attributes.has(ModelObjectCutAttribute::KeepAsParts) && upper->volumes.empty()) {
        m_model = Model();
        m_model.objects.push_back(upper);
        return m_model.objects;
    }

    ModelObjectPtrs cut_object_ptrs;

    if (m_attributes.has(ModelObjectCutAttribute::KeepAsParts) && !upper->volumes.empty()) {
        reset_instance_transformation(upper, m_instance, m_cut_matrix);
        cut_object_ptrs.push_back(upper);
    }
    else {
        // Delete all modifiers which are not intersecting with solid parts bounding box
        auto delete_extra_modifiers = [this](ModelObject* mo) {
            if (!mo) return;
            const BoundingBoxf3 obj_bb = mo->instance_bounding_box(m_instance);
            const Transform3d inst_matrix = mo->instances[m_instance]->get_transformation().get_matrix();

            for (int i = int(mo->volumes.size()) - 1; i >= 0; --i)
                if (const ModelVolume* vol = mo->volumes[i];
                    !vol->is_model_part() && !vol->is_cut_connector()) {
                    auto bb = vol->mesh().transformed_bounding_box(inst_matrix * vol->get_matrix());
                    if (!obj_bb.intersects(bb))
                        mo->delete_volume(i);
                }
        };

        post_process(upper, lower, cut_object_ptrs);
        delete_extra_modifiers(upper);
        delete_extra_modifiers(lower);

        if (m_attributes.has(ModelObjectCutAttribute::CreateDowels) && !dowels.empty()) {
            for (auto dowel : dowels) {
                reset_instance_transformation(dowel, m_instance);
                dowel->name += "-Dowel-" + dowel->volumes[0]->name;
                cut_object_ptrs.push_back(dowel);
            }
        }
    }

    finalize(cut_object_ptrs, saved_paintings);

    BOOST_LOG_TRIVIAL(trace) << "ModelObject::cut - end";

    return m_model.objects;
}

static void distribute_modifiers_from_object(ModelObject* from_obj, const int instance_idx, ModelObject* to_obj1, ModelObject* to_obj2)
{
    auto              obj1_bb = to_obj1 ? to_obj1->instance_bounding_box(instance_idx) : BoundingBoxf3();
    auto              obj2_bb = to_obj2 ? to_obj2->instance_bounding_box(instance_idx) : BoundingBoxf3();
    const Transform3d inst_matrix = from_obj->instances[instance_idx]->get_transformation().get_matrix();

    for (ModelVolume* vol : from_obj->volumes)
        if (!vol->is_model_part()) {
            // Don't add modifiers which are processed connectors
            if (vol->cut_info.is_connector && !vol->cut_info.is_processed)
                continue;
            auto bb = vol->mesh().transformed_bounding_box(inst_matrix * vol->get_matrix());
            // Don't add modifiers which are not intersecting with solid parts
            if (obj1_bb.intersects(bb))
                to_obj1->add_volume(*vol);
            if (obj2_bb.intersects(bb))
                to_obj2->add_volume(*vol);
        }
}

static void merge_solid_parts_inside_object(ModelObjectPtrs& objects)
{
    for (ModelObject* mo : objects) {
        TriangleMesh mesh;
        // Merge all SolidPart but not Connectors
        for (const ModelVolume* mv : mo->volumes) {
            if (mv->is_model_part() && !mv->is_cut_connector()) {
                TriangleMesh m = mv->mesh();
                m.transform(mv->get_matrix());
                mesh.merge(m);
            }
        }
        if (!mesh.empty()) {
            ModelVolume* new_volume = mo->add_volume(mesh);
            new_volume->name = mo->name;
            // Delete all merged SolidPart but not Connectors
            for (int i = int(mo->volumes.size()) - 2; i >= 0; --i) {
                const ModelVolume* mv = mo->volumes[i];
                if (mv->is_model_part() && !mv->is_cut_connector())
                    mo->delete_volume(i);
            }
            // Ensuring that volumes start with solid parts for proper slicing
            mo->sort_volumes(true);
        }
    }
}


const ModelObjectPtrs& Cut::perform_by_contour(const ModelObject* src_object, std::vector<Part> parts, int dowels_count)
{
    ModelObject* cut_mo = m_model.objects.front();

    // Clone the object to duplicate instances, materials etc.
    ModelObject* upper{ nullptr };
    if (m_attributes.has(ModelObjectCutAttribute::KeepUpper)) cut_mo->clone_for_cut(&upper);
    ModelObject* lower{ nullptr };
    if (m_attributes.has(ModelObjectCutAttribute::KeepLower)) cut_mo->clone_for_cut(&lower);

    if (upper && lower) {
        upper->name = upper->name + "_A";
        lower->name = lower->name + "_B";
    }

    // Save painting data so we later can remap it.
    std::vector<std::optional<TriangleSelector::SavedPainting>> saved_paintings;
    if (m_attributes.has(ModelObjectCutAttribute::KeepPaint)) {
        const auto instance_matrix = src_object->instances[m_instance]->get_transformation().get_matrix_no_offset();
        for (const auto volume : src_object->volumes) {
            saved_paintings.emplace_back(volume->save_painting());
            if (saved_paintings.back()) {
                // Transform mesh to cut space (same transform as process_volume_cut applies)
                saved_paintings.back()->mesh.transform(instance_matrix * volume->get_matrix(), true);
            }
        }
    }

    const size_t cut_parts_cnt = parts.size();
    bool has_modifiers = false;

    // Distribute SolidParts to the Upper/Lower object
    for (size_t id = 0; id < cut_parts_cnt; ++id) {
        if (parts[id].is_modifier)
            has_modifiers = true; // modifiers will be added later to the related parts
        else if (ModelObject* obj = (parts[id].selected ? upper : lower))
            obj->add_volume(*(cut_mo->volumes[id]));
    }

    if (has_modifiers) {
        // Distribute Modifiers to the Upper/Lower object
        distribute_modifiers_from_object(cut_mo, m_instance, upper, lower);
    }

    ModelObjectPtrs cut_object_ptrs;

    ModelVolumePtrs& volumes = cut_mo->volumes;
    if (volumes.size() == cut_parts_cnt) {
        // Means that object is cut without connectors

        // Just add Upper and Lower objects to cut_object_ptrs
        post_process(upper, lower, cut_object_ptrs);

        // Now merge all model parts together:
        merge_solid_parts_inside_object(cut_object_ptrs);

        // replace initial objects in model with cut object 
        finalize(cut_object_ptrs, saved_paintings);
    }
    else if (volumes.size() > cut_parts_cnt) {
        // Means that object is cut with connectors

        // All volumes are distributed to Upper / Lower object,
        // So we don’t need them anymore
        for (size_t id = 0; id < cut_parts_cnt; id++)
            delete* (volumes.begin() + id);
        volumes.erase(volumes.begin(), volumes.begin() + cut_parts_cnt);

        // Perform cut just to get connectors
        Cut cut(cut_mo, m_instance, m_cut_matrix, m_attributes);
        const ModelObjectPtrs& cut_connectors_obj = cut.perform_with_plane();
        assert(dowels_count > 0 ? cut_connectors_obj.size() >= 3 : cut_connectors_obj.size() == 2);

        // Connectors from upper object
        for (const ModelVolume* volume : cut_connectors_obj[0]->volumes)
            upper->add_volume(*volume, volume->type());

        // Connectors from lower object
        for (const ModelVolume* volume : cut_connectors_obj[1]->volumes)
            lower->add_volume(*volume, volume->type());

        // Add Upper and Lower objects to cut_object_ptrs
        post_process(upper, lower, cut_object_ptrs);

        // Now merge all model parts together:
        merge_solid_parts_inside_object(cut_object_ptrs);

        // replace initial objects in model with cut object
        finalize(cut_object_ptrs, saved_paintings);

        // Add Dowel-connectors as separate objects to model
        if (cut_connectors_obj.size() >= 3)
            for (size_t id = 2; id < cut_connectors_obj.size(); id++)
                m_model.add_object(*cut_connectors_obj[id]);
    }

    return m_model.objects;
}


const ModelObjectPtrs& Cut::perform_with_groove(const Groove&       groove,
                                                const Transform3d&  rotation_m,
                                                const int           groove_count,
                                                const float         groove_gap,
                                                const float         m_radius,
                                                bool                keep_as_parts /* = false*/)
{
    ModelObject* cut_mo = m_model.objects.front();

    // Clone the object to duplicate instances, materials etc.
    ModelObject* upper{ nullptr };
    cut_mo->clone_for_cut(&upper);
    ModelObject* lower{ nullptr };
    cut_mo->clone_for_cut(&lower);

    if (upper && lower) {
        upper->name = upper->name + "_A";
        lower->name = lower->name + "_B";
    }

    // Save painting data so we later can remap it.
    std::vector<std::optional<TriangleSelector::SavedPainting>> saved_paintings;
    if (m_attributes.has(ModelObjectCutAttribute::KeepPaint)) {
        const auto instance_matrix = cut_mo->instances[m_instance]->get_transformation().get_matrix_no_offset();
        for (const auto volume : cut_mo->volumes) {
            saved_paintings.emplace_back(volume->save_painting());
            if (saved_paintings.back()) {
                // Transform mesh to cut space (same transform as process_volume_cut applies)
                saved_paintings.back()->mesh.transform(instance_matrix * volume->get_matrix(), true);
            }
        }
    }

    const double groove_half_depth = 0.5 * double(groove.depth);

    Model tmp_model_for_cut = Model();

    Model tmp_model = Model();
    tmp_model.add_object(*cut_mo);
    ModelObject* tmp_object = tmp_model.objects.front();

    auto add_volumes_from_cut = [](ModelObject* object, const ModelObjectCutAttribute attribute, const Model& tmp_model_for_cut) {
        const auto& volumes = tmp_model_for_cut.objects.front()->volumes;
        for (const ModelVolume* volume : volumes)
            if (volume->is_model_part()) {
                if ((attribute == ModelObjectCutAttribute::KeepUpper && volume->is_from_upper()) ||
                    (attribute != ModelObjectCutAttribute::KeepUpper && !volume->is_from_upper())) {
                    ModelVolume* new_vol = object->add_volume(*volume);
                    new_vol->reset_from_upper();
                }
            }
    };

    auto cut = [this, add_volumes_from_cut]
                (ModelObject* object, const Transform3d& cut_matrix, const ModelObjectCutAttribute add_volumes_attribute, Model& tmp_model_for_cut) {
        Cut cut(object, m_instance, cut_matrix);

        tmp_model_for_cut = Model();
        tmp_model_for_cut.add_object(*cut.perform_with_plane().front());
        assert(!tmp_model_for_cut.objects.empty());

        object->clear_volumes();
        add_volumes_from_cut(object, add_volumes_attribute, tmp_model_for_cut);
        reset_instance_transformation(object, m_instance);
    };

    // cut by upper plane (+Z)
    {
        const Transform3d cut_matrix_upper = translation_transform(rotation_m * (groove_half_depth * Vec3d::UnitZ())) * m_cut_matrix;

        cut(tmp_object, cut_matrix_upper, ModelObjectCutAttribute::KeepLower, tmp_model_for_cut);
        add_volumes_from_cut(upper, ModelObjectCutAttribute::KeepUpper, tmp_model_for_cut);
    }

    // cut by lower plane (-Z)
    {
        const Transform3d cut_matrix_lower = translation_transform(rotation_m * (-groove_half_depth * Vec3d::UnitZ())) * m_cut_matrix;

        cut(tmp_object, cut_matrix_lower, ModelObjectCutAttribute::KeepUpper, tmp_model_for_cut);
        add_volumes_from_cut(lower, ModelObjectCutAttribute::KeepLower, tmp_model_for_cut);
    }

    // Compute same slot outer width used in preview plane
    const float  groove_width     = calculate_groove_width(groove, m_radius);

    ModelObject* groove_object{nullptr};

    // multiple cuts
    for (int i = 0; i < groove_count; i++) {
        bool is_first_groove = i == 0; 
        bool is_last_groove = i == groove_count - 1; 

        // Calculate the x-axis offset for this dovetail
        float groove_offset_factor_start = -.5 * ((groove_count - 1));
        float groove_offset_factor       = groove_offset_factor_start + i;

        float offset_x = groove_offset_factor * (groove_gap + groove_width);


        tmp_object->clone_for_cut(&groove_object);
        for (ModelVolume* volume : tmp_object->volumes) {
            ModelVolume* new_vol = groove_object->add_volume(*volume);
            new_vol->reset_from_upper();
        }

        // isolate area of current groove
        if (!is_first_groove) {
            float left_cut_position = (-groove_gap / 2.f) - (groove_width / 2.f) + offset_x;

            const Transform3d cut_matrix_left = translation_transform(rotation_m * (left_cut_position * Vec3d::UnitX())) *
                                                m_cut_matrix * rotation_transform(Vec3d(0, M_PI / 2.0, 0));

            cut(groove_object, cut_matrix_left, ModelObjectCutAttribute::KeepUpper, tmp_model_for_cut);
        }
        if (!is_last_groove) {
            float right_cut_position = (groove_gap / 2.f) + (groove_width / 2.f) + offset_x;

            const Transform3d cut_matrix_right = translation_transform(rotation_m * (right_cut_position * Vec3d::UnitX())) *
                                                 m_cut_matrix * rotation_transform(Vec3d(0, M_PI / 2.0, 0));
            cut(groove_object, cut_matrix_right, ModelObjectCutAttribute::KeepLower, tmp_model_for_cut);
        }

        const Transform3d groove_translation = translation_transform(rotation_m * (offset_x * Vec3d::UnitX()));
        // cut middle part with 2 angles and add parts to related upper/lower objects
        const double h_side_shift = 0.5 * double(groove.width + groove.depth / tan(groove.flaps_angle));

        // cut by angle1 plane
        {
            const Transform3d cut_matrix_angle1 = groove_translation * translation_transform(rotation_m * (-h_side_shift * Vec3d::UnitX())) *
                                                  m_cut_matrix * rotation_transform(Vec3d(0, -groove.flaps_angle, -groove.angle));

            cut(groove_object, cut_matrix_angle1, ModelObjectCutAttribute::KeepLower, tmp_model_for_cut);
            add_volumes_from_cut(lower, ModelObjectCutAttribute::KeepUpper, tmp_model_for_cut);
        }

        // cut by angle2 plane
        {
            const Transform3d cut_matrix_angle2 = groove_translation * translation_transform(rotation_m * (h_side_shift * Vec3d::UnitX())) *
                                                  m_cut_matrix * rotation_transform(Vec3d(0, groove.flaps_angle, groove.angle));

            cut(groove_object, cut_matrix_angle2, ModelObjectCutAttribute::KeepLower, tmp_model_for_cut);
            add_volumes_from_cut(lower, ModelObjectCutAttribute::KeepUpper, tmp_model_for_cut);
        }

        // apply tolerance to the middle part
        {
            const double h_groove_shift_tolerance = groove_half_depth - (double)groove.depth_tolerance;

            const Transform3d cut_matrix_lower_tolerance = groove_translation * translation_transform(rotation_m * (-h_groove_shift_tolerance * Vec3d::UnitZ())) *
                                                           m_cut_matrix;
            cut(groove_object, cut_matrix_lower_tolerance, ModelObjectCutAttribute::KeepUpper, tmp_model_for_cut);

            const double h_side_shift_tolerance = h_side_shift - 0.5 * double(groove.width_tolerance);

            const Transform3d cut_matrix_angle1_tolerance = groove_translation * translation_transform(rotation_m * (-h_side_shift_tolerance * Vec3d::UnitX())) *
                                                            m_cut_matrix * rotation_transform(Vec3d(0, -groove.flaps_angle, -groove.angle));
            cut(groove_object, cut_matrix_angle1_tolerance, ModelObjectCutAttribute::KeepLower, tmp_model_for_cut);

            const Transform3d cut_matrix_angle2_tolerance = groove_translation * translation_transform(rotation_m * (h_side_shift_tolerance * Vec3d::UnitX())) *
                                                            m_cut_matrix * rotation_transform(Vec3d(0, groove.flaps_angle, groove.angle));
            cut(groove_object, cut_matrix_angle2_tolerance, ModelObjectCutAttribute::KeepUpper, tmp_model_for_cut);
        }

        add_volumes_from_cut(upper, ModelObjectCutAttribute::KeepLower, tmp_model_for_cut);

        groove_object->clear_volumes();
    }

    ModelObjectPtrs cut_object_ptrs;

    if (keep_as_parts) {
        // add volumes from lower object to the upper, but mark them as a lower
        const auto& volumes = lower->volumes;
        for (const ModelVolume* volume : volumes) {
            ModelVolume* new_vol = upper->add_volume(*volume);
            new_vol->cut_info.is_from_upper = false;
        }

        // add modifiers
        for (const ModelVolume* volume : cut_mo->volumes)
            if (!volume->is_model_part())
                upper->add_volume(*volume);

        cut_object_ptrs.push_back(upper);

        // add lower object to the cut_object_ptrs just to correct delete it from the Model destructor and avoid memory leaks
        cut_object_ptrs.push_back(lower);
    }
    else {
        // add modifiers if object has any
        for (const ModelVolume* volume : cut_mo->volumes)
            if (!volume->is_model_part()) {
                distribute_modifiers_from_object(cut_mo, m_instance, upper, lower);
                break;
            }

        assert(!upper->volumes.empty() && !lower->volumes.empty());

        // Add Upper and Lower parts to cut_object_ptrs

        post_process(upper, lower, cut_object_ptrs);

        // Now merge all model parts together:
        merge_solid_parts_inside_object(cut_object_ptrs);
    }

    finalize(cut_object_ptrs, saved_paintings);

    return m_model.objects;
}

float Cut::calculate_groove_width (const Cut::Groove& groove, const float m_radius)
{
    // Compute same slot outer width used in preview plane
    const double flap_width             = is_approx(groove.flaps_angle, 0.f) ? groove.depth : groove.depth / sin(groove.flaps_angle);
    const double total_flap_width       = 2.0 * flap_width * cos(groove.flaps_angle);
    const double slot_neck_half_width   = 0.5f * (groove.width);
    const double slot_mouth_half_width  = 0.5 * (groove.width + total_flap_width);
    const double plane_half_height      = 0.5f* (1.5f * (1.5f *m_radius));
    const double flap_taper_offset      = plane_half_height * tan(groove.angle);
    const double slot_outer_x_max       = std::max(slot_mouth_half_width + flap_taper_offset, slot_neck_half_width + flap_taper_offset);

    return float(2.0 * slot_outer_x_max);
}

} // namespace Slic3r

