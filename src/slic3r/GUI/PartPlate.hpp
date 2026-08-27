#ifndef __part_plate_hpp_
#define __part_plate_hpp_

#include <vector>
#include <set>
#include <array>
#include <map>
#include <memory>
#include <thread>
#include <mutex>

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/Arrange.hpp"
#include "Plater.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "GLCanvas3D.hpp"
#include "GLTexture.hpp"
#include "3DScene.hpp"
#include "GLModel.hpp"
#include "3DBed.hpp"
#include "MeshUtils.hpp"
#include "libslic3r/ParameterUtils.hpp"
#include "libslic3r/PlateSlicingContext.hpp"

class GLUquadric;
typedef class GLUquadric GLUquadricObject;


// use PLATE_CURRENT_IDX stands for using current plate
// and use PLATE_ALL_IDX
#define PLATE_CURRENT_IDX   -1
#define PLATE_ALL_IDX       -2

#define MAX_PLATE_COUNT     36

inline int compute_colum_count(int count)
{
    float value = sqrt((float)count);
    float round_value = round(value);
    int cols;

    if (value > round_value)
        cols = round_value +1;
    else
        cols = round_value;

    return cols;
}


extern const float WIPE_TOWER_DEFAULT_X_POS;
extern const float WIPE_TOWER_DEFAULT_Y_POS;  // Max y

extern const float I3_WIPE_TOWER_DEFAULT_X_POS;
extern const float I3_WIPE_TOWER_DEFAULT_Y_POS; // Max y



namespace Slic3r {

class Model;
class ModelObject;
class ModelInstance;
class Print;
class SLAPrint;

namespace GUI {
class Plater;
class GLCanvas3D;
struct Camera;
class PartPlateList;

using GCodeResult = GCodeProcessorResult;

//Everything a plate needs to stand in for a specific printer's bed: the outline,
//the carve-outs, the multi-extruder areas, how tall the machine can print, and the
//stl Bed3D should draw underneath. Filled by PartPlateList::resolve_printer_bed.
struct PlateBed
{
    Pointfs              shape;
    Pointfs              exclude_areas;
    Pointfs              wrapping_exclude_areas;
    std::vector<Pointfs> extruder_areas;
    std::vector<double>  extruder_heights;
    double               printable_height { 0.0 };
    std::string          bed_model;   //may be empty; Bed3D then draws a plain bed
    std::string          bed_texture; //may be empty; the plate then draws a plain surface
};

class PartPlate : public ObjectBase
{
public:
    enum HeightLimitMode{
        HEIGHT_LIMIT_NONE,
        HEIGHT_LIMIT_BOTTOM,
        HEIGHT_LIMIT_TOP,
        HEIGHT_LIMIT_BOTH
    };

private:
    PartPlateList* m_partplate_list {nullptr };
    Plater* m_plater; //Plater reference, not own it
    Model* m_model; //Model reference, not own it
    PrinterTechnology  printer_technology;

    std::set<std::pair<int, int>> obj_to_instance_set;
    std::set<std::pair<int, int>> instance_outside_set;
    int m_plate_index;
    Vec3d m_origin;
    int m_width;
    int m_depth;
    int m_height;
    float m_height_to_lid;
    float m_height_to_rod;
    bool m_printable;
    bool m_locked;
    bool m_ready_for_slice;
    bool m_slice_result_valid;
    bool m_apply_invalid {false};
    float m_slice_percent;

    Print *m_print; //Print reference, not own it, no need to serialize
    GCodeProcessorResult *m_gcode_result;
    std::vector<FilamentInfo> slice_filaments_info;
    int m_print_index;

    //this plate's own bed texture (from its printer), and the one it last actually drew -
    //the latter bridges the frames where a newly wanted texture is still compressing, so a
    //printer change never shows an empty black bed
    std::string m_logo_texture_file;
    std::string m_logo_texture_shown;

    std::string m_tmp_gcode_path;       //use a temp path to store the gcode
    std::string m_temp_config_3mf_path; //use a temp path to store the config 3mf
    std::string m_gcode_path_from_3mf;  //use a path to store the gcode loaded from 3mf

    friend class PartPlateList;

    Pointfs m_shape;
    Pointfs m_exclude_area;
    std::vector<Pointfs> m_extruder_areas;
    std::vector<double> m_extruder_heights;
    // The bed shape as supplied by the printer profile, before it is translated
    // into world space by the plate origin. Kept so a plate can be repositioned
    // (or resized independently of its neighbours) without the caller having to
    // hand us the profile geometry again.
    Pointfs m_shape_local;
    Pointfs m_exclude_area_local;
    Pointfs m_wrapping_exclude_area_local;
    std::vector<Pointfs> m_extruder_areas_local;
    BoundingBoxf3 m_bounding_box;
    BoundingBoxf3 m_extended_bounding_box;
    mutable std::vector<BoundingBoxf3> m_exclude_bounding_box;
    mutable BoundingBoxf3 m_grabber_box;
    Transform3d m_grabber_trans_matrix;
    Slic3r::Geometry::Transformation position;
    std::vector<Vec3f> positions;
    ExPolygon m_print_polygon;
    PickingModel m_triangles;
    GLModel m_exclude_triangles;
    GLModel m_wrapping_detection_triangles;
    GLModel m_logo_triangles;
    GLModel m_gridlines;
    GLModel m_gridlines_bolder;
    GLModel m_height_limit_common;
    GLModel m_height_limit_bottom;
    GLModel m_height_limit_top;
    PickingModel m_del_icon;
    PickingModel m_arrange_icon;
    PickingModel m_orient_icon;
    PickingModel m_lock_icon;
    PickingModel m_plate_settings_icon;
    PickingModel m_plate_filament_map_icon;
    PickingModel m_plate_name_edit_icon;
    PickingModel m_move_front_icon;
    GLModel m_plate_idx_icon;
    GLTexture m_texture;

    // Every GL buffer above is authored in this plate's own frame, with its bed origin
    // at (0,0), and drawn through this matrix. Position is where a plate IS, not part
    // of what it is shaped like: baking the origin into the vertices made every move a
    // re-derivation of the outline, grid, icons and raycasters. A move now writes one
    // matrix.
    Transform3d m_model_matrix{ Transform3d::Identity() };
    // The bed raycasters handed to the canvas, so a move can re-aim them instead of
    // rebuilding their AABB trees. Weak because the canvas owns them and drops the lot
    // on a scene reset.
    std::vector<std::weak_ptr<SceneRaycasterItem>> m_picking_items;

    float m_scale_factor{ 1.0f };
    GLUquadricObject* m_quadric;
    int m_hover_id;
    bool m_selected;
    int m_timelapse_warning_code = 0;

    // BBS
    DynamicPrintConfig m_config;
    // Exact effective configuration that produced the retained G-code. Live
    // plate overrides are resolved separately and never overwrite this snapshot.
    DynamicPrintConfig m_sliced_config;
    //Why a retained slice this plate ARRIVED with was dropped at load (version skew, missing
    //presets). Non-empty only while nothing is attached: any freshly set sliced config clears
    //it, because the reason describes a slice that no longer exists, not this one.
    std::string m_sliced_config_dropped_reason;

    // Plate-owned context, and the only holder of it. An empty field is not
    // inheritance - there is no project printer behind it - it is a plate that has not
    // been given one yet, which PartPlateList::complete_plate_contexts fixes at the
    // moment the plate comes into being. See PlateSlicingContext.hpp.
    std::string m_printer_preset_name;
    std::string m_printer_vendor_id;
    std::string m_print_preset_name;
    std::vector<std::string> m_filament_preset_names;
    std::vector<std::string> m_filament_colours;
    std::string m_physical_printer_id;

    // Printable height of this plate's own printer, 0 meaning it has not been applied
    // yet. Applied inside set_pos_and_size so that every code path that (re)sizes this
    // plate - reflow, delete, reassignment - keeps the machine's own height instead of
    // stamping the list-wide one back on.
    double m_printable_height { 0.0 };

    // SoftFever
    // part plate name
    std::string m_name;
    GLModel m_plate_name_icon;
    GLTexture m_name_texture;
    wxCoord m_name_texture_width;
    wxCoord m_name_texture_height;

    void init();
    bool valid_instance(int obj_id, int instance_id) const;
    void generate_print_polygon(ExPolygon &print_polygon);
    void generate_exclude_polygon(ExPolygon &exclude_polygon);
    void generate_logo_polygon(ExPolygon &logo_polygon);
    void calc_bounding_boxes() const;
    // Put this plate's fixed geometry at m_origin: the world-space point lists the rest
    // of the app reads, the bounding boxes derived from them, the render matrix and the
    // registered raycasters. All O(shape points); no buffer is rebuilt.
    void apply_placement();
    // Rebuild every GL buffer from the local profile. Only a change of bed shape or of
    // the height rods needs this; a change of position never does.
    void rebuild_geometry();
    void calc_triangles(const ExPolygon& poly);
    void calc_exclude_triangles(const ExPolygon& poly);
    void calc_triangles_from_polygon(const ExPolygon &poly, GLModel& render_model);
    void calc_gridlines(const ExPolygon& poly, const BoundingBox& pp_bbox);
    void calc_height_limit();
    void calc_vertex_for_number(int index, bool one_number, GLModel &buffer);
    void calc_vertex_for_plate_name_edit_icon(GLTexture *texture, int index, PickingModel &model);
    void calc_vertex_for_icons(int index, PickingModel &model);
    // void calc_vertex_for_icons_background(int icon_count, GLModel &buffer);
    void render_background(bool force_default_color = false);
    void render_logo(bool bottom, bool render_cali = true, bool pale = false);
    void render_logo_texture(GLTexture &logo_texture, GLModel &logo_buffer, bool bottom, float opacity = 1.0f);

    //This plate's own bed texture, resolved from its printer alongside the bed shape. Empty
    //falls back to the list-wide filename, which is the pre-per-plate behaviour.
    void set_logo_texture_file(const std::string &filename) { m_logo_texture_file = filename; }
    const std::string &get_logo_texture_file() const { return m_logo_texture_file; }
    void render_exclude_area(bool force_default_color);
    //void render_background_for_picking(const ColorRGBA render_color) const;
    void render_grid(bool bottom);
    void render_wrapping_detection_area(bool force_default_color);
    void render_height_limit(PartPlate::HeightLimitMode mode = HEIGHT_LIMIT_BOTH);
    // void render_label(GLCanvas3D& canvas) const;
    // void render_grabber(const ColorRGBA render_color, bool use_lighting) const;
    // void render_face(float x_size, float y_size) const;
    // void render_arrows(const ColorRGBA render_color, bool use_lighting) const;
    // void render_left_arrow(const ColorRGBA render_color, bool use_lighting) const;
    // void render_right_arrow(const ColorRGBA render_color, bool use_lighting) const;
    void render_icon_texture(GLModel &buffer, GLTexture &texture);
    void show_tooltip(const std::string tooltip);
    void render_icons(bool bottom, bool only_name = false, int hover_id = -1);
    void render_only_numbers(bool bottom);
    void render_plate_name_texture();
    void invalidate_plate_name_texture();
    void register_raycasters_for_picking(GLCanvas3D& canvas);
    void register_model_for_picking(GLCanvas3D& canvas, PickingModel& model, int id);
    int picking_id_component(int idx) const;

    void on_filament_map_mode_change();

public:
    static constexpr unsigned int PLATE_NAME_HOVER_ID = 6;
    static constexpr unsigned int PLATE_FILAMENT_MAP_ID = 8;
    static constexpr unsigned int GRABBER_COUNT = 9;

    static ColorRGBA SELECT_COLOR;
    static ColorRGBA UNSELECT_COLOR;
    static ColorRGBA UNSELECT_DARK_COLOR;
    static ColorRGBA DEFAULT_COLOR;
    static ColorRGBA LINE_BOTTOM_COLOR;
    static ColorRGBA LINE_TOP_COLOR;
    static ColorRGBA LINE_TOP_DARK_COLOR;
    static ColorRGBA LINE_TOP_SEL_COLOR;
    static ColorRGBA LINE_TOP_SEL_DARK_COLOR;
    static ColorRGBA HEIGHT_LIMIT_BOTTOM_COLOR;
    static ColorRGBA HEIGHT_LIMIT_TOP_COLOR;

    static void update_render_colors();
    static void load_render_colors();

    PartPlate();
    PartPlate(PartPlateList *partplate_list, Vec3d origin, int width, int depth, int height, Plater* platerObj, Model* modelObj, bool printable=true, PrinterTechnology tech = ptFFF);
    ~PartPlate();

    bool operator<(PartPlate&) const;

    //clear alll the instances in plate
    void clear(bool clear_sliced_result = true);

    BedType get_bed_type(bool load_from_project = false) const;
    void set_bed_type(BedType bed_type);
    void reset_bed_type();

    void reset_skirt_start_angle();

    DynamicPrintConfig* config() { return &m_config; }
    const DynamicPrintConfig* config() const { return &m_config; }

    // set print sequence per plate
    //bool print_seq_same_global = true;
    void set_print_seq(PrintSequence print_seq = PrintSequence::ByDefault);
    PrintSequence get_print_seq() const;
    // Get the real effective print sequence of current plate.
    // If curr_plate's print_seq is ByDefault, use the global sequence
    // @return PrintSequence::{ByLayer,ByObject}
    PrintSequence get_real_print_seq(bool* plate_same_as_global=nullptr) const;

    std::vector<int> get_real_filament_maps(const DynamicConfig& g_config, bool* use_global_param = nullptr)const;
    std::vector<int> get_real_filament_volume_maps(const DynamicConfig& g_config, bool* use_global_param = nullptr) const;
    FilamentMapMode  get_real_filament_map_mode(const DynamicConfig& g_config,bool * use_global_param = nullptr) const;

    FilamentMapMode get_filament_map_mode() const;
    void set_filament_map_mode(const FilamentMapMode& mode);

    // get filament map, 0 based filament ids, 1 based extruder ids
    std::vector<int> get_filament_maps() const;
    void set_filament_maps(const std::vector<int>& f_maps);

    // per-filament nozzle-volume choice (NozzleVolumeType values, 0 based filament ids)
    std::vector<int> get_filament_volume_maps() const;
    void set_filament_volume_maps(const std::vector<int>& f_maps);
    void clear_filament_volume_map();

    // per-filament nozzle-group choice (0 based filament and nozzle ids)
    std::vector<int> get_filament_nozzle_maps() const;
    void set_filament_nozzle_maps(const std::vector<int>& f_maps);

    void clear_filament_map();
    void clear_filament_map_mode();

    bool has_spiral_mode_config() const;
    bool get_spiral_vase_mode() const;
    void set_spiral_vase_mode(bool spiral_mode, bool as_global);

    std::vector<Vec2d> get_plate_wrapping_detection_area() const;

    //static const int plate_x_offset = 20; //mm
    //static const double plate_x_gap = 0.2;
    ThumbnailData thumbnail_data;
    ThumbnailData no_light_thumbnail_data;
    ThumbnailData obj_preview_thumbnail_data;
    static const int plate_thumbnail_width = 512;
    static const int plate_thumbnail_height = 512;

    ThumbnailData top_thumbnail_data;
    ThumbnailData pick_thumbnail_data;

    //ThumbnailData cali_thumbnail_data;
    PlateBBoxData cali_bboxes_data;
    //static const int cali_thumbnail_width = 2560;
    //static const int cali_thumbnail_height = 2560;

    //set the plate's index
    void set_index(int index);

    //get the plate's index
    int get_index() const { return m_plate_index; }

    // SoftFever
    //get the plate's name
    std::string get_plate_name() const { return m_name; }
    void generate_plate_name_texture();
    //set the plate's name
    void set_plate_name(const std::string& name);

    //The printer this plate prints on. Never empty in a live project: see
    //PartPlateList::complete_plate_contexts.
    const std::string& get_printer_preset_name() const { return m_printer_preset_name; }
    void set_printer_preset_name(const std::string& name)
    {
        if (m_printer_preset_name == name)
            return;
        m_printer_preset_name = name;
        m_printer_vendor_id.clear();
        //the plate renders its machine next to its name, so the name texture is stale now
        invalidate_plate_name_texture();
    }
    //True once the plate has been given a printer, which every live plate has. Kept as a
    //name for the question rather than inlined, because the two moments where it is
    //genuinely false - a plate mid-construction, and a project written before per-plate
    //machines - are the two moments completion exists for.
    bool has_printer_assignment() const { return !m_printer_preset_name.empty(); }
    bool has_complete_context() const { return get_slicing_context().is_complete(); }

    //How many PROCESS settings this plate carries of its own, on top of the process it
    //names. Not a count of every key in m_config: bed type, print sequence, spiral mode and
    //the filament maps have had their own per-plate controls since long before this and
    //have their own rows in the UI, so counting them here would report a plate as edited
    //for having a bed type.
    //
    //It exists because a plate that names "0.20 Standard" and then overrides twelve of its
    //values is not describing itself by the name alone. A translation from another machine
    //produces exactly that state, and a row that showed only the name would be a half-truth
    //about settings somebody chose.
    size_t process_override_count() const;
    std::vector<std::string> process_override_keys() const;
    //Drop them. The user's own choice, offered because a carried set is still a set of
    //values they may not want; never done on their behalf.
    void clear_process_overrides();

    PlateSlicingContext get_slicing_context() const
    {
        return {m_printer_preset_name, m_printer_vendor_id, m_print_preset_name, m_filament_preset_names, m_filament_colours, m_physical_printer_id};
    }
    void set_slicing_context(const PlateSlicingContext &context)
    {
        const bool printer_changed = m_printer_preset_name != context.printer_preset_name;
        m_printer_preset_name      = context.printer_preset_name;
        m_printer_vendor_id        = context.printer_vendor_id;
        m_print_preset_name        = context.print_preset_name;
        m_filament_preset_names    = context.filament_preset_names;
        m_filament_colours         = context.filament_colours;
        m_physical_printer_id      = context.physical_printer_id;
        if (printer_changed)
            invalidate_plate_name_texture();
    }
    const std::string& get_print_preset_name() const { return m_print_preset_name; }
    const std::string& get_printer_vendor_id() const { return m_printer_vendor_id; }
    const std::vector<std::string>& get_filament_preset_names() const { return m_filament_preset_names; }
    const std::vector<std::string>& get_filament_colours() const { return m_filament_colours; }
    const std::string& get_physical_printer_id() const { return m_physical_printer_id; }

    //How tall this plate can print: its printer's height once the bed has been applied,
    //and whatever the plate was last sized to before that.
    double get_printable_height() const { return m_printable_height > 0.0 ? m_printable_height : (double)m_height; }
    void set_printable_height(double height) { m_printable_height = height; }

    void set_timelapse_warning_code(int code) { m_timelapse_warning_code = code; }
    int  timelapse_warning_code() { return m_timelapse_warning_code; }
    
    //get the print's object, result and index
    void get_print(PrintBase **print, GCodeResult **result, int *index);

    //set the print object, result and it's index
    void set_print(PrintBase *print, GCodeResult* result = nullptr, int index = -1);

    //get gcode filename
    std::string get_gcode_filename();

    bool is_valid_gcode_file();

    //get the plate's center point origin
    Vec3d get_center_origin();
    /* size and position related functions*/
    //set position and size
    void set_pos_and_size(Vec3d& origin, int width, int depth, int height, bool with_instance_move, bool do_clear = true);

    // BBS
    Vec2d get_size() const { return Vec2d(m_width, m_depth); }
    ModelObjectPtrs get_objects() { return m_model->objects; }
    ModelObjectPtrs get_objects_on_this_plate();
    ModelInstance* get_instance(int obj_id, int instance_id);
    BoundingBoxf3 get_objects_bounding_box();

    Vec3d get_origin() { return m_origin; }
    //Vec3d calculate_wipe_tower_size(const DynamicPrintConfig &config, const double w, const double wipe_volume, int plate_extruder_size = 0, bool use_global_objects = false) const;
    Vec3d estimate_wipe_tower_size(const DynamicPrintConfig & config, const double w, const double wipe_volume, int extruder_count = 1, int plate_extruder_size = 0, bool use_global_objects = false, bool enable_wrapping_detection = false) const;
    arrangement::ArrangePolygon estimate_wipe_tower_polygon(const DynamicPrintConfig & config, int plate_index, Vec3d& wt_pos, Vec3d& wt_size, int extruder_count = 1, int plate_extruder_size = 0, bool use_global_objects = false) const;
    bool check_objects_empty_and_gcode3mf(std::vector<int> &result) const;
    // get used filaments from config, 1 based idx
    std::vector<int> get_extruders(bool conside_custom_gcode = false) const;
    // Whether any object on this plate carries per-triangle filament painting. Paint states
    // are stored slot numbers and are deliberately never clipped, so the single-filament
    // slicing cut-down must not run over a painted plate unless the paint is all in slot 1.
    bool has_mmu_painted_object() const;
    std::vector<int> get_extruders_under_cli(bool conside_custom_gcode, DynamicPrintConfig& full_config) const;
    std::vector<int> get_extruders_without_support(bool conside_custom_gcode = false) const;
    // get used filaments from gcode result, 1 based idx
    std::vector<int> get_used_filaments();
    const std::vector<FilamentInfo>& get_slice_filaments_info() const { return slice_filaments_info; }
    int  get_physical_extruder_by_filament_id(const DynamicConfig& g_config, int idx) const;
    int  get_logical_extruder_by_filament_id(const DynamicConfig& g_config, int idx) const;
    bool check_filament_printable(const DynamicPrintConfig & config, wxString& error_message);
    bool check_tpu_printable_status(const DynamicPrintConfig & config, const std::vector<int> &tpu_filaments);
    bool check_mixture_of_pla_and_petg(const DynamicPrintConfig & config);
    bool check_mixture_filament_compatible(const DynamicPrintConfig& config, std::string &error_msg);
    bool check_compatible_of_nozzle_and_filament(const DynamicPrintConfig & config, const std::vector<std::string>& filament_presets, std::string& error_msg);

    /* instance related operations*/
    //judge whether instance is bound in plate or not
    bool contain_instance(int obj_id, int instance_id);
    bool contain_instance_totally(ModelObject* object, int instance_id) const;
    //judge whether instance is totally included in plate or not
    bool contain_instance_totally(int obj_id, int instance_id) const;

    //judge whether the plate's origin is at the left of instance or not
    bool is_left_top_of(int obj_id, int instance_id);

    //check whether instance is outside the plate or not
    bool check_outside(int obj_id, int instance_id, BoundingBoxf3* bounding_box = nullptr);

    //judge whether instance is intesected with plate or not
    bool intersect_instance(int obj_id, int instance_id, BoundingBoxf3* bounding_box = nullptr);

    //add an instance into plate
    int add_instance(int obj_id, int instance_id, bool move_position, BoundingBoxf3* bounding_box = nullptr);

    //remove instance from plate
    int remove_instance(int obj_id, int instance_id);

    //translate instance on the plate
    void translate_all_instance(Vec3d position);

    //duplicate all instance for count
    void duplicate_all_instance(unsigned int dup_count, bool need_skip, std::map<int, bool>& skip_objects);

    //update instance exclude state
    void update_instance_exclude_status(int obj_id, int instance_id, BoundingBoxf3* bounding_box = nullptr);

    //Re-run the outside-the-bed check for every instance on this plate against the
    //plate's current bed, refreshing instance_outside_set and the ready-for-slice
    //state. Needed after the bed changes under the parts (per-plate printer
    //reassignment): the parts keep their world coordinates, so nothing else would
    //notice they no longer fit. Returns the instances now outside so the caller can
    //name the objects instead of failing silently.
    std::vector<std::pair<int, int>> update_instances_outside_state();

    //update object's index caused by original object deleted
    void update_object_index(int obj_idx_removed, int obj_idx_max);

    // set objects configs when enabling spiral vase mode.
    void set_vase_mode_related_object_config(int obj_id = -1);

    //whether it is empty
    bool empty() { return obj_to_instance_set.empty(); }

    //How many instances sit on this plate, and whether any of them fell off its bed.
    //Read-only, so a reporting caller can say both without reaching into the sets.
    int  instance_count() const { return (int) obj_to_instance_set.size(); }
    bool has_instances_outside() const { return !instance_outside_set.empty(); }

    int printable_instance_size();

    //whether it is has printable instances
    bool has_printable_instances();
    bool is_all_instances_unprintable();

    //move instances to left or right PartPlate
    void move_instances_to(PartPlate& left_plate, PartPlate& right_plate, BoundingBoxf3* bounding_box = nullptr);

    /*rendering related functions*/
    const Pointfs& get_shape() const { return m_shape; }
    bool set_shape(const Pointfs& shape, const Pointfs& exclude_areas, const std::vector<Pointfs>& extruder_areas, const std::vector<double>& extruder_heights, Vec2d position, float height_to_lid, float height_to_rod);

    // The untranslated profile geometry backing this plate.
    const Pointfs& get_local_shape() const { return m_shape_local; }
    const Pointfs& get_local_exclude_area() const { return m_exclude_area_local; }
    const Pointfs& get_local_wrapping_exclude_area() const { return m_wrapping_exclude_area_local; }
    void set_local_wrapping_exclude_area(const Pointfs& area) { m_wrapping_exclude_area_local = area; m_wrapping_detection_triangles.reset(); }
    const std::vector<Pointfs>& get_local_extruder_areas() const { return m_extruder_areas_local; }
    // Footprint of this plate's own bed, independent of any neighbour.
    Vec2d get_local_size() const;
    // This plate's own frame in world space. Every buffer it owns is built at the
    // origin and drawn through this.
    const Transform3d& get_model_matrix() const { return m_model_matrix; }
    const std::vector<Pointfs>& get_extruder_areas() const { return m_extruder_areas; }
    const std::vector<double>& get_extruder_heights() const { return m_extruder_heights; }
    bool contains(const Vec3d& point) const;
    bool contains(const GLVolume& v) const;
    bool contains(const BoundingBoxf3& bb) const;
    bool intersects(const BoundingBoxf3& bb) const;

    void render(const Transform3d& view_matrix, const Transform3d& projection_matrix, bool bottom, bool only_body = false, bool force_background_color = false, HeightLimitMode mode = HEIGHT_LIMIT_NONE, int hover_id = -1, bool render_cali = false, bool show_grid = true);

    void set_selected();
    void set_unselected();
    void set_hover_id(int id) { m_hover_id = id; }
    const BoundingBoxf3& get_bounding_box(bool extended = false) { return extended ? m_extended_bounding_box : m_bounding_box; }
    const BoundingBox get_bounding_box_crd();
    BoundingBoxf3 get_plate_box() {return get_build_volume();}
    BoundingBoxf3 get_build_volume(bool use_share = false);

    const std::vector<BoundingBoxf3>& get_exclude_areas() { return m_exclude_bounding_box; }


    /*status related functions*/
    //update status
    void update_states();

    //is locked or not
    bool is_locked() const { return m_locked; }
    void lock(bool state) { m_locked = state; }

    //is a printable plate or not
    bool is_printable() const { return m_printable; }

    //can be sliced or not
    bool can_slice() const
    {
        return m_ready_for_slice && !m_apply_invalid;
    }
    void update_slice_ready_status(bool ready_slice)
    {
        m_ready_for_slice = ready_slice;
    }

    //bedtype mismatch or not
    bool is_apply_result_invalid() const
    {
        return m_apply_invalid;
    }
    void update_apply_result_invalid(bool invalid)
    {
        m_apply_invalid = invalid;
    }

    //is slice result valid or not
    bool is_slice_result_valid() const;
    bool has_retained_slice_result() const { return m_slice_result_valid && !m_sliced_config.empty(); }
    // Compose this plate's exact effective slicing configuration: its resolved context
    // (printer / process / filament presets, project row, filament maps) with this plate's
    // own overrides applied on top. Pure query - it mutates no plate state, and on failure
    // it names the reason so a caller that has somewhere to say it can name the plate too.
    //
    // This is the single basis on which a retained slice is judged current. It is what
    // apply_plate_config hands to the engine, and it is what is captured when a slice
    // completes. A Print's full_print_config() is deliberately NOT that basis: see
    // update_slice_result_valid_state.
    bool compose_slicing_config(DynamicPrintConfig &config, std::string &error) const;
    //Print time and filament weight of THIS plate's own retained slice. Both are read
    //from the plate's GCodeResult, so they survive a reopen and never report another
    //plate's figures; the Print object they used to come from is rebuilt per slice.
    bool get_retained_print_statistics(float &print_time_seconds, double &weight_grams) const;
    void set_sliced_config(const DynamicPrintConfig &config) { m_sliced_config = config; m_sliced_config_dropped_reason.clear(); }
    const DynamicPrintConfig &get_sliced_config() const { return m_sliced_config; }
    const std::string &sliced_config_dropped_reason() const { return m_sliced_config_dropped_reason; }
    void set_sliced_config_dropped_reason(const std::string &reason) { m_sliced_config_dropped_reason = reason; }

    //is slice result ready for print
    bool is_slice_result_ready_for_print() const { return is_slice_result_ready_for_print(is_slice_result_valid()); }
    // Overload for a caller that already holds the validity answer. is_slice_result_valid() is
    // no longer the bool read it used to be: it recomposes this plate's exact config and
    // compares it with the snapshot the slice was made from. A render pass that asks it once
    // per plate and then asks this too pays that twice.
    bool is_slice_result_ready_for_print(bool slice_result_valid) const
    {
        bool result = slice_result_valid;
        if (result)
            result = m_gcode_result ?
			(!m_gcode_result->toolpath_outside && m_gcode_result->gcode_check_result.error_code == 0 && !m_gcode_result->filament_printable_reuslt.has_value()) :
			false;// && !m_gcode_result->conflict_result.has_value()  gcode conflict can also print
        return result;
    }

    // check whether plate's slice result valid for export to file
    bool is_slice_result_ready_for_export()
    {
        return is_slice_result_ready_for_print() && has_printable_instances();
    }

    //invalid sliced result
    // capture_config: when a slice has just produced this plate's G-code, capture the
    // configuration it was produced from, so the plate can later be asked whether it is
    // still current. The one caller that must pass false is the 3MF load path, which has
    // already restored the snapshot the file was sliced with; recomposing there would
    // replace it with whatever is loaded now and every reopened project would read as
    // current no matter which presets had changed.
    void update_slice_result_valid_state(bool valid = false, bool capture_config = true);

    void update_slicing_percent(float percent)
    {
        m_slice_percent = percent;
    }

    float get_slicing_percent() { return m_slice_percent; }

    /*slice related functions*/
    //update current slice context into backgroud slicing process
    void update_slice_context(BackgroundSlicingProcess& process);
    //return the fff print object
    Print* fff_print() { return m_print; }
    //return the slice result
    GCodeProcessorResult* get_slice_result() { return m_gcode_result; }

    std::string           get_tmp_gcode_path();
    std::string           get_temp_config_3mf_path();
    //this API should only be used for command line usage
    void set_tmp_gcode_path(std::string new_path)
    {
        m_tmp_gcode_path = new_path;
    }
    //load gcode from file
    int load_gcode_from_file(const std::string& filename);
    //load thumbnail data from file
    int load_thumbnail_data(std::string filename, ThumbnailData& thumb_data);
    //load pattern thumbnail data from file
    int load_pattern_thumbnail_data(std::string filename);
    //load pattern box data from file
    int load_pattern_box_data(std::string filename);

    std::vector<int> get_first_layer_print_sequence() const;
    std::vector<LayerPrintSequence> get_other_layers_print_sequence() const;
    void set_first_layer_print_sequence(const std::vector<int> &sorted_filaments);
    void set_other_layers_print_sequence(const std::vector<LayerPrintSequence>& layer_seq_list);
    void update_first_layer_print_sequence(size_t filament_nums);
    void update_first_layer_print_sequence_when_delete_filament(size_t filamen_id);

    void print() const;

    void on_extruder_count_changed(int extruder_count);
    void set_filament_count(int filament_count);
    void on_filament_added();
    void on_filament_deleted(int filament_count, int filament_id);

    friend class cereal::access;
    friend class UndoRedo::StackImpl;

    template<class Archive> void load(Archive& ar) {
        std::vector<std::pair<int, int>>	objects_and_instances;
        std::vector<std::pair<int, int>>	instances_outside;

        ar(m_plate_index, m_name, m_printer_preset_name, m_printable_height, m_print_index, m_origin, m_width, m_depth, m_height, m_locked, m_selected, m_ready_for_slice, m_slice_result_valid, m_apply_invalid, m_printable, m_tmp_gcode_path, objects_and_instances, instances_outside, m_config, m_sliced_config, m_sliced_config_dropped_reason, m_printer_vendor_id, m_print_preset_name, m_filament_preset_names, m_filament_colours, m_physical_printer_id);

        for (std::vector<std::pair<int, int>>::iterator it = objects_and_instances.begin(); it != objects_and_instances.end(); ++it)
            obj_to_instance_set.insert(std::pair(it->first, it->second));

        for (std::vector<std::pair<int, int>>::iterator it = instances_outside.begin(); it != instances_outside.end(); ++it)
            instance_outside_set.insert(std::pair(it->first, it->second));
    }
    template<class Archive> void save(Archive& ar) const {
        std::vector<std::pair<int, int>>	objects_and_instances;
        std::vector<std::pair<int, int>>	instances_outside;

        for (std::set<std::pair<int, int>>::iterator it = instance_outside_set.begin(); it != instance_outside_set.end(); ++it)
            instances_outside.emplace_back(it->first, it->second);

        for (std::set<std::pair<int, int>>::iterator it = obj_to_instance_set.begin(); it != obj_to_instance_set.end(); ++it)
            objects_and_instances.emplace_back(it->first, it->second);

        ar(m_plate_index, m_name, m_printer_preset_name, m_printable_height, m_print_index, m_origin, m_width, m_depth, m_height, m_locked, m_selected, m_ready_for_slice, m_slice_result_valid, m_apply_invalid, m_printable, m_tmp_gcode_path, objects_and_instances, instances_outside, m_config, m_sliced_config, m_sliced_config_dropped_reason, m_printer_vendor_id, m_print_preset_name, m_filament_preset_names, m_filament_colours, m_physical_printer_id);
    }
    /*template<class Archive> void serialize(Archive& ar)
    {
        std::vector<std::pair<int, int>> objects_and_instances;
        for (std::set<std::pair<int, int>>::iterator it = obj_to_instance_set.begin(); it != obj_to_instance_set.end(); ++it)
            objects_and_instances.emplace_back(it->first, it->second);
        ar(m_plate_index, m_origin, m_width, m_depth, m_height, m_locked, m_ready_for_slice, m_printable, objects_and_instances);
    }*/
};

class PartPlateList : public ObjectBase
{
    Plater* m_plater; //Plater reference, not own it
    Model* m_model; //Model reference, not own it
    PrinterTechnology  printer_technology;

    std::vector<PartPlate*> m_plate_list;
    std::map<int, PrintBase*> m_print_list;
    std::map<int, GCodeResult*> m_gcode_result_list;
    std::mutex m_plates_mutex;
    int m_plate_count;
    int m_plate_cols;
    int m_current_plate;
    int m_print_index;

    int m_plate_width;
    int m_plate_depth;
    int m_plate_height;

    float m_height_to_lid;
    float m_height_to_rod;
    PartPlate::HeightLimitMode m_height_limit_mode{PartPlate::HEIGHT_LIMIT_BOTH};

    PartPlate unprintable_plate;
    Pointfs m_shape;
    Pointfs m_exclude_areas;
    Pointfs m_wrapping_exclude_areas;
    std::vector<Pointfs> m_extruder_areas;
    std::vector<double> m_extruder_heights;
    BoundingBoxf3 m_bounding_box;
    bool m_intialized;
    std::string m_logo_texture_filename;
    GLTexture m_logo_texture;
    //One texture per bed-texture file, kept for the life of the list. A plate switching
    //printers looks its texture up here, so switching BACK is free, and a texture is never
    //destroyed to make room for its replacement - which is what used to draw a black bed
    //while the replacement compressed.
    std::map<std::string, std::unique_ptr<GLTexture>> m_logo_texture_cache;
    GLTexture m_del_texture;
    GLTexture m_del_hovered_texture;
    GLTexture m_move_front_hovered_texture;
    GLTexture m_move_front_texture;
    GLTexture m_arrange_texture;
    GLTexture m_arrange_hovered_texture;
    GLTexture m_orient_texture;
    GLTexture m_orient_hovered_texture;
    GLTexture m_locked_texture;
    GLTexture m_locked_hovered_texture;
    GLTexture m_lockopen_texture;
    GLTexture m_lockopen_hovered_texture;
    GLTexture m_plate_settings_texture;
    GLTexture m_plate_settings_changed_texture;
    GLTexture m_plate_settings_hovered_texture;
    GLTexture m_plate_settings_changed_hovered_texture;
    GLTexture m_plate_set_filament_map_texture;
    GLTexture m_plate_set_filament_map_hovered_texture;
    GLTexture m_plate_name_edit_texture;
    GLTexture m_plate_name_edit_hovered_texture;
    GLTexture m_idx_textures[MAX_PLATE_COUNT];
    // set render option
    bool render_bedtype_logo = true;
    bool render_plate_settings = true;
    bool render_cali_logo = true;

    bool m_is_dark = false;

    int m_filament_count = 1;

    void init();
    //compute the origin for printable plate with index i
    Vec3d compute_origin(int index, int column_count);
    //compute the origin for unprintable plate
    Vec3d compute_origin_for_unprintable();
    //compute shape position
    Vec2d compute_shape_position(int index, int cols);
    //generate icon textures
    void generate_icon_textures();
    void release_icon_textures();

    friend class cereal::access;
    friend class UndoRedo::StackImpl;
    friend class PartPlate;

public:
    void set_default_wipe_tower_pos_for_plate(int plate_idx, bool init_pos = false);
    class BedTextureInfo {
    public:
        class TexturePart {
        public:
            // position
            float x;
            float y;
            float w;
            float h;
            std::string filename;
            GLTexture* texture { nullptr };
            GLModel* buffer { nullptr };
            TexturePart(float xx, float yy, float ww, float hh, std::string file){
                x = xx; y = yy;
                w = ww; h = hh;
                filename = file;
                texture = nullptr;
                buffer = nullptr;
            }

            TexturePart(const TexturePart& part) {
                this->x = part.x;
                this->y = part.y;
                this->w = part.w;
                this->h = part.h;
                this->buffer    = part.buffer;
                this->filename  = part.filename;
                this->texture   = part.texture;
            }
            void update_pos(float xx, float yy, float ww, float hh) {
                x = xx;
                y = yy;
                w = ww;
                h = hh;
            }
            void update_file(std::string file) {
                filename = file;
            }

            void update_buffer();
            void reset();
        };
        // x/y/w/h are bed-local millimetres. Each plate draws these shared buffers
        // through its own frame, so no copy of them carries a plate's position.
        std::vector<TexturePart> parts;
        void                     reset();
    };

    static constexpr unsigned int MAX_PLATES_COUNT = MAX_PLATE_COUNT;
    static GLTexture bed_textures[(unsigned int)btCount];
    static bool is_load_bedtype_textures;
    static bool is_load_cali_texture;
    static bool is_load_extruder_only_area_textures;

    PartPlateList(int width, int depth, int height, Plater* platerObj, Model* modelObj, PrinterTechnology tech = ptFFF);
    PartPlateList(Plater* platerObj, Model* modelObj, PrinterTechnology tech = ptFFF);
    ~PartPlateList();

    //this may be happened after machine changed
    void reset_size(int width, int depth, int height, bool reload_objects = true, bool update_shapes = false);
    //clear all the instances in the plate, but keep the plates
    void clear(bool delete_plates = false, bool release_print_list = false, bool except_locked = false, int plate_index = -1);
    //clear all the instances in the plate, and delete the plates, only keep the first default plate
    void reset(bool do_init);
    //compute the origin for printable plate with index i using new width
    Vec3d compute_origin_using_new_size(int i, int new_width, int new_depth);

    //reset partplate to init states
    void reinit();

    //Lay every plate out from its own footprint. Plates flow left to right into
    //rows of m_plate_cols; each row is as deep as its deepest plate. Replaces the
    //old uniform grid, which could only ever place identically sized plates.
    void reflow_layout();
    //world-space origin of a plate's bed. Use this instead of multiplying an
    //index by a stride: with per-plate beds there is no single stride.
    Vec2d get_plate_origin_2d(int index) const;
    //footprint reflow_layout should reserve for a plate, falling back to the
    //list-wide size for plates that have not been given a bed of their own yet
    Vec2d get_plate_layout_size(int index) const;
    //Origin for any slot, including ones past the end of the list. Arrange hands
    //back bed indices for plates it expects us to create, so we have to be able to
    //say where a plate *will* sit. Not-yet-existing slots are assumed default sized.
    Vec2d predict_plate_origin(int index) const;

    void get_plate_size(int& width, int& depth, int& height) {
        width = m_plate_width;
        depth = m_plate_depth;
        height = m_plate_height;
    }

    // Pantheon: update plates after moving plate to the front
    void update_plates();

    /*basic plate operations*/
    //create an empty plate and return its index
    int create_plate(bool adjust_position = true);

    // duplicate plate
    int duplicate_plate(int index);

    //destroy print which has the index of print_index
    int destroy_print(int print_index);

    //delete a plate by index
    int delete_plate(int index);

    //delete a plate by pointer
    //int delete_plate(PartPlate* plate);
    void delete_selected_plate();

    //get a plate pointer by index
    PartPlate* get_plate(int index);
    //const overload, so a read-only caller can name an exact plate without either
    //casting the constness away or giving up its own const
    const PartPlate* get_plate(int index) const
    {
        return (index < 0 || index >= (int) m_plate_list.size()) ? nullptr : m_plate_list[index];
    }

    void get_height_limits(float& height_to_lid, float& height_to_rod)
    {
        height_to_lid = m_height_to_lid;
        height_to_rod = m_height_to_rod;
    }

    void set_height_limits_mode(PartPlate::HeightLimitMode mode)
    {
        m_height_limit_mode = mode;
    }

    int get_curr_plate_index() const { return m_current_plate; }
    PartPlate* get_curr_plate() { return m_plate_list[m_current_plate]; }
    const PartPlate* get_curr_plate() const { return m_plate_list[m_current_plate]; }

    std::vector<PartPlate*>& get_plate_list() { return m_plate_list; };

    PartPlate* get_selected_plate();

    std::vector<PartPlate*> get_nonempty_plate_list();

    std::vector<const GCodeProcessorResult*> get_nonempty_plates_slice_results();

    //compute the origin for printable plate with index i
    Vec3d get_current_plate_origin() { return compute_origin(m_current_plate, m_plate_cols); }
    Vec2d get_current_shape_position() { return compute_shape_position(m_current_plate, m_plate_cols); }
    Pointfs get_exclude_area() { return m_exclude_areas; }
    Pointfs get_wrapping_exclude_area() const { return m_wrapping_exclude_areas; }

    std::set<int> get_extruders(bool conside_custom_gcode = false) const;

    //select plate
    int select_plate(int index);

    //get the plate counts, not including the invalid plate
    int get_plate_count() const;

    //update the plate cols due to plate count change
    void update_plate_cols();

    void update_all_plates_pos_and_size(bool adjust_position = true, bool with_unprintable_move = true, bool switch_plate_type = false, bool do_clear = true);

    //get the plate cols
    int get_plate_cols() { return m_plate_cols; }

    //move the plate to position index
    int move_plate_to_index(int old_index, int new_index);

    //lock plate
    int lock_plate(int index, bool state);

    //is locked
    bool is_locked(int index) { return m_plate_list[index]->is_locked();}

    //find plate by print index, return -1 if not found
    int find_plate_by_print_index(int index);

    /*instance related operations*/
    //find instance in which plate, return -1 when not found
    //this function only judges whether it is intersect with plate
    int find_instance(int obj_id, int instance_id);
    int find_instance(BoundingBoxf3& bounding_box);

    //find instance belongs to which plate
    //this function not only judges whether it is intersect with plate, but also judges whether it is fully included in plate
    //returns -1 when can not find any plate
    int find_instance_belongs(int obj_id, int instance_id);

    //notify instance's update, need to refresh the instance in plates
    int notify_instance_update(int obj_id, int instance_id, bool is_new = false);

    //notify instance is removed
    int notify_instance_removed(int obj_id, int instance_id);

    //add instance to special plate, need to remove from the original plate
    int add_to_plate(int obj_id, int instance_id, int plate_id);

    //reload all objects
    int reload_all_objects(bool except_locked = false, int plate_index = -1);

    //reload objects for newly created plate
    int construct_objects_list_for_new_plate(int plate_index);

    /* arrangement related functions */
    //compute the plate index
    //preprocess an arrangement::ArrangePolygon, return true if it is in a locked plate
    bool preprocess_arrange_polygon(int obj_index, int instance_index, arrangement::ArrangePolygon& arrange_polygon, bool selected);
    bool preprocess_arrange_polygon_other_locked(int obj_index, int instance_index, arrangement::ArrangePolygon& arrange_polygon, bool selected);
    //geometry_from_plate says which plate's bed supplies the exclude-area geometry,
    //translated into that plate's local frame. The default of 0 preserves the old
    //behaviour: plate 0 sits at the origin, so its world coords already are local
    //coords, and with uniform beds every plate's exclude area was identical anyway.
    //With per-plate printers that stops being true, so callers arranging on a
    //specific plate must name it.
    bool preprocess_exclude_areas(arrangement::ArrangePolygons& unselected, bool enable_wrapping_detect, int num_plates = 16, float inflation = 0, int geometry_from_plate = 0);
    bool preprocess_nonprefered_areas(arrangement::ArrangePolygons& regions, int num_plates = 1, float inflation=0);

    void postprocess_bed_index_for_selected(arrangement::ArrangePolygon& arrange_polygon);
    void postprocess_bed_index_for_unselected(arrangement::ArrangePolygon& arrange_polygon);
    void postprocess_bed_index_for_current_plate(arrangement::ArrangePolygon& arrange_polygon);

    //postprocess an arrangement:;ArrangePolygon
    void postprocess_arrange_polygon(arrangement::ArrangePolygon& arrange_polygon, bool selected);

    /*rendering related functions*/
    void on_change_color_mode(bool is_dark) { m_is_dark = is_dark; }
    void render(const Transform3d& view_matrix, const Transform3d& projection_matrix, bool bottom, bool only_current = false, bool only_body = false, int hover_id = -1, bool render_cali = false, bool show_grid = true);
    void set_render_option(bool bedtype_texture, bool plate_settings);
    void set_render_cali(bool value = true) { render_cali_logo = value; }
    void register_raycasters_for_picking(GLCanvas3D& canvas)
    {
        for (auto plate : m_plate_list)
            plate->register_raycasters_for_picking(canvas);
    }
    BoundingBoxf3& get_bounding_box() { return m_bounding_box; }
    //int select_plate_by_hover_id(int hover_id);
    int select_plate_by_obj(int obj_index, int instance_index);
    void calc_bounding_boxes();
    void select_plate_view();
    bool set_shapes(const Pointfs              &shape,
                    const Pointfs              &exclude_areas,
                    const Pointfs              &wrapping_exclude_areas,
                    const std::vector<Pointfs> &extruder_areas,
                    const std::vector<double>  &extruder_heights,
                    const std::string          &custom_texture,
                    float                       height_to_lid,
                    float                       height_to_rod);
    //Resolve a printer preset name to its bed geometry. Returns false when the preset
    //is not installed, which is normal when opening a project authored elsewhere; the
    //caller then falls back to the project printer's bed.
    //NOTE: not static and not usable headless. It reads the GUI preset bundle, and in
    //CLI mode there is no wxApp instance to read it from, so a named assignment reports
    //failure instead of being substituted.
    bool resolve_printer_bed(const std::string &preset_name, PlateBed &bed) const;

    //GIVE EVERY PLATE A COMPLETE CONTEXT OF ITS OWN.
    //
    //This is what replaces the project printer. The old rule - an empty field reads as
    //the globally selected preset - ran on every slice, every frame and every board
    //rebuild, and it is the whole of why a project could hold several machines in its
    //data and still slice on one. This runs once per plate instead, at the two moments a
    //plate can exist without a context: it has just been created, or it came out of a
    //project written before per-plate machines. After it, every field is a name the
    //plate owns.
    //
    //Each plate is seeded from the previous complete one, so a new plate lands on the
    //machine the user was already working on and a legacy project keeps whatever
    //machines it did record. Only when no plate has one is the current preset selection
    //read, because a first plate has to come from somewhere.
    //
    //Returns how many plates it completed. Does nothing in CLI mode, which resolves
    //plate contexts against the project config loaded from the 3MF and never through the
    //preset bundle.
    //A plate in an EXISTING session. Each plate is completed from the last complete one,
    //so a new plate lands on the machine the user is already working on. Only when no plate
    //has a context at all is the bundle's current selection used - the first plate of a
    //session, where the selection is a real remembered choice and the only holder there is.
    int complete_plate_contexts();

    //A LOADED project. The seed is what the FILE declared, read out of the 3MF's own config,
    //so this is correct wherever it is called from and whatever the bundle happens to hold.
    //That is the repair: the previous shape sampled the bundle and was therefore only correct
    //at one instant, which is how every plate of a Bambu project ended up named
    //"Default Filament" and quoting 21 hours.
    int complete_plate_contexts(const PlateSlicingContext &declared);

    //True when this process has a preset bundle to resolve plate beds from, which is
    //every GUI run and no CLI run. See the definition.
    bool plate_beds_come_from_presets() const;

    //How many distinct printers the project holds. One is a single-machine project; more
    //is what makes naming a plate's machine on its label worth the pixels.
    int distinct_printer_count() const;

    //The label rule above is a property of the SET, so the plate that changed is not the
    //only one whose label goes stale: crossing between one machine and several restyles
    //every plate at once. Called from the paths that write a plate's printer.
    void refresh_plate_labels_if_machine_count_changed();
    //Give one plate the bed of its exact assigned printer. A plate that names no printer,
    //and a named preset this build does not have, are both unresolved: the plate is marked
    //invalid and reported, and no bed is drawn in place of the one it asked for.
    //reflow=false skips the per-plate relayout so a caller applying several beds in a
    //row can do one trailing reflow_layout() instead of N visible shuffles.
    bool apply_printer_to_plate(int index, bool reflow = true);
    //Re-apply every plate's assignment, e.g. straight after loading a project.
    void apply_printer_assignments();

    //give a single plate a bed of its own, leaving every other plate alone.
    //triggers a relayout because the neighbours have to shuffle around the new
    //footprint, unless reflow=false defers that to the caller.
    bool set_plate_shape(int                         index,
                         const Pointfs              &shape,
                         const Pointfs              &exclude_areas,
                         const std::vector<Pointfs> &extruder_areas,
                         const std::vector<double>  &extruder_heights,
                         float                       height_to_lid,
                         float                       height_to_rod,
                         bool                        reflow = true);
    void set_hover_id(int id);
    void reset_hover_id();
    bool intersects(const BoundingBoxf3 &bb);
    bool contains(const BoundingBoxf3 &bb);

    const std::string &get_logo_texture_filename() { return m_logo_texture_filename; }
    //Find-or-load the texture for one bed-texture file. Starts an asynchronous load on first
    //sight, returns nullptr until the texture is fully on the GPU, and never evicts - so the
    //caller can keep drawing whatever it drew last while a new texture arrives.
    GLTexture *logo_texture_for(const std::string &filename);
    void               update_logo_texture_filename(const std::string &texture_filename);
    /*slice related functions*/
    //update current slice context into backgroud slicing process
    void update_slice_context_to_current_plate(BackgroundSlicingProcess& process);
    //return the current fff print object
    Print& get_current_fff_print() const;
    //return the slice result
    GCodeProcessorResult* get_current_slice_result() const;
    //will create a plate and load gcode, return the plate index
    int create_plate_from_gcode_file(const std::string& filename);

    //invalid all the plater's slice result
    void invalid_all_slice_result();
    //set current plater's slice result to valid
    void update_current_slice_result_state(bool valid) { m_plate_list[m_current_plate]->update_slice_result_valid_state(valid); }
    //is slice result valid or not
    bool is_all_slice_results_valid() const;
    bool is_all_slice_results_ready_for_print() const;
    bool is_all_plates_ready_for_slice() const;
    bool is_all_slice_result_ready_for_export() const;
    void print() const;

    //get the all the sliced result
    void get_sliced_result(std::vector<bool>& sliced_result, std::vector<std::string>& gcode_paths);
    //retruct plates structures after de-serialize
    int rebuild_plates_after_deserialize(std::vector<bool>& previous_sliced_result, std::vector<std::string>& previous_gcode_paths);

    //retruct plates structures after auto-arrangement
    int rebuild_plates_after_arrangement(bool recycle_plates = true, bool except_locked = false, int plate_index = -1);

    /* load/store releted functions, with_gcode = true and plate_idx = -1, export all gcode
    * if with_gcode = true and specify plate_idx, export plate_idx gcode only
    */
    int store_to_3mf_structure(PlateDataPtrs& plate_data_list, bool with_slice_info = true, int plate_idx = -1);
    int load_from_3mf_structure(PlateDataPtrs& plate_data_list, int filament_count = 1);
    //load gcode files
    int load_gcode_files();

    template<class Archive> void serialize(Archive& ar)
    {
        //ar(cereal::base_class<ObjectBase>(this));
        //Cancel undo/redo for m_shape ,Because the printing area of different models is different, currently if the grid changes, it cannot correspond to the model on the left ui
        ar(m_plate_width, m_plate_depth, m_plate_height, m_height_to_lid, m_height_to_rod, m_height_limit_mode, m_plate_count, m_current_plate, m_plate_list, unprintable_plate);
        //ar(m_plate_width, m_plate_depth, m_plate_height, m_plate_count, m_current_plate);
    }
    struct Rect
    {
        int x;
        int y;
        int w;
        int h;
    };
    bool calc_extruder_only_area(Rect &left_only_rect, Rect &right_only_rect);
    void init_bed_type_info();
    bool init_extruder_only_area_info();
    void load_bedtype_textures();
    void load_extruder_only_area_textures();

    void show_cali_texture(bool show = true);
    void init_cali_texture_info();
    void load_cali_textures();

    void on_extruder_count_changed(int extruder_count);

    void set_filament_count(int filament_count);
    void on_filament_deleted(int filament_count, int filament_id);
    void on_filament_added(int filament_count);

    //Last value refresh_plate_labels_if_machine_count_changed acted on. Not state the
    //app reads: purely the memory that makes "did this cross the boundary?" answerable.
    int m_last_distinct_printer_count { -1 };

    std::map<int, bool> m_allow_bed_type_in_double_nozzle;
    BedTextureInfo bed_texture_info[btCount];
    BedTextureInfo cali_texture_info;
    BedTextureInfo extruder_only_area_info[(unsigned char) Slic3r::ExtruderOnlyAreaType::btAreaCount];
};

} // namespace GUI
} // namespace Slic3r

namespace cereal
{
    template <class Archive> struct specialize<Archive, Slic3r::GUI::PartPlate, cereal::specialization::member_load_save> {};
}
#endif //__part_plate_hpp_
