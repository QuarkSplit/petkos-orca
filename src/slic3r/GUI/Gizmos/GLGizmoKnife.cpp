#include "GLGizmoKnife.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"

#include "libslic3r/MeshKnife.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <glad/gl.h>

#include <cmath>

namespace Slic3r {
namespace GUI {

static const ColorRGBA KNIFE_LOOP_COLOR = {0.31f, 0.53f, 0.65f, 0.9f}; // arctic accent

static const double WHEEL_STEP_RAD = 5.0 * M_PI / 180.0;

GLGizmoKnife::GLGizmoKnife(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoKnife::on_init()
{
    return true;
}

std::string GLGizmoKnife::on_get_name() const
{
    return _u8L("Knife");
}

bool GLGizmoKnife::on_is_activable() const
{
    return m_parent.get_selection().is_single_full_instance();
}

CommonGizmosDataID GLGizmoKnife::on_get_requirements() const
{
    return CommonGizmosDataID::SelectionInfo;
}

void GLGizmoKnife::on_set_state()
{
    if (m_state == On) {
        reset_all();
        build_fragments();
        // The first knife line runs vertically on screen: its plane's normal is the camera's
        // right vector, flattened against the axis scrolling will rotate about.
        const Camera &camera = wxGetApp().plater()->get_camera();
        const Vec3d   axis   = view_axis();
        Vec3d         n      = camera.get_dir_right() - camera.get_dir_right().dot(axis) * axis;
        m_plane_normal       = n.norm() > 1e-6 ? n.normalized() : Vec3d(axis.unitOrthogonal());
        m_normal_initialized = true;
    } else {
        //Leaving the tool with cuts stacked up still lands them: a cut the user made and saw
        //is not allowed to silently evaporate because they pressed Escape.
        if (m_cuts_made > 0)
            commit(false);
        reset_all();
    }
}

void GLGizmoKnife::data_changed(bool is_serializing)
{
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo != m_cached_object) {
        reset_all();
        if (m_state == On && mo != nullptr)
            build_fragments();
    }
}

void GLGizmoKnife::reset_all()
{
    m_fragments.clear();
    m_cached_object     = nullptr;
    m_target_volume_idx = -1;
    m_cuts_made         = 0;
    m_hover_valid       = false;
    m_contour.reset();
    m_contour_dirty = false;
}

void GLGizmoKnife::build_fragments()
{
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr)
        return;
    for (int idx = 0; idx < int(mo->volumes.size()); ++idx) {
        const ModelVolume *mv = mo->volumes[idx];
        if (!mv->is_model_part())
            continue;
        Fragment fragment;
        fragment.mesh              = mv->get_mesh_shared_ptr();
        fragment.raycaster         = std::make_unique<MeshRaycaster>(fragment.mesh);
        fragment.source_volume_idx = idx;
        m_fragments.emplace_back(std::move(fragment));
    }
    m_cached_object = mo;
}

Vec3d GLGizmoKnife::view_axis() const
{
    const Vec3d forward = wxGetApp().plater()->get_camera().get_dir_forward();
    int         axis    = 0;
    forward.cwiseAbs().maxCoeff(&axis);
    Vec3d result   = Vec3d::Zero();
    result[axis]   = forward[axis] > 0. ? 1. : -1.;
    return result;
}

//The fragment's world placement: fragments live in their source volume's local space, and a
//cut never moves a vertex, so the source volume's transform keeps fitting every piece of it.
static Transform3d fragment_trafo(const ModelObject *mo, int active_instance, int volume_idx)
{
    return mo->instances[active_instance]->get_transformation().get_matrix() *
           mo->volumes[volume_idx]->get_matrix();
}

void GLGizmoKnife::update_hover(const Vec2d &mouse_position)
{
    if (m_fragments.empty() || m_cached_object == nullptr)
        return;
    m_last_mouse = mouse_position;

    const int     active_instance = m_c->selection_info()->get_active_instance();
    const Camera &camera          = wxGetApp().plater()->get_camera();

    bool   found            = false;
    Vec3d  best_point_world = Vec3d::Zero();
    double best_distance    = std::numeric_limits<double>::max();
    for (const Fragment &fragment : m_fragments) {
        const Transform3d trafo = fragment_trafo(m_cached_object, active_instance, fragment.source_volume_idx);
        Vec3f  hit, normal;
        size_t facet = 0;
        if (fragment.raycaster->unproject_on_mesh(mouse_position, trafo, camera, hit, normal, nullptr, &facet)) {
            const Vec3d  world    = trafo * hit.cast<double>();
            const double distance = (camera.get_position() - world).squaredNorm();
            if (distance < best_distance) {
                best_distance    = distance;
                best_point_world = world;
                found            = true;
            }
        }
    }

    if (found != m_hover_valid || (found && (best_point_world - m_hover_point_world).norm() > 1e-9)) {
        m_hover_valid       = found;
        m_hover_point_world = best_point_world;
        m_contour_dirty     = true;
    }
}

void GLGizmoKnife::rebuild_contour()
{
    m_contour.reset();
    m_contour_dirty = false;
    if (!m_hover_valid || m_cached_object == nullptr)
        return;

    const int    active_instance = m_c->selection_info()->get_active_instance();
    const Camera &camera         = wxGetApp().plater()->get_camera();
    // Nudged towards the camera so the loop is not eaten by the surface it lies on.
    const Vec3d nudge = -camera.get_dir_forward() * 0.05;

    GLModel::Geometry geometry;
    geometry.format = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3};

    unsigned int vertex_count = 0;
    for (const Fragment &fragment : m_fragments) {
        if (m_target_volume_idx >= 0 && fragment.source_volume_idx != m_target_volume_idx)
            continue;
        const Transform3d trafo   = fragment_trafo(m_cached_object, active_instance, fragment.source_volume_idx);
        const Transform3d inverse = trafo.inverse();
        const Vec3f p_local = (inverse * m_hover_point_world).cast<float>();
        const Vec3f n_local = (trafo.linear().transpose() * m_plane_normal).normalized().cast<float>();

        for (const auto &[a, b] : MeshKnife::contour(fragment.mesh->its, p_local, n_local)) {
            const Vec3f world_a = (trafo * a.cast<double>() + nudge).cast<float>();
            const Vec3f world_b = (trafo * b.cast<double>() + nudge).cast<float>();
            geometry.add_vertex(world_a);
            geometry.add_vertex(world_b);
            geometry.add_line(vertex_count, vertex_count + 1);
            vertex_count += 2;
        }
    }
    if (vertex_count > 0)
        m_contour.init_from(std::move(geometry));
}

void GLGizmoKnife::on_render()
{
    if (m_contour_dirty)
        rebuild_contour();
    if (!m_contour.is_initialized())
        return;

    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const Camera &camera = wxGetApp().plater()->get_camera();
    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    // The whole loop stays visible through the model: a knife line you can only half see is
    // a guess, and this tool exists to end guessing.
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glLineWidth(2.0f));

    m_contour.set_color(KNIFE_LOOP_COLOR);
    m_contour.render();

    glsafe(::glLineWidth(1.0f));
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));
    shader->stop_using();
}

bool GLGizmoKnife::on_wheel(float steps)
{
    if (!m_hover_valid)
        return false; // off the mesh the wheel keeps meaning zoom
    m_plane_normal  = Eigen::AngleAxisd(double(steps) * WHEEL_STEP_RAD, view_axis()) * m_plane_normal;
    m_plane_normal.normalize();
    m_contour_dirty = true;
    return true;
}

bool GLGizmoKnife::on_mouse(const wxMouseEvent &mouse_event)
{
    const Vec2d mouse_position(mouse_event.GetX(), mouse_event.GetY());
    if (mouse_event.Moving()) {
        update_hover(mouse_position);
        return false;
    }
    if (mouse_event.LeftDown()) {
        update_hover(mouse_position);
        if (!m_hover_valid)
            return false;
        if (!apply_cut())
            return true; // consumed, but the plane grazed nothing
        if (mouse_event.ControlDown()) {
            // Keep the knife: the cut pieces are already the new fragments.
            m_contour_dirty = true;
            return true;
        }
        commit(true);
        return true;
    }
    if (mouse_event.LeftUp())
        return m_hover_valid;
    return false;
}

bool GLGizmoKnife::apply_cut()
{
    if (!m_hover_valid || m_cached_object == nullptr)
        return false;
    const int active_instance = m_c->selection_info()->get_active_instance();

    //The first cut chooses the victim: whichever volume the cursor is on. From then on the
    //knife only ever cuts that volume's pieces, so a multi-part object cannot be shredded by
    //accident.
    if (m_target_volume_idx < 0) {
        double best_distance = std::numeric_limits<double>::max();
        const Camera &camera = wxGetApp().plater()->get_camera();
        for (const Fragment &fragment : m_fragments) {
            const Transform3d trafo = fragment_trafo(m_cached_object, active_instance, fragment.source_volume_idx);
            Vec3f  hit, normal;
            size_t facet = 0;
            if (fragment.raycaster->unproject_on_mesh(m_last_mouse, trafo, camera, hit, normal, nullptr, &facet)) {
                const double distance = (camera.get_position() - trafo * hit.cast<double>()).squaredNorm();
                if (distance < best_distance) {
                    best_distance       = distance;
                    m_target_volume_idx = fragment.source_volume_idx;
                }
            }
        }
        if (m_target_volume_idx < 0)
            return false;
        m_fragments.erase(std::remove_if(m_fragments.begin(), m_fragments.end(),
                                         [this](const Fragment &f) { return f.source_volume_idx != m_target_volume_idx; }),
                          m_fragments.end());
    }

    const Transform3d trafo   = fragment_trafo(m_cached_object, active_instance, m_target_volume_idx);
    const Transform3d inverse = trafo.inverse();
    const Vec3f p_local = (inverse * m_hover_point_world).cast<float>();
    const Vec3f n_local = (trafo.linear().transpose() * m_plane_normal).normalized().cast<float>();

    std::vector<Fragment> next;
    next.reserve(m_fragments.size() + 1);
    size_t divided = 0;
    for (Fragment &fragment : m_fragments) {
        MeshKnife::CutResult result = MeshKnife::planar_cut(fragment.mesh->its, p_local, n_local);
        if (!result.cut) {
            next.emplace_back(std::move(fragment));
            continue;
        }
        ++divided;
        for (indexed_triangle_set *half : {&result.upper, &result.lower}) {
            Fragment piece;
            piece.mesh              = std::make_shared<const TriangleMesh>(std::move(*half));
            piece.raycaster         = std::make_unique<MeshRaycaster>(piece.mesh);
            piece.source_volume_idx = m_target_volume_idx;
            next.emplace_back(std::move(piece));
        }
    }
    if (divided == 0)
        return false;
    m_fragments = std::move(next);
    ++m_cuts_made;
    return true;
}

void GLGizmoKnife::commit(bool close_after)
{
    ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (m_cuts_made > 0 && m_fragments.size() > 1 && mo != nullptr && mo == m_cached_object && m_target_volume_idx >= 0) {
        wxGetApp().plater()->take_snapshot("Knife cut");

        ModelVolume *old = mo->volumes[m_target_volume_idx];

        ModelVolume *first_piece = nullptr;
        int          piece       = 0;
        for (const Fragment &fragment : m_fragments) {
            ModelVolume *v = mo->add_volume(*old, TriangleMesh(*fragment.mesh));
            v->name        = old->name + " - " + GUI::format(_L("cut %1%"), ++piece);
            v->set_new_unique_id();
            if (first_piece == nullptr)
                first_piece = v;
        }
        std::swap(mo->volumes[m_target_volume_idx], mo->volumes.back());
        mo->delete_volume(mo->volumes.size() - 1);

        m_cuts_made = 0; // committed; on_set_state(Off) must not commit again
        wxGetApp().plater()->update();
        wxGetApp().obj_list()->select_item([this, first_piece]() {
            wxDataViewItem sel_item;
            wxDataViewItemArray items = wxGetApp().obj_list()->reorder_volumes_and_get_selection(
                m_parent.get_selection().get_object_idx(),
                [first_piece](const ModelVolume *volume) { return volume == first_piece; });
            if (!items.IsEmpty())
                sel_item = items.front();
            return sel_item;
        });
    }
    m_cuts_made = 0;

    if (close_after)
        // Toggling the current gizmo closes it; deferred so the click that asked for it
        // finishes its own dispatch first.
        wxGetApp().CallAfter([this]() {
            if (m_parent.get_gizmos_manager().get_current_type() == GLGizmosManager::EType::Knife)
                m_parent.get_gizmos_manager().open_gizmo(GLGizmosManager::EType::Knife);
        });
}

void GLGizmoKnife::on_render_input_window(float x, float y, float bottom_limit)
{
    y = std::min(y, bottom_limit - ImGui::GetWindowHeight());

    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    GizmoImguiBegin("Knife", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    const Vec3d axis = view_axis();
    int         axis_idx = 0;
    axis.cwiseAbs().maxCoeff(&axis_idx);
    m_imgui->text(GUI::format(_L("Scroll rotates the cut about %1%"), std::string(1, "XYZ"[axis_idx])));
    m_imgui->text(_u8L("Click: cut.  Ctrl+Click: cut and keep cutting."));
    if (m_cuts_made > 0)
        m_imgui->text(GUI::format(_L("%1% cut(s) made - the next plain click finishes."), m_cuts_made));

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

} // namespace GUI
} // namespace Slic3r
