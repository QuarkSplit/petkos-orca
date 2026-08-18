#ifndef slic3r_GLGizmoKnife_hpp_
#define slic3r_GLGizmoKnife_hpp_

#include <memory>

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/MeshUtils.hpp"

namespace Slic3r {

class ModelVolume;
class TriangleMesh;

namespace GUI {

//A knife instead of a plane rig. Point at the mesh and the loop a cut would make is already
//there; scroll to rotate it about the axis you are looking along (look from above, it spins
//about Z - re-aim the camera to choose the axis); click to cut and drop back to the select
//tool; Ctrl+click to cut and keep the knife, stacking cuts that all land as parts together.
//
//The plane always contains the view direction, so on screen the knife reads as a straight
//line across the model. The cut is MeshKnife::planar_cut - the slicer's own layer cut turned
//sideways - so there is no boolean, no repair, and nothing to hang.
class GLGizmoKnife : public GLGizmoBase
{
    //One piece of the volume being cut. Before the first cut there is one fragment per part
    //volume of the object; every cut replaces the fragments it crosses with their halves.
    //The raycaster shares the mesh, so a fragment is cheap to carry.
    struct Fragment
    {
        std::shared_ptr<const TriangleMesh> mesh;
        std::unique_ptr<MeshRaycaster>      raycaster;
        int                                 source_volume_idx{-1};
    };

public:
    GLGizmoKnife(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);

    bool on_mouse(const wxMouseEvent &mouse_event) override;
    // Called from GLGizmosManager::on_mouse_wheel; steps are wheel notches, sign included.
    bool on_wheel(float steps);
    void data_changed(bool is_serializing) override;

protected:
    bool on_init() override;
    std::string on_get_name() const override;
    bool on_is_activable() const override;
    void on_render() override;
    void on_render_input_window(float x, float y, float bottom_limit) override;
    void on_set_state() override;
    CommonGizmosDataID on_get_requirements() const override;

private:
    void  build_fragments();
    void  reset_all();
    Vec3d view_axis() const; // the world axis the camera most looks along, signed
    void  update_hover(const Vec2d &mouse_position);
    void  rebuild_contour();
    bool  apply_cut();
    void  commit(bool close_after);

    std::vector<Fragment> m_fragments;
    const ModelObject    *m_cached_object{nullptr};
    int                   m_target_volume_idx{-1}; // locked by the first cut
    size_t                m_cuts_made{0};

    Vec3d m_plane_normal{Vec3d::UnitX()}; // world space, unit
    bool  m_normal_initialized{false};

    bool  m_hover_valid{false};
    Vec3d m_hover_point_world{Vec3d::Zero()};

    GLModel m_contour;
    bool    m_contour_dirty{false};

    Vec2d m_last_mouse{Vec2d::Zero()};
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoKnife_hpp_
