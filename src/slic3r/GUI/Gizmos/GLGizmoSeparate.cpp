#include "GLGizmoSeparate.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <glad/gl.h>

#include <cmath>

namespace Slic3r {
namespace GUI {

static const ColorRGBA SEPARATE_HOVER_COLOR  = {0.31f, 0.53f, 0.65f, 0.35f}; // arctic, tentative
static const ColorRGBA SEPARATE_PINNED_COLOR = {0.31f, 0.53f, 0.65f, 0.60f}; // arctic, locked

GLGizmoSeparate::GLGizmoSeparate(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoSeparate::on_init()
{
    return true;
}

std::string GLGizmoSeparate::on_get_name() const
{
    return _u8L("Separate");
}

bool GLGizmoSeparate::on_is_activable() const
{
    return m_parent.get_selection().is_single_full_instance();
}

CommonGizmosDataID GLGizmoSeparate::on_get_requirements() const
{
    return CommonGizmosDataID(int(CommonGizmosDataID::SelectionInfo) | int(CommonGizmosDataID::Raycaster));
}

void GLGizmoSeparate::on_set_state()
{
    if (m_state == Off) {
        reset_selection();
        m_caches.clear();
        m_cached_object       = nullptr;
        m_cached_volume_count = 0;
        m_feedback.clear();
    }
}

void GLGizmoSeparate::data_changed(bool is_serializing)
{
    // A different object, or the same object with a different volume list, obsoletes every
    // cached index and the selection with it.
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo != m_cached_object || (mo != nullptr && mo->volumes.size() != m_cached_volume_count)) {
        m_caches.clear();
        m_cached_object       = nullptr;
        m_cached_volume_count = 0;
        reset_selection();
    }
}

void GLGizmoSeparate::reset_selection()
{
    m_seed_mesh      = -1;
    m_seed_facet     = -1;
    m_pinned         = false;
    m_selection.clear();
    m_selected_count = 0;
    m_highlight.reset();
    m_highlight_dirty = false;
    m_rr              = RaycastResult{};
}

void GLGizmoSeparate::ensure_caches()
{
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr)
        return;
    if (mo == m_cached_object && mo->volumes.size() == m_cached_volume_count)
        return;

    m_caches.clear();
    // The order must mirror the Raycaster common data's: part volumes, in volume order.
    for (int idx = 0; idx < int(mo->volumes.size()); ++idx) {
        const ModelVolume *mv = mo->volumes[idx];
        if (!mv->is_model_part())
            continue;
        VolumeCache cache;
        cache.volume     = mv;
        cache.volume_idx = idx;
        cache.neighbors  = its_face_neighbors(mv->mesh().its);
        cache.normals    = its_face_normals(mv->mesh().its);
        m_caches.emplace_back(std::move(cache));
    }
    m_cached_object       = mo;
    m_cached_volume_count = mo->volumes.size();
}

void GLGizmoSeparate::update_hover(const Vec2d &mouse_position)
{
    if (m_pinned)
        return;
    if (m_rr.mouse_position == mouse_position)
        return;
    ensure_caches();
    if (m_caches.empty() || m_c->raycaster() == nullptr)
        return;

    const ModelObject   *mo = m_c->selection_info()->model_object();
    const ModelInstance *mi = mo->instances[m_c->selection_info()->get_active_instance()];
    const Camera        &camera = wxGetApp().plater()->get_camera();

    auto raycasters = m_c->raycaster()->raycasters();
    if (raycasters.size() != m_caches.size())
        return; // the common data has not caught up with the model yet

    m_rr.mouse_position = mouse_position;
    int    closest_mesh  = -1;
    size_t closest_facet = 0;
    double closest_distance = std::numeric_limits<double>::max();
    for (int mesh_id = 0; mesh_id < int(m_caches.size()); ++mesh_id) {
        const Transform3d trafo = mi->get_transformation().get_matrix() * m_caches[mesh_id].volume->get_matrix();
        Vec3f  hit, normal;
        size_t facet = 0;
        if (raycasters[mesh_id]->unproject_on_mesh(mouse_position, trafo, camera, hit, normal, nullptr, &facet)) {
            const double distance = (camera.get_position() - trafo * hit.cast<double>()).squaredNorm();
            if (distance < closest_distance) {
                closest_distance = distance;
                closest_mesh     = mesh_id;
                closest_facet    = facet;
            }
        }
    }
    m_rr.mesh_id = closest_mesh;
    m_rr.facet   = closest_facet;

    if (closest_mesh < 0) {
        if (!m_selection.empty()) {
            m_selection.clear();
            m_selected_count  = 0;
            m_highlight_dirty = true;
        }
        m_seed_mesh  = -1;
        m_seed_facet = -1;
        return;
    }
    //Crossability is a property of the edge, so the region is the connected component of the
    //seed: hovering anywhere inside the current region names the same region, and the fill
    //only reruns when the cursor actually leaves it.
    if (closest_mesh == m_seed_mesh && int(closest_facet) < int(m_selection.size()) && m_selection[closest_facet])
        return;
    m_seed_mesh  = closest_mesh;
    m_seed_facet = int(closest_facet);
    recompute_fill();
}

void GLGizmoSeparate::recompute_fill()
{
    if (m_seed_mesh < 0 || m_seed_mesh >= int(m_caches.size()) || m_seed_facet < 0) {
        m_selection.clear();
        m_selected_count  = 0;
        m_highlight_dirty = true;
        return;
    }
    const VolumeCache &cache = m_caches[m_seed_mesh];
    m_selection = MeshSeparator::flood_fill(cache.volume->mesh().its, cache.neighbors, cache.normals,
                                            m_seed_facet, m_params);
    m_selected_count  = std::count(m_selection.begin(), m_selection.end(), char(1));
    m_highlight_dirty = true;
}

void GLGizmoSeparate::on_render()
{
    if (m_seed_mesh < 0 || m_selected_count == 0 || m_seed_mesh >= int(m_caches.size()))
        return;

    if (m_highlight_dirty) {
        m_highlight.reset();
        const indexed_triangle_set &src = m_caches[m_seed_mesh].volume->mesh().its;
        indexed_triangle_set subset;
        std::vector<int> vertex_map(src.vertices.size(), -1);
        for (int f = 0; f < int(src.indices.size()); ++f) {
            if (!m_selection[f])
                continue;
            Vec3i32 tri;
            for (int j = 0; j < 3; ++j) {
                int &m = vertex_map[src.indices[f][j]];
                if (m < 0) {
                    m = int(subset.vertices.size());
                    subset.vertices.emplace_back(src.vertices[src.indices[f][j]]);
                }
                tri[j] = m;
            }
            subset.indices.emplace_back(tri);
        }
        m_highlight.init_from(subset);
        m_highlight_dirty = false;
    }
    if (!m_highlight.is_initialized())
        return;

    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const ModelObject   *mo = m_c->selection_info()->model_object();
    const ModelInstance *mi = mo->instances[m_c->selection_info()->get_active_instance()];
    const Camera        &camera = wxGetApp().plater()->get_camera();
    const Transform3d    world  = mi->get_transformation().get_matrix() * m_caches[m_seed_mesh].volume->get_matrix();

    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * world);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));
    // Pulled a whisker towards the camera, so the highlight wins the depth fight with the
    // very faces it is highlighting.
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(-1.f, -1.f));

    m_highlight.set_color(m_pinned ? SEPARATE_PINNED_COLOR : SEPARATE_HOVER_COLOR);
    m_highlight.render();

    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glDisable(GL_BLEND));
    shader->stop_using();
}

bool GLGizmoSeparate::on_mouse(const wxMouseEvent &mouse_event)
{
    const Vec2d mouse_position(mouse_event.GetX(), mouse_event.GetY());
    if (mouse_event.Moving()) {
        update_hover(mouse_position);
        return false;
    }
    if (mouse_event.LeftDown()) {
        // A click locks the region under the cursor; a click elsewhere re-locks there.
        m_pinned = false;
        m_rr     = RaycastResult{};
        update_hover(mouse_position);
        if (m_seed_mesh >= 0 && m_selected_count > 0) {
            m_pinned = true;
            m_feedback.clear();
            return true;
        }
        return false;
    }
    if (mouse_event.LeftUp())
        return m_pinned;
    return false;
}

void GLGizmoSeparate::perform_separation()
{
    if (!m_pinned || m_seed_mesh < 0 || m_seed_mesh >= int(m_caches.size()))
        return;
    const VolumeCache &cache = m_caches[m_seed_mesh];

    MeshSeparator::Result result;
    if (!MeshSeparator::separate(cache.volume->mesh().its, cache.neighbors, m_selection, result)) {
        m_feedback = result.error;
        return;
    }
    if (result.open_chains > 0)
        m_feedback = _u8L("The source mesh has holes along the seam; matching openings were left open.");
    else
        m_feedback = GUI::format(_L("Detached %1% mm³, keeping %2% mm³; both parts are watertight."),
                                 int(std::round(std::abs(result.volume_region))),
                                 int(std::round(std::abs(result.volume_rest))));

    wxGetApp().plater()->take_snapshot("Separate");

    ModelObject *mo      = m_c->selection_info()->model_object();
    ModelVolume *old     = mo->volumes[cache.volume_idx];
    const int    old_idx = cache.volume_idx;

    ModelVolume *rest_volume = mo->add_volume(*old, TriangleMesh(std::move(result.rest.mesh)));
    rest_volume->name        = old->name;
    rest_volume->set_new_unique_id();

    ModelVolume *region_volume = mo->add_volume(*old, TriangleMesh(std::move(result.region.mesh)));
    region_volume->name        = old->name + " - " + _u8L("separated");
    region_volume->set_new_unique_id();

    //The surface triangles ARE the input triangles, bit for bit - the separator says so and
    //records which is which in src_face - so every painted channel (filament colour, seams,
    //supports, fuzzy skin) crosses by index: exact, instant, finer than a facet. Cap faces
    //have no ancestor and stay unpainted. Without this the split silently stripped the paint
    //off both halves, which is losing material information to a geometry operation.
    auto carry_paint = [old](ModelVolume *new_volume, const MeshSeparator::Part &part) {
        std::vector<int> src_to_dst(old->mesh().its.indices.size(), -1);
        for (size_t f = 0; f < part.src_face.size(); ++f)
            if (part.src_face[f] >= 0)
                src_to_dst[size_t(part.src_face[f])] = int(f);
        new_volume->remap_painting_by_facets(src_to_dst, *old);
    };
    carry_paint(rest_volume, result.rest);
    carry_paint(region_volume, result.region);

    std::swap(mo->volumes[old_idx], mo->volumes.back());
    mo->delete_volume(mo->volumes.size() - 1);

    wxGetApp().plater()->update();
    wxGetApp().obj_list()->select_item([this, region_volume]() {
        wxDataViewItem sel_item;
        wxDataViewItemArray items = wxGetApp().obj_list()->reorder_volumes_and_get_selection(
            m_parent.get_selection().get_object_idx(),
            [region_volume](const ModelVolume *volume) { return volume == region_volume; });
        if (!items.IsEmpty())
            sel_item = items.front();
        return sel_item;
    });

    reset_selection();
}

void GLGizmoSeparate::on_render_input_window(float x, float y, float bottom_limit)
{
    y = std::min(y, bottom_limit - ImGui::GetWindowHeight());

    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    GizmoImguiBegin("Separate", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    const float slider_width = 200.f * m_parent.get_scale();

    m_imgui->text(_u8L("Angle threshold"));
    ImGui::PushItemWidth(slider_width);
    bool params_changed = m_imgui->bbl_slider_float_style("##separate_angle", &m_params.angle_threshold_deg, 1.f, 120.f, "%.0f");
    ImGui::PopItemWidth();
    params_changed |= m_imgui->bbl_checkbox(_L("Stop only at inward seams"), m_params.concave_only);
    if (params_changed && m_seed_facet >= 0)
        recompute_fill();

    if (m_selected_count > 0)
        m_imgui->text(GUI::format(_L("%1% faces selected"), m_selected_count));
    else
        m_imgui->text(_u8L("Hover the model; click to lock a region."));

    m_imgui->disabled_begin(!m_pinned || m_selected_count == 0);
    if (m_imgui->button(_L("Separate")))
        perform_separation();
    m_imgui->disabled_end();
    ImGui::SameLine();
    if (m_imgui->button(_L("Reset")))
        reset_selection();

    if (!m_feedback.empty())
        m_imgui->text(m_feedback);

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

} // namespace GUI
} // namespace Slic3r
