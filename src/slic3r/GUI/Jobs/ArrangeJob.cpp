#include "ArrangeJob.hpp"

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/SVG.hpp"
#include "libslic3r/MTUtils.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/ModelArrange.hpp"

#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"

#include "libnest2d/common.hpp"

#define SAVE_ARRANGE_POLY 0

namespace Slic3r { namespace GUI {
    using ArrangePolygon = arrangement::ArrangePolygon;

// Cache the wti info
class WipeTower: public GLCanvas3D::WipeTowerInfo {
public:
    explicit WipeTower(const GLCanvas3D::WipeTowerInfo &wti)
        : GLCanvas3D::WipeTowerInfo(wti)
    {}

    explicit WipeTower(GLCanvas3D::WipeTowerInfo &&wti)
        : GLCanvas3D::WipeTowerInfo(std::move(wti))
    {}

    void apply_arrange_result(const Vec2d& tr, double rotation, int item_id)
    {
        m_pos = unscaled(tr); m_rotation = rotation;
        apply_wipe_tower();
    }

    ArrangePolygon get_arrange_polygon() const
    {
        Polygon ap({
            {scaled(m_bb.min)},
            {scaled(m_bb.max.x()), scaled(m_bb.min.y())},
            {scaled(m_bb.max)},
            {scaled(m_bb.min.x()), scaled(m_bb.max.y())}
            });

        ArrangePolygon ret;
        ret.poly.contour = std::move(ap);
        ret.translation  = scaled(m_pos);
        ret.rotation     = m_rotation;
        //BBS
        ret.name = "WipeTower";
        ret.is_virt_object = true;
        ret.is_wipe_tower = true;
        ++ret.priority;

        BOOST_LOG_TRIVIAL(debug) << " arrange: wipe tower info:" << m_bb << ", m_pos: " << m_pos.transpose();

        return ret;
    }
};

// BBS: add partplate logic
static WipeTower get_wipe_tower(const Plater &plater, int plate_idx)
{
    return WipeTower{plater.canvas3D()->get_wipe_tower_info(plate_idx)};
}

//The resolution itself, reporting failure instead of throwing it. This is the core so the
//two forms cannot drift: the throwing one below is this plus the throw. A caller that must
//not refuse the user's gesture (paste) needs the answer "no, and here is why"; an arrange
//that was asked for explicitly still wants the throw, because there is nothing sensible to
//arrange against and the message reaches the user through the job's handler.
static bool try_resolve_arrange_plate(PartPlate *plate, ResolvedPlateSlicingConfig &resolved, std::string &error)
{
    PresetBundle &bundle = *wxGetApp().preset_bundle;
    PlateSlicingContext context;
    std::vector<int> filament_maps;
    std::vector<int> volume_maps;
    if (plate != nullptr) {
        context       = plate->get_slicing_context();
        filament_maps = plate->get_real_filament_maps(bundle.project_config);
        volume_maps   = plate->get_real_filament_volume_maps(bundle.project_config);
    } else {
        const auto *maps = bundle.project_config.option<ConfigOptionInts>("filament_map");
        const auto *volumes = bundle.project_config.option<ConfigOptionInts>("filament_volume_map");
        if (maps != nullptr)
            filament_maps = maps->values;
        if (volumes != nullptr)
            volume_maps = volumes->values;
    }
    if (!bundle.resolve_plate_slicing_config(context, filament_maps, volume_maps,
                                             resolved, error)) {
        if (plate != nullptr) {
            plate->update_apply_result_invalid(true);
            error = (boost::format("Plate %1% has an unresolved slicing context: %2%")
                     % (plate->get_index() + 1) % error).str();
        } else {
            error = "The Project-row slicing context is unresolved: " + error;
        }
        return false;
    }
    if (plate != nullptr)
        resolved.config.apply(*plate->config(), true);
    return true;
}

static ResolvedPlateSlicingConfig resolve_arrange_plate(PartPlate *plate)
{
    ResolvedPlateSlicingConfig resolved;
    std::string                error;
    if (!try_resolve_arrange_plate(plate, resolved, error))
        throw RuntimeError(error);
    return resolved;
}

arrangement::ArrangePolygon get_wipetower_arrange_poly(WipeTower* tower)
{
    ArrangePolygon ap = tower->get_arrange_polygon();
    ap.bed_idx = 0;
    ap.setter = NULL; // do not move wipe tower
    return ap;
}

void ArrangeJob::clear_input()
{
    const Model &model = m_plater->model();

    size_t count = 0, cunprint = 0; // To know how much space to reserve
    for (auto obj : model.objects)
        for (auto mi : obj->instances)
            mi->printable ? count++ : cunprint++;

    params.nonprefered_regions.clear();
    m_selected.clear();
    m_unselected.clear();
    m_unprintable.clear();
    m_locked.clear();
    m_unarranged.clear();
    m_uncompatible_plates.clear();
    m_selected.reserve(count + 1 /* for optional wti */);
    m_unselected.reserve(count + 1 /* for optional wti */);
    m_unprintable.reserve(cunprint /* for optional wti */);
    m_locked.reserve(count + 1 /* for optional wti */);
    current_plate_index = 0;
}

ArrangePolygon ArrangeJob::prepare_arrange_polygon(int object_idx, int instance_idx)
{
    PartPlateList &plates = m_plater->get_partplate_list();
    int plate_idx = only_on_partplate ? current_plate_index : plates.find_instance_belongs(object_idx, instance_idx);
    if (plate_idx < 0)
        plate_idx = only_on_partplate ? current_plate_index : plates.find_instance(object_idx, instance_idx);

    const ResolvedPlateSlicingConfig resolved = resolve_arrange_plate(plate_idx < 0 ? nullptr : plates.get_plate(plate_idx));
    return get_instance_arrange_poly(m_plater->model().objects[object_idx]->instances[instance_idx], resolved.config);
}

void ArrangeJob::prepare_selected() {
    PartPlateList& plate_list = m_plater->get_partplate_list();

    clear_input();

    Model& model = m_plater->model();
    bool selected_is_locked = false;
    //BBS: remove logic for unselected object
    //double stride = bed_stride_x(m_plater);

    std::vector<const Selection::InstanceIdxsList*>
        obj_sel(model.objects.size(), nullptr);

    for (auto& s : m_plater->get_selection().get_content())
        if (s.first < int(obj_sel.size()))
            obj_sel[size_t(s.first)] = &s.second;

    // Go through the objects and check if inside the selection
    for (size_t oidx = 0; oidx < model.objects.size(); ++oidx) {
        const Selection::InstanceIdxsList* instlist = obj_sel[oidx];
        ModelObject* mo = model.objects[oidx];

        std::vector<bool> inst_sel(mo->instances.size(), false);

        if (instlist)
            for (auto inst_id : *instlist)
                inst_sel[size_t(inst_id)] = true;

        for (size_t i = 0; i < inst_sel.size(); ++i) {
            ModelInstance* mi = mo->instances[i];
            ArrangePolygon&& ap = prepare_arrange_polygon((int)oidx, (int)i);
            //BBS: partplate_list preprocess
            //remove the locked plate's instances, neither in selected, nor in un-selected
            bool locked = plate_list.preprocess_arrange_polygon(oidx, i, ap, inst_sel[i]);
            if (!locked)
                {
                ArrangePolygons& cont = mo->instances[i]->printable ?
                    (inst_sel[i] ? m_selected :
                        m_unselected) :
                    m_unprintable;

                ap.itemid = cont.size();
                cont.emplace_back(std::move(ap));
                }
            else
                {
                //skip this object due to be locked in plate
                ap.itemid = m_locked.size();
                m_locked.emplace_back(std::move(ap));
                if (inst_sel[i])
                    selected_is_locked = true;
                BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": skip locked instance, obj_id %1%, instance_id %2%, name %3%") % oidx % i % mo->name;
                }
            }
        }


    // If the selection was empty arrange everything
    //if (m_selected.empty()) m_selected.swap(m_unselected);
    if (m_selected.empty()) {
        if (!selected_is_locked)
            m_selected.swap(m_unselected);
        else {
            m_plater->get_notification_manager()->push_notification(NotificationType::BBLPlateInfo,
                NotificationManager::NotificationLevel::WarningNotificationLevel, into_u8(_L("All the selected objects are on a locked plate.\nCannot auto-arrange these objects.")));
            }
        }

    prepare_wipe_tower();


    // The strides have to be removed from the fixed items. For the
    // arrangeable (selected) items bed_idx is ignored and the
    // translation is irrelevant.
    //BBS: remove logic for unselected object
    //for (auto &p : m_unselected) p.translation(X) -= p.bed_idx * stride;
}

void ArrangeJob::prepare_all() {
    clear_input();

    PartPlateList& plate_list = m_plater->get_partplate_list();    
    for (size_t i = 0; i < plate_list.get_plate_count(); i++) {
        PartPlate* plate = plate_list.get_plate(i);
        bool same_as_global_print_seq = true;
        plate->get_real_print_seq(&same_as_global_print_seq);
        if (plate->is_locked() == false && !same_as_global_print_seq) {
            plate->lock(true);
            m_uncompatible_plates.push_back(i);
        }
    }


    Model &model = m_plater->model();
    bool selected_is_locked = false;

    // Go through the objects and check if inside the selection
    for (size_t oidx = 0; oidx < model.objects.size(); ++oidx) {
        ModelObject *mo = model.objects[oidx];

        for (size_t i = 0; i < mo->instances.size(); ++i) {
            ModelInstance * mi = mo->instances[i];
            ArrangePolygon&& ap = prepare_arrange_polygon((int)oidx, (int)i);
            //BBS: partplate_list preprocess
            //remove the locked plate's instances, neither in selected, nor in un-selected
            bool locked = plate_list.preprocess_arrange_polygon(oidx, i, ap, true);
            if (!locked)
            {
                ArrangePolygons& cont = mo->instances[i]->printable ? m_selected :m_unprintable;

                ap.itemid = cont.size();
                cont.emplace_back(std::move(ap));
            }
            else
            {
                //skip this object due to be locked in plate
                ap.itemid = m_locked.size();
                m_locked.emplace_back(std::move(ap));
                selected_is_locked = true;
                BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": skip locked instance, obj_id %1%, instance_id %2%") % oidx % i;
            }
        }
    }


    // If the selection was empty arrange everything
    //if (m_selected.empty()) m_selected.swap(m_unselected);
    if (m_selected.empty()) {
        if (!selected_is_locked) {
            m_plater->get_notification_manager()->push_notification(NotificationType::BBLPlateInfo,
                NotificationManager::NotificationLevel::WarningNotificationLevel, into_u8(_L("No arrangeable objects are selected.")));
        }
        else {
            m_plater->get_notification_manager()->push_notification(NotificationType::BBLPlateInfo,
                NotificationManager::NotificationLevel::WarningNotificationLevel, into_u8(_L("All the selected objects are on a locked plate.\nCannot auto-arrange these objects.")));
        }
    }

    prepare_wipe_tower();

    //Wrapping detection is a printer property, so it is read off the plate whose bed these
    //exclusion regions are being built against - the current one - and not off a project
    //that no longer has a printer.
    const ResolvedPlateSlicingConfig current = resolve_arrange_plate(plate_list.get_curr_plate());
    const bool enable_wrapping = current.config.opt_bool("enable_wrapping_detection");

    // add the virtual object into unselect list if has
    plate_list.preprocess_exclude_areas(m_unselected, enable_wrapping, MAX_NUM_PLATES);
}

arrangement::ArrangePolygon estimate_wipe_tower_info(int                                plate_index,
                                                      PartPlate                         *geometry_plate,
                                                      const ResolvedPlateSlicingConfig &resolved,
                                                      std::set<int>                    &extruder_ids)
{
    // we have to estimate the depth using the extruder number of all plates
    int extruder_size = extruder_ids.size();

    Vec3d wipe_tower_size, wipe_tower_pos;
    const auto *nozzles = resolved.config.option<ConfigOptionFloats>("nozzle_diameter");
    const int nozzle_nums = nozzles == nullptr ? 0 : (int)nozzles->values.size();
    auto arrange_poly = geometry_plate->estimate_wipe_tower_polygon(resolved.config, plate_index, wipe_tower_pos,
                                                                    wipe_tower_size, nozzle_nums, extruder_size);
    arrange_poly.bed_idx = plate_index;
    return arrange_poly;
}

// 准备料塔。逻辑如下：
// 1. 以下几种情况不需要料塔：
//    1）料塔被禁用，
//    2）逐件打印，
//    3）不允许不同材料落在相同盘，且没有多色对象
// 2. 以下情况需要料塔：
//    1）某对象是多色对象；
//    2）打开了支撑，且支撑体与接触面使用的是不同材料
//    3）允许不同材料落在相同盘，且所有选定对象中使用了多种热床温度相同的材料
//     （所有对象都是单色的，但不同对象的材料不同，例如：对象A使用红色PLA，对象B使用白色PLA）
void ArrangeJob::prepare_wipe_tower()
{
    bool need_wipe_tower = false;

    // estimate if we need wipe tower for all plates:
    // need wipe tower if some object has multiple extruders (has paint-on colors or support material)
    for (const auto& item : m_selected) {
        std::set<int> obj_extruders;
        obj_extruders.insert(item.extrude_ids.begin(), item.extrude_ids.end());
        if (obj_extruders.size() > 1) {
            need_wipe_tower = true;
            BOOST_LOG_TRIVIAL(info) << "arrange: need wipe tower because object " << item.name << " has multiple extruders (has paint-on colors)";
            break;
        }
    }

    // if multile extruders have same bed temp, we need wipe tower
    // 允许不同材料落在相同盘，且所有选定对象中使用了多种热床温度相同的材料
    if (params.allow_multi_materials_on_same_plate) {
        std::map<int, std::set<int>> bedTemp2extruderIds;
        for (const auto& item : m_selected)
            for (auto id : item.extrude_ids) { bedTemp2extruderIds[item.bed_temp].insert(id); }
        for (const auto& be : bedTemp2extruderIds) {
            if (be.second.size() > 1) {
                need_wipe_tower = true;
                BOOST_LOG_TRIVIAL(info) << "arrange: need wipe tower because allow_multi_materials_on_same_plate=true and we have multiple extruders of same type";
                break;
            }
        }
    }
    BOOST_LOG_TRIVIAL(info) << "arrange: need_wipe_tower=" << need_wipe_tower;


    ArrangePolygon    wipe_tower_ap;
    wipe_tower_ap.name = "WipeTower";
    wipe_tower_ap.is_virt_object = true;
    wipe_tower_ap.is_wipe_tower = true;
    std::set<int> extruder_ids;
    PartPlateList& ppl = wxGetApp().plater()->get_partplate_list();
    int plate_count = ppl.get_plate_count();
    if (!only_on_partplate) {
        extruder_ids = ppl.get_extruders(true);
    }

    //A BED THAT DOES NOT EXIST YET BELONGS TO THE PLATE THAT WILL OVERFLOW ONTO IT.
    //
    //finalize gives every overflow plate the context of its SOURCE plate, because an object
    //must not change machine by failing to fit. The last plate in the list is not that
    //plate: it is whichever plate happens to be last, on whatever machine, and estimating a
    //future bed's wipe tower against it describes a machine nothing will print on.
    //
    //Which plate overflows is not known until arrange has run, so what prepare can name is
    //the plate this arrange is anchored to - the one the user is looking at, which is also
    //where arrange_per_plate sends items with no plate of their own. There is no project
    //template to be a plate's shape instead.
    PartPlate *future_template = ppl.get_curr_plate();
    if (future_template == nullptr && plate_count > 0)
        future_template = ppl.get_plate(0);

    int bedid_unlocked = 0;
    const int bed_limit = future_template == nullptr ? plate_count : MAX_NUM_PLATES;
    for (int bedid = 0; bedid < bed_limit; bedid++) {
        const bool future_plate = bedid >= plate_count;
        PartPlate* pl = future_plate ? future_template : ppl.get_plate(bedid);
        if (!future_plate && pl->is_locked())
            continue;
        const ResolvedPlateSlicingConfig resolved = resolve_arrange_plate(pl);
        const bool enable_prime_tower = resolved.config.opt_bool("enable_prime_tower");
        const bool smooth_timelapse = resolved.config.opt_enum<TimelapseType>("timelapse_type") == TimelapseType::tlSmooth;
        const bool sequential = pl->get_real_print_seq() == PrintSequence::ByObject;
        const bool plate_needs_wipe_tower = enable_prime_tower && !sequential && (need_wipe_tower || smooth_timelapse);
        if (!future_plate) {
            auto wti = get_wipe_tower(*m_plater, bedid);
            if (wti) {
                // wipe tower is already there
                wipe_tower_ap = get_wipetower_arrange_poly(&wti);
                wipe_tower_ap.bed_idx = bedid_unlocked;
                m_unselected.emplace_back(wipe_tower_ap);
                bedid_unlocked++;
                continue;
            }
        }
        if (plate_needs_wipe_tower) {
            if (!future_plate) {
                auto plate_extruders = pl->get_extruders(true);
                extruder_ids.clear();
                extruder_ids.insert(plate_extruders.begin(), plate_extruders.end());
            }
            wipe_tower_ap = estimate_wipe_tower_info(bedid, pl, resolved, extruder_ids);
            wipe_tower_ap.bed_idx = bedid_unlocked;
            m_unselected.emplace_back(wipe_tower_ap);
        }
        bedid_unlocked++;
    }
}


//BBS: prepare current part plate for arranging
void ArrangeJob::prepare_partplate() {
    clear_input();

    PartPlateList& plate_list = m_plater->get_partplate_list();
    PartPlate* plate = plate_list.get_curr_plate();
    current_plate_index = plate_list.get_curr_plate_index();
    assert(plate != nullptr);

    if (plate->empty())
    {
        //no instances on this plate
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": no instances in current plate!");

        return;
    }

    if (plate->is_locked()) {
        m_plater->get_notification_manager()->push_notification(NotificationType::BBLPlateInfo,
            NotificationManager::NotificationLevel::WarningNotificationLevel, into_u8(_L("This plate is locked.\nCannot auto-arrange on this plate.")));
        return;
    }

    Model& model = m_plater->model();

    // Go through the objects and check if inside the selection
    for (size_t oidx = 0; oidx < model.objects.size(); ++oidx)
    {
        ModelObject* mo = model.objects[oidx];
        for (size_t inst_idx = 0; inst_idx < mo->instances.size(); ++inst_idx)
        {
            bool             in_plate = plate->contain_instance(oidx, inst_idx) || plate->intersect_instance(oidx, inst_idx);
            ArrangePolygon&& ap = prepare_arrange_polygon((int)oidx, (int)inst_idx);

            ArrangePolygons& cont = mo->instances[inst_idx]->printable ?
                (in_plate ? m_selected : m_unselected) :
                m_unprintable;
            bool locked = plate_list.preprocess_arrange_polygon_other_locked(oidx, inst_idx, ap, in_plate);
            if (!locked)
            {
                ap.itemid = cont.size();
                cont.emplace_back(std::move(ap));
            }
            else
            {
                //skip this object due to be not in current plate, treated as locked
                ap.itemid = m_locked.size();
                m_locked.emplace_back(std::move(ap));
                //BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": skip locked instance, obj_id %1%, name %2%") % oidx % mo->name;
            }
        }
    }

    // BBS
    if (auto wti = get_wipe_tower(*m_plater, current_plate_index)) {
        ArrangePolygon&& ap = get_wipetower_arrange_poly(&wti);
        m_unselected.emplace_back(std::move(ap));
    }

    const ResolvedPlateSlicingConfig resolved = resolve_arrange_plate(plate);
    const bool enable_wrapping = resolved.config.opt_bool("enable_wrapping_detection");

    // add the virtual object into unselect list if has
    plate_list.preprocess_exclude_areas(m_unselected, enable_wrapping, current_plate_index + 1);
}

//BBS: add partplate logic
void ArrangeJob::prepare()
{
    m_plater->get_notification_manager()->push_notification(NotificationType::ArrangeOngoing,
        NotificationManager::NotificationLevel::RegularNotificationLevel, _u8L("Arranging..."));
    m_plater->get_notification_manager()->bbl_close_plateinfo_notification();

    params = init_arrange_params(m_plater);

    //BBS update extruder params and speed table before arranging
    PartPlate *current_plate = m_plater->get_partplate_list().get_curr_plate();
    const ResolvedPlateSlicingConfig resolved = resolve_arrange_plate(current_plate);
    const Slic3r::DynamicPrintConfig& config = resolved.config;
    auto& print = wxGetApp().plater()->get_partplate_list().get_current_fff_print();
    auto print_config = print.config();
    int numExtruders = (int)resolved.filament_presets.size();

    Model::setExtruderParams(config, numExtruders);
    Model::setPrintSpeedTable(config, print_config);

    int state = m_plater->get_prepare_state();
    if (state == Job::JobPrepareState::PREPARE_STATE_DEFAULT) {
        only_on_partplate = false;
        prepare_all();
    }
    else if (state == Job::JobPrepareState::PREPARE_STATE_MENU) {
        only_on_partplate = true;   // only arrange items on current plate
        prepare_partplate();
    }


#if SAVE_ARRANGE_POLY
    if (1)
    { // subtract excluded region and get a polygon bed
        auto& print = wxGetApp().plater()->get_partplate_list().get_current_fff_print();
        auto print_config = print.config();
        bed_poly.points = get_bed_shape(*m_plater->config());
        Polygons exclude_polys = get_bed_excluded_area(print_config);
        bed_poly = diff({ bed_poly }, exclude_polys)[0];
    }

    BoundingBox bbox = bed_poly.bounding_box();
    Point center = bbox.center();
    auto polys_to_draw = m_selected;
    for (auto it = polys_to_draw.begin(); it != polys_to_draw.end(); it++) {
        it->poly.translate(center);
        bbox.merge(it->poly);
    }
    SVG svg("SVG/arrange_poly.svg", bbox);
    if (svg.is_opened()) {
        svg.draw_outline(bed_poly);
        //svg.draw_grid(bbox, "gray", scale_(0.05));
        std::vector<std::string> color_array = { "red","black","yellow","gree","blue" };
        for (auto it = polys_to_draw.begin(); it != polys_to_draw.end(); it++) {
            std::string color = color_array[(it - polys_to_draw.begin()) % color_array.size()];
            svg.add_comment(it->name);
            svg.draw_text(get_extents(it->poly).min, it->name.c_str(), color.c_str());
            svg.draw_outline(it->poly, color);
        }
    }
#endif

    check_unprintable();
}

void ArrangeJob::check_unprintable()
{
    //An item is judged against the height of the machine that will print it: the
    //plate's own printer when the plate is pinned to one, the project printer
    //otherwise. Judging everything against the project height would wrongly reject
    //items on a plate assigned to a taller machine.
    PartPlateList& ppl = m_plater->get_partplate_list();
    std::vector<int> logical_to_plate;   //arrange numbering skips locked plates
    if (only_on_partplate)
        logical_to_plate.push_back(current_plate_index);
    else
        for (int i = 0; i < ppl.get_plate_count(); ++i)
            if (!ppl.get_plate(i)->is_locked())
                logical_to_plate.push_back(i);

    auto allowed_height = [&](const ArrangePolygon& ap) -> double {
        //in current-plate mode everything belongs to the current plate
        const int g = only_on_partplate ? 0 : ap.src_bed_idx;
        if (g >= 0 && g < (int)logical_to_plate.size()) {
            PartPlate* plate = ppl.get_plate(logical_to_plate[g]);
            if (plate != nullptr)
                return plate->get_printable_height();
        }
        return (double)params.printable_height;
    };

    for (auto it = m_selected.begin(); it != m_selected.end();) {
        if (it->poly.area() < 0.001 || it->height > allowed_height(*it))
        {
#if SAVE_ARRANGE_POLY
            SVG svg(data_dir() + "/SVG/arrange_unprintable_"+it->name+".svg", get_extents(it->poly));
            if (svg.is_opened())
                svg.draw_outline(it->poly);
#endif
            if (it->poly.area() < 0.001) {
                auto msg = (boost::format(
                    _utf8("Object %s has zero size and can't be arranged."))
                    % _utf8(it->name)).str();
                m_plater->get_notification_manager()->push_notification(NotificationType::BBLPlateInfo,
                    NotificationManager::NotificationLevel::WarningNotificationLevel, msg);
            }
            m_unprintable.push_back(*it);
            it = m_selected.erase(it);
        }
        else
            it++;
    }
}

void ArrangeJob::process(Ctl &ctl)
{
    static const auto arrangestr = _u8L("Arranging");
    ctl.update_status(0, arrangestr);
    ctl.call_on_main_thread([this]{ prepare(); }).wait();;

    auto & partplate_list = m_plater->get_partplate_list();

    params.stopcondition = [&ctl]() { return ctl.was_canceled(); };

    params.progressind = [this, &ctl](unsigned num_finished, std::string str = "") {
        ctl.update_status(num_finished * 100 / status_range(), _u8L("Arranging") + str);
    };

    {
        BOOST_LOG_TRIVIAL(warning)<< "Arrange full params: "<< params.to_json();
        BOOST_LOG_TRIVIAL(info) << boost::format("arrange: items selected before arranging: %1%") % m_selected.size();
        for (auto selected : m_selected) {
            BOOST_LOG_TRIVIAL(debug) << selected.name << ", extruder: " << selected.extrude_ids.back() << ", bed: " << selected.bed_idx << ", filemant_type:" << selected.filament_temp_type
                << ", trans: " << selected.translation.transpose();
        }
        BOOST_LOG_TRIVIAL(debug) << "arrange: items unselected before arrange: " << m_unselected.size();
        for (auto item : m_unselected)
            BOOST_LOG_TRIVIAL(debug) << item.name << ", bed: " << item.bed_idx << ", trans: " << item.translation.transpose()
            <<", bbox:"<<get_extents(item.poly).min.transpose()<<","<<get_extents(item.poly).max.transpose();
    }

    //There is one arrange path, because there is one kind of plate: every plate owns a
    //printer and therefore a bed. The single-project-bed call this used to fall back to
    //is gone with the project printer that made it true.
    arrange_per_plate(ctl);

    // sort by item id
    std::sort(m_selected.begin(), m_selected.end(), [](auto a, auto b) {return a.itemid < b.itemid; });
    {
        BOOST_LOG_TRIVIAL(info) << boost::format("arrange: items selected after arranging: %1%") % m_selected.size();
        for (auto selected : m_selected)
            BOOST_LOG_TRIVIAL(debug) << selected.name << ", extruder: " << selected.extrude_ids.back() << ", bed: " << selected.bed_idx
                                     << ", bed_temp: " << selected.first_bed_temp << ", print_temp: " << selected.print_temp
                                     << ", trans: " << unscale<double>(selected.translation(X)) << ","<< unscale<double>(selected.translation(Y));
        BOOST_LOG_TRIVIAL(debug) << "arrange: items unselected after arrange: "<< m_unselected.size();
        for (auto item : m_unselected)
            BOOST_LOG_TRIVIAL(debug) << item.name << ", bed: " << item.bed_idx << ", trans: " << item.translation.transpose();
    }

    // put unpackable items to m_unprintable so they goes outside
    bool we_have_unpackable_items = false;
    for (auto item : m_selected) {
        if (item.bed_idx < 0) {
            //BBS: already processed in m_selected
            //m_unprintable.push_back(std::move(item));
            we_have_unpackable_items = true;
        }
    }

    // finalize just here.
    ctl.update_status(100,
        ctl.was_canceled() ? _u8L("Arranging canceled.") :
        we_have_unpackable_items ? _u8L("Arranging complete, but some items were not able to be arranged. Reduce spacing and try again.") : _u8L("Arranging done."));
}

//Arrange with per-plate beds. Sticky first, pool second; see the header comment.
void ArrangeJob::arrange_per_plate(Ctl& ctl)
{
    PartPlateList& ppl = m_plater->get_partplate_list();

    //arrange numbers beds skipping locked plates; recover which real plate each bed is
    std::vector<int> logical_to_plate;
    if (only_on_partplate) {
        logical_to_plate.push_back(current_plate_index);
    }
    else {
        for (int i = 0; i < ppl.get_plate_count(); ++i)
            if (!ppl.get_plate(i)->is_locked())
                logical_to_plate.push_back(i);
    }
    const int unlocked_count = (int)logical_to_plate.size();

    //Where a homeless item goes. An item with no source plate - freshly imported, or
    //dropped outside every bed - used to fall into the pool of plates that shared the
    //project bed. There is no such pool, and picking an arbitrary plate would decide which
    //machine prints it. The current plate is the one the user is looking at, which is
    //where the object visually already is, so that is the plate that adopts it.
    int homeless_bed = -1;
    for (int g = 0; g < unlocked_count; ++g)
        if (logical_to_plate[g] == current_plate_index) { homeless_bed = g; break; }
    if (homeless_bed < 0 && unlocked_count > 0)
        homeless_bed = 0;   //the current plate is locked; the first unlocked one is the only other honest answer

    // Group selected items by the plate they are already on.
    std::map<int, std::vector<size_t>> sticky_groups;
    for (size_t k = 0; k < m_selected.size(); ++k) {
        const int src = m_selected[k].src_bed_idx;
        const int bed = (src >= 0 && src < unlocked_count) ? src : homeless_bed;
        if (bed >= 0)
            sticky_groups[bed].push_back(k);
        else {
            m_selected[k].bed_idx = arrangement::UNARRANGED;
            BOOST_LOG_TRIVIAL(error) << "arrange: " << m_selected[k].name
                                     << " has no target plate and every plate is locked";
        }
    }

    //Overflow beds are numbered after every existing one. They become new plates in
    //finalize, and each carries the context of the plate it overflowed from - an object
    //must not change machine by failing to fit.
    m_overflow_bed_base = unlocked_count;
    m_overflow_plate_contexts.clear();

    //progress spans all the sub-arranges as if they were one
    size_t done = 0;
    auto make_progress = [this, &ctl](size_t offset) {
        return [this, &ctl, offset](unsigned num_finished, std::string str = "") {
            ctl.update_status((int)(offset + num_finished) * 100 / status_range(), _u8L("Arranging") + str);
        };
    };
    auto is_region_name = [](const std::string& name) {
        //plate-shape-derived virtual objects; rebuilt per plate from its own bed
        return name.rfind("ExcludedRegion", 0) == 0 || name.rfind("WrappingRegion", 0) == 0;
    };

    //one arrange per plate, on that plate's own bed
    for (const std::pair<const int, std::vector<size_t>> &group : sticky_groups) {
        const int g  = group.first;
        auto      it = sticky_groups.find(g);
        if (it->second.empty())
            continue;
        if (ctl.was_canceled())
            return;

        const int plate_idx = logical_to_plate[g];
        PartPlate* plate = ppl.get_plate(plate_idx);
        const ResolvedPlateSlicingConfig resolved = resolve_arrange_plate(plate);
        const DynamicPrintConfig &plate_config = resolved.config;
        const bool plate_enable_wrapping = plate_config.opt_bool("enable_wrapping_detection");

        //this plate's own outline, shrunk the same way the project bed is
        Points bed_g;
        const Pointfs& local_shape = plate->get_local_shape();
        if (local_shape.empty())
            throw RuntimeError((boost::format("Plate %1% is assigned to printer '%2%' but has no resolved bed geometry")
                                % (plate_idx + 1) % plate->get_printer_preset_name()).str());
        for (const Vec2d& p : local_shape)
            bed_g.emplace_back(scaled(p.x()), scaled(p.y()));

        //fixed items living on this bed keep their geometry; the plate-shape-derived
        //regions are rebuilt from this plate's own bed
        ArrangePolygons unsel_g;
        for (const ArrangePolygon& ap : m_unselected) {
            if (ap.bed_idx != g || is_region_name(ap.name))
                continue;
            unsel_g.emplace_back(ap);
            unsel_g.back().bed_idx = 0;
        }
        ppl.preprocess_exclude_areas(unsel_g, plate_enable_wrapping, 1, 0, plate_idx);
        if (resolved.is_bbl_printer && params.avoid_extrusion_cali_region && plate_config.opt_bool("scan_first_layer"))
            ppl.preprocess_nonprefered_areas(unsel_g, 1);

        arrangement::ArrangeParams params_g = params;
        params_g.clearance_height_to_rod = plate_config.opt_float("extruder_clearance_height_to_rod");
        params_g.clearance_height_to_lid = plate_config.opt_float("extruder_clearance_height_to_lid");
        params_g.clearance_radius        = plate_config.opt_float("extruder_clearance_radius");
        params_g.printable_height        = (float)plate_config.opt_float("printable_height");
        params_g.nozzle_height           = plate_config.opt_float("nozzle_height");
        params_g.align_center            = plate_config.option<ConfigOptionPoint>("best_object_pos")->value;
        params_g.is_seq_print            = plate->get_real_print_seq() == PrintSequence::ByObject;
        params_g.bed_shrink_x            = params_g.is_seq_print ? BED_SHRINK_SEQ_PRINT : 0;
        params_g.bed_shrink_y            = params_g.is_seq_print ? BED_SHRINK_SEQ_PRINT : 0;
        params_g.excluded_regions.clear();
        ppl.preprocess_exclude_areas(params_g.excluded_regions, plate_enable_wrapping, 1, scale_(1), plate_idx);
        params_g.progressind = make_progress(done);

        ArrangePolygons sel_g;
        sel_g.reserve(it->second.size());
        for (size_t k : it->second) {
            ArrangePolygon& ap = m_selected[k];
            if (ap.height > params_g.printable_height) {
                //taller than this plate's machine; there is no point asking arrange
                ap.bed_idx = arrangement::UNARRANGED;
                BOOST_LOG_TRIVIAL(warning) << "arrange: " << ap.name << " is taller than the printer assigned to plate "
                                           << (plate_idx + 1) << ", sending it to the unprintable area";
                continue;
            }
            sel_g.emplace_back(ap);
        }

        if (!sel_g.empty()) {
            update_arrange_params(params_g, &plate_config, sel_g);
            update_selected_items_inflation(sel_g, &plate_config, params_g);
            update_unselected_items_inflation(unsel_g, &plate_config, params_g);
            update_selected_items_axis_align(sel_g, &plate_config, params_g);
            bed_g = arrangement::get_shrink_bedpts(std::move(bed_g), params_g);
            BOOST_LOG_TRIVIAL(info) << boost::format("arrange: plate %1% ('%2%'): %3% items on its own bed")
                % (plate_idx + 1) % plate->get_printer_preset_name() % sel_g.size();
            arrangement::arrange(sel_g, unsel_g, bed_g, params_g);

            //Bed 0 is this plate. Bed k>0 is arrange's k-th extra bed OF THE SAME SHAPE,
            //which is the honest home for an overflow: it is the same machine by
            //construction. Those become new plates in finalize, each carrying this
            //plate's context, so nothing changes machine by not fitting. A negative bed
            //could not be placed at all and goes to the unprintable area.
            std::map<int, size_t> by_itemid;
            for (size_t k : it->second)
                by_itemid[m_selected[k].itemid] = k;
            //one overflow plate per extra bed this plate needed, allocated on first use
            std::map<int, int> overflow_bed_of;
            for (ArrangePolygon& res : sel_g) {
                auto slot = by_itemid.find(res.itemid);   //arrange may reorder
                if (slot == by_itemid.end())
                    continue;
                if (res.bed_idx == 0) {
                    res.bed_idx = g;
                }
                else if (res.bed_idx > 0 && !only_on_partplate) {
                    auto known = overflow_bed_of.find(res.bed_idx);
                    if (known == overflow_bed_of.end()) {
                        const int bed = m_overflow_bed_base + (int) m_overflow_plate_contexts.size();
                        m_overflow_plate_contexts.push_back(plate->get_slicing_context());
                        known = overflow_bed_of.emplace(res.bed_idx, bed).first;
                        BOOST_LOG_TRIVIAL(info) << boost::format("arrange: plate %1% overflows onto a new plate on the same machine ('%2%')")
                            % (plate_idx + 1) % plate->get_printer_preset_name();
                    }
                    res.bed_idx = known->second;
                }
                else if (res.bed_idx > 0) {
                    //Arranging ONE plate, from its own context menu. finalize routes this
                    //through postprocess_bed_index_for_current_plate, which maps every bed
                    //past the first onto a single index and creates no plate - so an overflow
                    //bed here is not a new plate on the same machine, it is a phantom. The
                    //user asked to arrange this plate, not to acquire another one, so what
                    //does not fit goes to the unprintable area where they can see it.
                    BOOST_LOG_TRIVIAL(warning) << "arrange: " << res.name << " does not fit plate "
                                               << (plate_idx + 1) << ", sending it to the unprintable area";
                    res.bed_idx = arrangement::UNARRANGED;
                }
                else {
                    BOOST_LOG_TRIVIAL(warning) << "arrange: " << res.name << " could not be placed on plate "
                                               << (plate_idx + 1) << ", sending it to the unprintable area";
                    res.bed_idx = arrangement::UNARRANGED;
                }
                m_selected[slot->second] = std::move(res);
            }
        }
        done += it->second.size();
    }

}

ArrangeJob::ArrangeJob() : m_plater{wxGetApp().plater()} { }

static std::string concat_strings(const std::set<std::string> &strings,
                                  const std::string &delim = "\n")
{
    return std::accumulate(
        strings.begin(), strings.end(), std::string(""),
        [delim](const std::string &s, const std::string &name) {
            return s + name + delim;
        });
}

void ArrangeJob::finalize(bool canceled, std::exception_ptr &eptr) {
    try {
        if (eptr)
            std::rethrow_exception(eptr);
    } catch (libnest2d::GeometryException &) {
        show_error(m_plater, _(L("Arrange failed. "
                                 "Found some exceptions when processing object geometries.")));
        eptr = nullptr;
    } catch (...) {
        eptr = std::current_exception();
    }

    if (canceled || eptr)
        return;

    // Unprintable items go to the last virtual bed
    int beds = 0;

    //BBS: partplate
    PartPlateList& plate_list = m_plater->get_partplate_list();
    //clear all the relations before apply the arrangement results
    plate_list.remember_material_contexts();
    if (only_on_partplate) {
        plate_list.clear(false, false, true, current_plate_index);
    }
    else
        plate_list.clear(false, false, true, -1);
    //Which plate each overflow bed became. postprocess rewrites bed_idx into a real plate
    //index and creates the plate on the way, so the mapping only exists across that one
    //call - it is captured here rather than recomputed, because recomputing it would mean
    //a second implementation of postprocess's locked-plate arithmetic.
    std::map<int, int> overflow_plate_of_bed;

    //BBS: adjust the bed_index, create new plates, get the max bed_index
    for (ArrangePolygon& ap : m_selected) {
        //if (ap.bed_idx < 0) continue;  // bed_idx<0 means unarrangable
        const int bed_before = ap.bed_idx;
        //BBS: partplate postprocess
        if (only_on_partplate)
            plate_list.postprocess_bed_index_for_current_plate(ap);
        else
            plate_list.postprocess_bed_index_for_selected(ap);

        if (bed_before >= m_overflow_bed_base && ap.bed_idx >= 0 &&
            bed_before - m_overflow_bed_base < (int) m_overflow_plate_contexts.size())
            overflow_plate_of_bed[bed_before] = ap.bed_idx;

        beds = std::max(ap.bed_idx, beds);

        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": arrange selected %4%: bed_id %1%, trans {%2%,%3%}") % ap.bed_idx % unscale<double>(ap.translation(X)) % unscale<double>(ap.translation(Y)) % ap.name;
    }

    //BBS: adjust the bed_index, create new plates, get the max bed_index
    for (ArrangePolygon& ap : m_unselected)
    {
        if (ap.is_virt_object)
            continue;

        //BBS: partplate postprocess
        if (!only_on_partplate)
            plate_list.postprocess_bed_index_for_unselected(ap);

        beds = std::max(ap.bed_idx, beds);
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":arrange unselected %4%: bed_id %1%, trans {%2%,%3%}") % ap.bed_idx % unscale<double>(ap.translation(X)) % unscale<double>(ap.translation(Y)) % ap.name;
    }

    //An overflow plate carries the machine of the plate it overflowed from. create_plate
    //seeds nothing - a plate is seeded by whoever asked for it - so without this the plate
    //postprocess just created would name no printer at all, and an object that merely did
    //not fit would land on a plate that cannot be drawn or sliced.
    for (const std::pair<const int, int> &mapped : overflow_plate_of_bed) {
        PartPlate *created = plate_list.get_plate(mapped.second);
        if (created == nullptr)
            continue;
        const PlateSlicingContext &context = m_overflow_plate_contexts[mapped.first - m_overflow_bed_base];
        if (created->get_slicing_context() == context)
            continue;
        created->set_slicing_context(context);
        plate_list.apply_printer_to_plate(mapped.second, false);
        BOOST_LOG_TRIVIAL(info) << boost::format("arrange: plate %1% was created for overflow and takes printer '%2%'")
            % (mapped.second + 1) % context.printer_preset_name;
    }
    if (!overflow_plate_of_bed.empty())
        plate_list.reflow_layout();

    for (ArrangePolygon& ap : m_locked) {
        beds = std::max(ap.bed_idx, beds);

        plate_list.postprocess_arrange_polygon(ap, false);

        ap.apply();
    }

    // Apply the arrange result to all selected objects
    for (ArrangePolygon& ap : m_selected) {
        //BBS: partplate postprocess
        plate_list.postprocess_arrange_polygon(ap, true);

        ap.apply();
    }

    // Apply the arrange result to unselected objects(due to the sukodu-style column changes, the position of unselected may also be modified)
    for (ArrangePolygon& ap : m_unselected)
    {
        if (ap.is_virt_object)
            continue;

        //BBS: partplate postprocess
        plate_list.postprocess_arrange_polygon(ap, false);

        ap.apply();
    }

    // Move the unprintable items to the last virtual bed.
    // Note ap.apply() moves relatively according to bed_idx, so we need to subtract the orignal bed_idx
    for (ArrangePolygon& ap : m_unprintable) {
        ap.bed_idx = beds + 1;
        plate_list.postprocess_arrange_polygon(ap, true);

        ap.apply();
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":arrange m_unprintable: name: %4%, bed_id %1%, trans {%2%,%3%}") % ap.bed_idx % unscale<double>(ap.translation(X)) % unscale<double>(ap.translation(Y)) % ap.name;
    }

    m_plater->update();
    // BBS
    //wxGetApp().obj_manipul()->set_dirty();

    if (!m_unarranged.empty()) {
        std::set<std::string> names;
        for (ModelInstance *mi : m_unarranged)
            names.insert(mi->get_object()->name);

        m_plater->get_notification_manager()->push_notification(GUI::format(
            _L("Arrangement ignored the following objects which can't fit into a single bed:\n%s"),
            concat_strings(names, "\n")));
    }
    m_plater->get_notification_manager()->close_notification_of_type(NotificationType::ArrangeOngoing);

    // Resolve transfers before empty source plates are recycled, so a refused
    // transfer can return its instance to the original bed.
    if (plate_list.apply_pending_material_transfers())
        m_plater->set_plater_dirty(true);

    //BBS: reload all objects due to arrange
    if (only_on_partplate) {
        plate_list.rebuild_plates_after_arrangement(!only_on_partplate, true, current_plate_index);
    }
    else {
        plate_list.rebuild_plates_after_arrangement(!only_on_partplate, true);
    }

    // unlock the plates we just locked
    for (int i : m_uncompatible_plates)
        plate_list.get_plate(i)->lock(false);

    // BBS: update slice context and gcode result.
    m_plater->update_slicing_context_to_current_partplate();

    wxGetApp().obj_list()->reload_all_plates();

    m_plater->update();

    m_plater->m_arrange_running.store(false);
}

std::optional<arrangement::ArrangePolygon>
get_wipe_tower_arrangepoly(const Plater &plater)
{
    int id = plater.canvas3D()->fff_print()->get_plate_index();
    if (auto wti = get_wipe_tower(plater, id))
        return get_wipetower_arrange_poly(&wti);

    return {};
}

//NOTE: bed_stride_x()/bed_stride_y() lived here. A single stride between logical
//beds is meaningless once plates carry their own printers; positions come from
//PartPlateList::get_plate_origin_2d()/predict_plate_origin() instead.

// call before get selected and unselected
arrangement::ArrangeParams init_arrange_params(Plater *p)
{
    arrangement::ArrangeParams         params;
    GLCanvas3D::ArrangeSettings       &settings     = p->canvas3D()->get_arrange_settings();
    auto                              &print        = wxGetApp().plater()->get_partplate_list().get_current_fff_print();
    const PrintConfig                 &print_config = print.config();

    auto [object_skirt_offset, object_skirt_witdh] = print.object_skirt_offset();

    params.clearance_height_to_rod             = print_config.extruder_clearance_height_to_rod.value;
    params.clearance_height_to_lid             = print_config.extruder_clearance_height_to_lid.value;
    params.clearance_radius                    = print_config.extruder_clearance_radius.value + object_skirt_offset * 2;
    params.object_skirt_offset                 = object_skirt_offset;
    params.printable_height                    = print_config.printable_height.value;
    params.allow_rotations                     = settings.enable_rotation;
    params.nozzle_height                       = print_config.nozzle_height.value;
    params.align_center                        = print_config.best_object_pos.value;
    params.allow_multi_materials_on_same_plate = settings.allow_multi_materials_on_same_plate;
    params.avoid_extrusion_cali_region         = settings.avoid_extrusion_cali_region;
    params.is_seq_print                        = settings.is_seq_print;
    params.min_obj_distance                    = scaled(settings.distance);
    params.align_to_y_axis                     = settings.align_to_y_axis;

    int state = p->get_prepare_state();
    if (state == Job::JobPrepareState::PREPARE_STATE_MENU) {
        PartPlateList &plate_list = p->get_partplate_list();
        PartPlate *    plate      = plate_list.get_curr_plate();
        bool plate_same_as_global = true;
        params.is_seq_print       = plate->get_real_print_seq(&plate_same_as_global) == PrintSequence::ByObject;
        // if plate's print sequence is not the same as global, the settings.distance is no longer valid, we set it to auto
        if (!plate_same_as_global)
            params.min_obj_distance = 0;
    }

    if (params.is_seq_print) {
        params.bed_shrink_x = BED_SHRINK_SEQ_PRINT;
        params.bed_shrink_y = BED_SHRINK_SEQ_PRINT;
    }
    return params;
}

//Per-plate machines: see the header for the contract. All of the packing below happens
//in the destination plate's own local frame, because that is the frame its bed outline
//and its exclusion areas are expressed in. Instance positions are world, so they are
//converted on the way in and converted back on the way out.
PlacementResult place_instances_on_plate(Plater *plater, int plate_idx,
                                         const std::vector<std::pair<int, int>> &instances)
{
    PlacementResult result;
    if (plater == nullptr || instances.empty())
        return result;

    PartPlateList &ppl   = plater->get_partplate_list();
    PartPlate     *plate = ppl.get_plate(plate_idx);
    if (plate == nullptr) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": there is no plate %1% to place onto") % plate_idx;
        return result;
    }

    Model &model = plater->model();
    auto instance_of = [&model](const std::pair<int, int> &id) -> ModelInstance * {
        if (id.first < 0 || id.first >= (int) model.objects.size())
            return nullptr;
        ModelObject *mo = model.objects[id.first];
        if (id.second < 0 || id.second >= (int) mo->instances.size())
            return nullptr;
        return mo->instances[id.second];
    };

    //Everything this plate's machine says about packing. Resolving the plate rather
    //than the project is the whole point: plate 3 may be a different machine, with a
    //different bed and different exclusion areas, from the plate the copy came from.
    //
    //An unresolved plate is not a refusal here. This runs under Ctrl+V, after
    //Selection::paste_objects_from_clipboard has already called Model::add_object for
    //every clipboard object and before the object list learns about them, so throwing
    //out of it terminated the application AND left the Model holding objects nothing
    //owned. The plate state that produces it is one the fork supports deliberately: the
    //picker offers "Keep <name> (not installed)". Unresolved therefore packs nothing and
    //every copy goes to the row in front of the plate, which is what the tail already
    //does for anything that will not fit, with the reason named.
    ResolvedPlateSlicingConfig resolved;
    std::string                resolve_error;
    const bool                 plate_resolved = try_resolve_arrange_plate(plate, resolved, resolve_error);
    const DynamicPrintConfig & plate_config   = resolved.config;
    const Vec2d                plate_origin   = ppl.get_plate_origin_2d(plate_idx);

    arrangement::ArrangePolygons selected;
    std::vector<size_t>          selected_source;
    std::vector<size_t>          leftovers;

    if (!plate_resolved) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": " << resolve_error
                                   << "; placing the copies in front of the plate instead of packing them";
        for (size_t k = 0; k < instances.size(); ++k)
            leftovers.push_back(k);
    } else {
        arrangement::ArrangeParams params = init_arrange_params(plater);
        params.clearance_height_to_rod = plate_config.opt_float("extruder_clearance_height_to_rod");
        params.clearance_height_to_lid = plate_config.opt_float("extruder_clearance_height_to_lid");
        params.clearance_radius        = plate_config.opt_float("extruder_clearance_radius");
        params.printable_height        = (float) plate->get_printable_height();
        params.nozzle_height           = plate_config.opt_float("nozzle_height");
        params.is_seq_print            = plate->get_real_print_seq() == PrintSequence::ByObject;
        params.bed_shrink_x            = params.is_seq_print ? BED_SHRINK_SEQ_PRINT : 0;
        params.bed_shrink_y            = params.is_seq_print ? BED_SHRINK_SEQ_PRINT : 0;
        //Spacing is the user's arrange spacing. init_arrange_params() zeroes it when the
        //current plate prints in a different sequence from the project, which is a rule
        //about the plate being arranged, not about this one.
        params.min_obj_distance = scaled(plater->canvas3D()->get_arrange_settings().distance);
        //A copy keeps the orientation it was copied with.
        params.allow_rotations = false;
        //Both of these move the whole packed pile once packing is done, and that pile
        //includes the items already on the plate. Those items' positions do survive (only
        //the new ones are read back) but the new ones would land shifted off them and
        //overlap. So alignment is off by default, and best_object_pos is neutralised so
        //that AutoArranger does not turn it back on as a user-defined alignment. It is
        //switched back on below in the one case where there is no pile to stay relative to.
        params.do_final_align = false;
        params.align_center   = Vec2d(0.5, 0.5);
        params.progressind    = [](unsigned, std::string) {};
        params.excluded_regions.clear();
        params.nonprefered_regions.clear();

        const bool enable_wrapping = plate_config.opt_bool("enable_wrapping_detection");
        ppl.preprocess_exclude_areas(params.excluded_regions, enable_wrapping, 1, scale_(1), plate_idx);

        //Fixed items: everything already standing on this plate, plus its wipe tower.
        //These are preloaded into the bin and never moved.
        arrangement::ArrangePolygons fixed;
        std::set<std::pair<int, int>> being_placed(instances.begin(), instances.end());
        for (size_t oidx = 0; oidx < model.objects.size(); ++oidx) {
            ModelObject *mo = model.objects[oidx];
            for (size_t iidx = 0; iidx < mo->instances.size(); ++iidx) {
                if (being_placed.count({(int) oidx, (int) iidx}) > 0)
                    continue;
                if (!plate->contain_instance((int) oidx, (int) iidx) && !plate->intersect_instance((int) oidx, (int) iidx))
                    continue;
                arrangement::ArrangePolygon ap = get_instance_arrange_poly(mo->instances[iidx], plate_config);
                if (ap.poly.contour.size() < 3)
                    continue;
                ap.name    = mo->name;
                ap.bed_idx = 0;
                ap.setter  = nullptr;
                ap.translation(X) -= scaled<double>(plate_origin.x());
                ap.translation(Y) -= scaled<double>(plate_origin.y());
                fixed.emplace_back(std::move(ap));
            }
        }
        //The wipe tower rectangle is rebuilt here from its plate-local position and its
        //footprint rather than taken from WipeTower::get_arrange_polygon(), which mixes a
        //world-space outline with a plate-local translation and so only lands correctly
        //on plate 0.
        const GLCanvas3D::WipeTowerInfo wti = plater->canvas3D()->get_wipe_tower_info(plate_idx);
        if (wti) {
            const Vec2d wt_pos  = wti.pos();
            const Vec2d wt_size = wti.bb_size();
            arrangement::ArrangePolygon ap;
            ap.poly.contour = Polygon({{scaled(wt_pos.x()), scaled(wt_pos.y())},
                                       {scaled(wt_pos.x() + wt_size.x()), scaled(wt_pos.y())},
                                       {scaled(wt_pos.x() + wt_size.x()), scaled(wt_pos.y() + wt_size.y())},
                                       {scaled(wt_pos.x()), scaled(wt_pos.y() + wt_size.y())}});
            ap.bed_idx        = 0;
            ap.is_virt_object = true;
            ap.is_wipe_tower  = true;
            ap.height         = 1;
            ap.name           = "WipeTower";
            fixed.emplace_back(std::move(ap));
        }

        //An empty plate has no pile for the copies to stay relative to, so they may be
        //centred exactly the way an arrange of an empty plate centres things. The moment
        //there is something already standing there, that centring would drag the copies
        //off the objects they were packed around, and it stays off.
        if (fixed.empty()) {
            params.do_final_align = true;
            if (const ConfigOptionPoint *best_pos = plate_config.option<ConfigOptionPoint>("best_object_pos"))
                params.align_center = best_pos->value;
        }

        //Items to place. A degenerate hull is dropped by Arrange's process_arrangeable(),
        //which would slide every later result onto the wrong item, so it never enters the
        //list; it is reported as unplaced instead.
        for (size_t k = 0; k < instances.size(); ++k) {
            ModelInstance *mi = instance_of(instances[k]);
            if (mi == nullptr)
                continue;
            arrangement::ArrangePolygon ap = get_instance_arrange_poly(mi, plate_config);
            if (ap.poly.contour.size() < 3 || ap.poly.area() < 0.001) {
                leftovers.push_back(k);
                continue;
            }
            ap.name    = model.objects[instances[k].first]->name;
            ap.bed_idx = 0;
            ap.itemid  = (int) selected.size();
            //the result is applied here, in world coordinates, not through the setter
            ap.setter = nullptr;
            ap.translation(X) -= scaled<double>(plate_origin.x());
            ap.translation(Y) -= scaled<double>(plate_origin.y());
            selected.emplace_back(std::move(ap));
            selected_source.push_back(k);
        }

        //This plate's own bed outline. An empty one means the plate was never shaped,
        //which is a defect to repair at its source; it does not license packing against
        //the project bed, so everything falls through to the row below instead.
        Points bedpts;
        for (const Vec2d &p : plate->get_local_shape())
            bedpts.emplace_back(scaled(p.x()), scaled(p.y()));

        if (bedpts.size() >= 3 && !selected.empty()) {
            update_arrange_params(params, &plate_config, selected);
            update_selected_items_inflation(selected, &plate_config, params);
            update_unselected_items_inflation(fixed, &plate_config, params);
            bedpts = arrangement::get_shrink_bedpts(std::move(bedpts), params);

            BOOST_LOG_TRIVIAL(info) << boost::format("place: %1% new item(s) onto plate %2% ('%3%'), around %4% fixed item(s)")
                                           % selected.size() % (plate_idx + 1)
                                           % plate->get_printer_preset_name()
                                           % fixed.size();
            arrangement::arrange(selected, fixed, bedpts, params);
            result.arranged = true;

            for (size_t i = 0; i < selected.size(); ++i) {
                ModelInstance *mi = instance_of(instances[selected_source[i]]);
                if (mi == nullptr)
                    continue;
                //bed 0 is this plate. Anything else means the nester needed another bed,
                //i.e. it did not fit here.
                if (selected[i].bed_idx != 0) {
                    leftovers.push_back(selected_source[i]);
                    continue;
                }
                Vec2d offs = selected[i].translation.cast<double>();
                offs.x() += scaled<double>(plate_origin.x());
                offs.y() += scaled<double>(plate_origin.y());
                mi->apply_arrange_result(offs, selected[i].rotation);
                ++result.placed;
            }
        }
        else {
            for (size_t i : selected_source)
                leftovers.push_back(i);
        }
    }

    if (leftovers.empty())
        return result;

    //Nothing fitted, or some of it did not. Lay those out in a row immediately in
    //front of the plate: visible, not stacked on each other, and one drag from where
    //the user wants them. Leaving them hidden under what is already there, or not
    //pasting at all, would both be worse.
    std::sort(leftovers.begin(), leftovers.end());
    leftovers.erase(std::unique(leftovers.begin(), leftovers.end()), leftovers.end());

    const BoundingBoxf3 plate_box = plate->get_build_volume();
    const coord_t       gap       = scaled(10.);
    coord_t             cursor_x  = scaled(plate_box.min.x());
    const coord_t       row_top   = scaled(plate_box.min.y()) - gap;
    std::set<std::string> leftover_names;

    for (size_t k : leftovers) {
        ModelInstance *mi = instance_of(instances[k]);
        if (mi == nullptr)
            continue;
        ++result.unplaced;
        leftover_names.insert(model.objects[instances[k].first]->name);

        //The footprint, and nothing else. This row is geometry: where a shape can be
        //laid down so it is visible and not on top of its neighbour. It deliberately
        //does not go through get_instance_arrange_poly(), which reads temperatures,
        //support settings and brim widths out of a print config and dereferences those
        //options unconditionally - so on an unresolved plate, which is exactly when this
        //branch takes every copy, it would crash rather than answer.
        arrangement::ArrangePolygon ap;
        mi->get_arrange_polygon(&ap);
        if (ap.poly.contour.size() < 3)
            continue;   //no footprint to lay out; it keeps the position it was created at
        Polygon hull = ap.poly.contour;
        hull.rotate(ap.rotation);
        const BoundingBox hull_bb = hull.bounding_box();
        //the contour carries no X/Y offset, so the instance offset that puts the
        //footprint at a chosen corner is that corner minus the footprint's own corner.
        const Vec2d offs((double) (cursor_x - hull_bb.min.x()), (double) (row_top - hull_bb.max.y()));
        mi->apply_arrange_result(offs, ap.rotation);
        cursor_x += hull_bb.size().x() + gap;
    }

    if (result.unplaced > 0) {
        std::string names;
        for (const std::string &name : leftover_names)
            names += (names.empty() ? "" : ", ") + name;
        //Two different reasons reach this row, and saying "no room" for the second one
        //would send the user looking for space they already have. An unresolved plate
        //names the plate and what is wrong with it, because that is the thing to fix.
        //std::string, not wxString: GUI::format returns std::string and
        //NotificationManager::push_notification takes one.
        const std::string message =
            plate_resolved ?
                GUI::format(_L("There was no room left on plate %1% for: %2%\n"
                               "They were placed in front of the plate instead, so you can move them where you want them."),
                            plate_idx + 1, names) :
                GUI::format(_L("Plate %1% could not be used to place: %2%\n%3%\n"
                               "They were placed in front of the plate instead, so you can move them where you want them."),
                            plate_idx + 1, names, resolve_error);
        plater->get_notification_manager()->push_notification(
            NotificationType::BBLPlateInfo, NotificationManager::NotificationLevel::WarningNotificationLevel,
            message);
    }

    return result;
}

}} // namespace Slic3r::GUI
