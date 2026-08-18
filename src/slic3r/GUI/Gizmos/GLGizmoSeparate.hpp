#ifndef slic3r_GLGizmoSeparate_hpp_
#define slic3r_GLGizmoSeparate_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLModel.hpp"

#include "libslic3r/MeshSeparator.hpp"

namespace Slic3r {

class ModelVolume;

namespace GUI {

//The paint bucket for solids, on screen. Hovering the model previews the region the bucket
//would take - every face reachable from the cursor without crossing a sharp edge - a click
//locks it, the slider re-fills it, and Separate detaches it as its own watertight part while
//sealing the remainder. All of it is MeshSeparator; this class is the mouse, the highlight
//and the panel.
class GLGizmoSeparate : public GLGizmoBase
{
    //Adjacency and normals for one part volume of the selected object, built once per volume
    //and reused by every hover: the fill is O(faces), but the index behind it need not be.
    struct VolumeCache
    {
        const ModelVolume   *volume{nullptr};
        int                  volume_idx{-1}; // index into the ModelObject's volumes
        std::vector<Vec3i32> neighbors;
        std::vector<Vec3f>   normals;
    };

    struct RaycastResult
    {
        Vec2d  mouse_position{Vec2d::Zero()};
        int    mesh_id{-1};
        size_t facet{0};
    };

public:
    GLGizmoSeparate(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);

    bool on_mouse(const wxMouseEvent &mouse_event) override;
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
    void ensure_caches();
    void reset_selection();
    void update_hover(const Vec2d &mouse_position);
    void recompute_fill();
    void perform_separation();

    std::vector<VolumeCache> m_caches;
    const ModelObject       *m_cached_object{nullptr};
    size_t                   m_cached_volume_count{0};

    MeshSeparator::FillParams m_params{45.f, true};
    int  m_seed_mesh{-1};
    int  m_seed_facet{-1};
    bool m_pinned{false};

    std::vector<char> m_selection;
    size_t            m_selected_count{0};
    GLModel           m_highlight;
    bool              m_highlight_dirty{false};
    std::string       m_feedback;

    RaycastResult m_rr;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoSeparate_hpp_
