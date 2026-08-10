#include "FillBedJob.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "libnest2d/common.hpp"

#include <numeric>

namespace Slic3r {
namespace GUI {

static ResolvedPlateSlicingConfig resolve_fill_plate(PartPlate *plate)
{
    if (plate == nullptr)
        throw RuntimeError("Fill bed has no target plate");
    PresetBundle &bundle = *wxGetApp().preset_bundle;
    const std::vector<int> filament_maps = plate->get_real_filament_maps(bundle.project_config);
    const std::vector<int> volume_maps   = plate->get_real_filament_volume_maps(bundle.project_config);
    ResolvedPlateSlicingConfig resolved;
    std::string error;
    if (!bundle.resolve_plate_slicing_config(plate->get_slicing_context(), filament_maps, volume_maps,
                                             resolved, error)) {
        plate->update_apply_result_invalid(true);
        throw RuntimeError((boost::format("Plate %1% has an unresolved slicing context: %2%")
                            % (plate->get_index() + 1) % error).str());
    }
    resolved.config.apply(*plate->config(), true);
    return resolved;
}

//BBS: add partplate related logic
void FillBedJob::prepare()
{
    PartPlateList& plate_list = m_plater->get_partplate_list();

    m_locked.clear();
    m_selected.clear();
    m_unselected.clear();
    m_bedpts.clear();

    params = init_arrange_params(m_plater);

    m_object_idx = m_plater->get_selected_object_idx();
    if (m_object_idx == -1)
        return;

    //select current plate at first
    int sel_id = m_plater->get_selection().get_instance_idx();
    sel_id = std::max(sel_id, 0);

    int sel_ret = plate_list.select_plate_by_obj(m_object_idx, sel_id);
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":select plate obj_id %1%, ins_id %2%, ret %3%}") % m_object_idx % sel_id % sel_ret;

    PartPlate* plate = plate_list.get_curr_plate();
    const ResolvedPlateSlicingConfig resolved = resolve_fill_plate(plate);
    const DynamicPrintConfig &plate_config = resolved.config;
    Model& model = m_plater->model();
    BoundingBox plate_bb = plate->get_bounding_box_crd();
    int plate_cols = plate_list.get_plate_cols();
    int cur_plate_index = plate->get_index();
    //world -> plate-local now goes through the plate's actual origin instead of
    //row/col-times-stride, which was only ever right while every plate was the
    //same size (per-plate printer assignments broke that assumption)
    const Vec2d plate_origin = plate_list.get_plate_origin_2d(cur_plate_index);

    ModelObject *model_object = m_plater->model().objects[m_object_idx];
    if (model_object->instances.empty()) return;

    m_selected.reserve(model_object->instances.size());
    for (size_t oidx = 0; oidx < model.objects.size(); ++oidx)
    {
        ModelObject* mo = model.objects[oidx];
        for (size_t inst_idx = 0; inst_idx < mo->instances.size(); ++inst_idx)
        {
            bool selected = (oidx == m_object_idx);

            ArrangePolygon ap = get_instance_arrange_poly(mo->instances[inst_idx], plate_config);
            BoundingBox ap_bb = ap.transformed_poly().contour.bounding_box();
            ap.name = mo->name;

            if (selected)
            {
                if (mo->instances[inst_idx]->printable)
                {
                    ++ap.priority;
                    ap.itemid = m_selected.size();
                    m_selected.emplace_back(ap);
                }
                else
                {
                    if (plate_bb.contains(ap_bb))
                    {
                        ap.bed_idx = 0;
                        ap.itemid = m_unselected.size();
                        ap.row = cur_plate_index / plate_cols;
                        ap.col = cur_plate_index % plate_cols;
                        ap.translation(X) -= scaled<double>(plate_origin.x());
                        ap.translation(Y) -= scaled<double>(plate_origin.y());
                        m_unselected.emplace_back(ap);
                    }
                    else
                    {
                        ap.bed_idx = PartPlateList::MAX_PLATES_COUNT;
                        ap.itemid = m_locked.size();
                        m_locked.emplace_back(ap);
                    }
                }
            }
            else
            {
                if (plate_bb.contains(ap_bb))
                {
                    ap.bed_idx = 0;
                    ap.itemid = m_unselected.size();
                    ap.row = cur_plate_index / plate_cols;
                    ap.col = cur_plate_index % plate_cols;
                    ap.translation(X) -= scaled<double>(plate_origin.x());
                    ap.translation(Y) -= scaled<double>(plate_origin.y());
                    m_unselected.emplace_back(ap);
                }
                else
                {
                    ap.bed_idx = PartPlateList::MAX_PLATES_COUNT;
                    ap.itemid = m_locked.size();
                    m_locked.emplace_back(ap);
                }
            }
        }
    }
    /*
    for (ModelInstance *inst : model_object->instances)
        if (inst->printable) {
            ArrangePolygon ap = get_arrange_poly(inst);
            // Existing objects need to be included in the result. Only
            // the needed amount of object will be added, no more.
            ++ap.priority;
            m_selected.emplace_back(ap);
        }*/

    if (m_selected.empty()) return;

    bool enable_wrapping = plate_config.opt_bool("enable_wrapping_detection");
    //add the virtual object into unselect list if has
    //bed 0 here is the current plate, so its own exclude areas apply
    double scaled_exclusion_gap = scale_(1);
    plate_list.preprocess_exclude_areas(params.excluded_regions, enable_wrapping, 1, scaled_exclusion_gap, cur_plate_index);
    plate_list.preprocess_exclude_areas(m_unselected, enable_wrapping, 16, 0, cur_plate_index);

    m_bedpts = get_bed_shape(plate_config);

    auto &objects = m_plater->model().objects;
    /*BoundingBox bedbb = get_extents(m_bedpts);

    for (size_t idx = 0; idx < objects.size(); ++idx)
        if (int(idx) != m_object_idx)
            for (ModelInstance *mi : objects[idx]->instances) {
                ArrangePolygon ap = get_arrange_poly(mi);
                auto ap_bb = ap.transformed_poly().contour.bounding_box();

                if (ap.bed_idx == 0 && !bedbb.contains(ap_bb))
                    ap.bed_idx = arrangement::UNARRANGED;

                m_unselected.emplace_back(ap);
            }*/
    if (auto wt = get_wipe_tower_arrangepoly(*m_plater))
        m_unselected.emplace_back(std::move(*wt));

    double sc = scaled<double>(1.) * scaled(1.);

    auto polys = offset_ex(m_selected.front().poly, params.min_obj_distance / 2);
    ExPolygon poly = polys.empty() ? m_selected.front().poly : polys.front();
    double poly_area = poly.area() / sc;
    double unsel_area = std::accumulate(m_unselected.begin(),
                                        m_unselected.end(), 0.,
                                        [cur_plate_index](double s, const auto &ap) {
                                            //BBS: m_unselected instance is in the same partplate
                                            return s + (ap.bed_idx == cur_plate_index) * ap.poly.area();
                                            //return s + (ap.bed_idx == 0) * ap.poly.area();
                                        }) / sc;

    double fixed_area = unsel_area + m_selected.size() * poly_area;
    double bed_area   = Polygon{m_bedpts}.area() / sc;

    // This is the maximum number of items, the real number will always be close but less.
    int needed_items = (bed_area - fixed_area) / poly_area;

    //int sel_id = m_plater->get_selection().get_instance_idx();
    // if the selection is not a single instance, choose the first as template
    //sel_id = std::max(sel_id, 0);
    ModelInstance *mi = model_object->instances[sel_id];
    ArrangePolygon template_ap = get_instance_arrange_poly(mi, plate_config);

    //Both of these are read by the loop below whatever m_instances is, and only the
    //instance path assigned them, so the object path was adding two uninitialised
    //doubles on every iteration.
    double offset_base = 0.0, offset = 0.0;
    if (m_instances) {
        offset_base = m_plater->canvas3D()->get_size_proportional_to_max_bed_size(0.05);
        offset = offset_base;
    }

    for (int i = 0; i < needed_items; ++i, offset += offset_base) {
        ArrangePolygon ap = template_ap;
        ap.poly = m_selected.front().poly;
        ap.bed_idx = PartPlateList::MAX_PLATES_COUNT;
        ap.itemid = -1;
        ap.setter = [this, mi, offset](const ArrangePolygon &p) {
            ModelObject *mo = m_plater->model().objects[m_object_idx];
            ModelObject *obj;
            if (m_instances) {
                ModelInstance* model_instance = mo->instances.back();
                Vec3d offset_vec = model_instance->get_offset() + Vec3d(offset, offset, 0.0);
                mo->add_instance(offset_vec, model_instance->get_scaling_factor(), model_instance->get_rotation(), model_instance->get_mirror());
                obj = mo;
            } else {
                ModelObject* newObj = m_plater->model().add_object(*mo);
                newObj->name = mo->name +" "+ std::to_string(p.itemid);
                obj = newObj;
            }
            for (ModelInstance *newInst : obj->instances) { newInst->apply_arrange_result(p.translation.cast<double>(), p.rotation); }            
            //m_plater->sidebar().obj_list()->paste_objects_into_list({m_plater->model().objects.size()-1});
        };
        m_selected.emplace_back(ap);
    }

    m_status_range = m_selected.size();

    // The strides have to be removed from the fixed items. For the
    // arrangeable (selected) items bed_idx is ignored and the
    // translation is irrelevant.
    //BBS: remove logic for unselected object
    /*double stride = bed_stride(m_plater);
    for (auto &p : m_unselected)
        if (p.bed_idx > 0)
            p.translation(X) -= p.bed_idx * stride;*/
}

void FillBedJob::process(Ctl &ctl)
{
    auto statustxt = _u8L("Filling");
    ctl.call_on_main_thread([this] { prepare(); }).wait();
    ctl.update_status(0, statustxt);

    if (m_object_idx == -1 || m_selected.empty()) return;

    PartPlate* cur_plate = m_plater->get_partplate_list().get_curr_plate();
    const ResolvedPlateSlicingConfig resolved = resolve_fill_plate(cur_plate);
    const DynamicPrintConfig &plate_config = resolved.config;
    params.clearance_height_to_rod = plate_config.opt_float("extruder_clearance_height_to_rod");
    params.clearance_height_to_lid = plate_config.opt_float("extruder_clearance_height_to_lid");
    params.clearance_radius        = plate_config.opt_float("extruder_clearance_radius");
    params.printable_height        = plate_config.opt_float("printable_height");
    params.nozzle_height           = plate_config.opt_float("nozzle_height");
    params.align_center            = plate_config.option<ConfigOptionPoint>("best_object_pos")->value;
    params.is_seq_print            = cur_plate->get_real_print_seq() == PrintSequence::ByObject;
    params.bed_shrink_x            = params.is_seq_print ? BED_SHRINK_SEQ_PRINT : 0;
    params.bed_shrink_y            = params.is_seq_print ? BED_SHRINK_SEQ_PRINT : 0;
    update_arrange_params(params, &plate_config, m_selected);
    m_bedpts = get_shrink_bedpts(&plate_config, params);

    //filling happens on the current plate; when that plate is pinned to its own
    //printer, fill against that printer's bed and height, not the project's
    {
        if (cur_plate->has_printer_assignment()) {
            if (cur_plate->get_local_shape().empty())
                throw RuntimeError((boost::format("Plate %1% is assigned to printer '%2%' but has no resolved bed geometry")
                                    % (cur_plate->get_index() + 1) % cur_plate->get_printer_preset_name()).str());
            Points bed;
            for (const Vec2d& pt : cur_plate->get_local_shape())
                bed.emplace_back(scaled(pt.x()), scaled(pt.y()));
            m_bedpts = arrangement::get_shrink_bedpts(std::move(bed), params);
            params.printable_height = (float)cur_plate->get_printable_height();
        }
    }

    auto &partplate_list               = m_plater->get_partplate_list();
    if (resolved.is_bbl_printer && params.avoid_extrusion_cali_region && plate_config.opt_bool("scan_first_layer"))
        partplate_list.preprocess_nonprefered_areas(m_unselected, MAX_NUM_PLATES);

    update_selected_items_inflation(m_selected, &plate_config, params);
    update_unselected_items_inflation(m_unselected, &plate_config, params);

    bool do_stop = false;
    params.stopcondition = [&ctl, &do_stop]() {
        return ctl.was_canceled() || do_stop;
    };

    params.progressind = [this, &ctl, &statustxt](unsigned st,std::string str="") {
         if (st > 0)
             ctl.update_status(st * 100 / status_range(), statustxt + " " + str);
    };

    params.on_packed = [&do_stop] (const ArrangePolygon &ap) {
        do_stop = ap.bed_idx > 0 && ap.priority == 0;
    };
    // final align用的是凸包，在有fixed item的情况下可能找到的参考点位置是错的，这里就不做了。见STUDIO-3265
    params.do_final_align = !resolved.is_bbl_printer;

    if (m_selected.size() > 100){
        // too many items, just find grid empty cells to put them
        Vec2f step = unscaled<float>(get_extents(m_selected.front().poly).size()) + Vec2f(m_selected.front().brim_width, m_selected.front().brim_width);
        std::vector<Vec2f> empty_cells = Plater::get_empty_cells(step);
        size_t n=std::min(m_selected.size(), empty_cells.size());
        for (size_t i = 0; i < n; i++) {
            m_selected[i].translation = scaled<coord_t>(empty_cells[i]);
            m_selected[i].bed_idx= 0;
        }
        for (size_t i = n; i < m_selected.size(); i++) {
            m_selected[i].bed_idx = -1;
        }
    }
    else
        arrangement::arrange(m_selected, m_unselected, m_bedpts, params);

    // finalize just here.
    ctl.update_status(100, ctl.was_canceled() ?
                                       _u8L("Bed filling canceled.") :
                                       _u8L("Bed filling done."));
}

FillBedJob::FillBedJob(bool instances) : m_plater{wxGetApp().plater()}, m_instances{instances} {}

void FillBedJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    // Ignore the arrange result if aborted.
    if (canceled || eptr)
        return;

    if (m_object_idx == -1) return;

    ModelObject *model_object = m_plater->model().objects[m_object_idx];
    if (model_object->instances.empty()) return;

    //BBS: partplate
    PartPlateList& plate_list = m_plater->get_partplate_list();
    int plate_cols = plate_list.get_plate_cols();
    int cur_plate = plate_list.get_curr_plate_index();

    size_t inst_cnt = model_object->instances.size();

    int added_cnt = std::accumulate(m_selected.begin(), m_selected.end(), 0, [](int s, auto &ap) {
        return s + int(ap.priority == 0 && ap.bed_idx == 0);
    });

    int oldSize = m_plater->model().objects.size();

    if (added_cnt > 0) {
        //BBS: adjust the selected instances
        for (ArrangePolygon& ap : m_selected) {
            if (ap.bed_idx != 0) {
                BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":skipped: bed_id %1%, trans {%2%,%3%}") % ap.bed_idx % unscale<double>(ap.translation(X)) % unscale<double>(ap.translation(Y));
                /*if (ap.itemid == -1)*/
                    continue;
                ap.bed_idx = plate_list.get_plate_count();
            }
            else
                ap.bed_idx = cur_plate;

            if (m_selected.size() <= 100) {
                ap.row = ap.bed_idx / plate_cols;
                ap.col = ap.bed_idx % plate_cols;
                //plate-local -> world through the plate's actual (or predicted) origin,
                //not a uniform stride; plates can differ in size now
                const Vec2d plate_origin = plate_list.predict_plate_origin(ap.bed_idx);
                ap.translation(X) += scaled<double>(plate_origin.x());
                ap.translation(Y) += scaled<double>(plate_origin.y());
            }

            ap.apply();

            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":selected: bed_id %1%, trans {%2%,%3%}") % ap.bed_idx % unscale<double>(ap.translation(X)) % unscale<double>(ap.translation(Y));
        }

        int   newSize = m_plater->model().objects.size();
        auto obj_list = m_plater->sidebar().obj_list();
        for (size_t i = oldSize; i < newSize; i++) {
            obj_list->add_object_to_list(i, true, true, false);
            obj_list->update_printable_state(i, 0);
        }

        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ": paste_objects_into_list";

        /*for (ArrangePolygon& ap : m_selected) {
            if (ap.bed_idx != arrangement::UNARRANGED && (ap.priority != 0 || ap.bed_idx == 0))
                ap.apply();
        }*/

        //model_object->ensure_on_bed();
        //BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ": model_object->ensure_on_bed()";

        if (m_instances) {// && wxGetApp().app_config->get("auto_arrange") == "true") {
            m_plater->set_prepare_state(Job::PREPARE_STATE_MENU);
            m_plater->arrange();
        }
        m_plater->update();
    }

    m_plater->mark_plate_toolbar_image_dirty();
}

}} // namespace Slic3r::GUI
