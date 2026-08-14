#ifndef ARRANGEJOB_HPP
#define ARRANGEJOB_HPP


#include <optional>
#include <utility>
#include <vector>

#include "Job.hpp"
#include "libslic3r/Arrange.hpp"
#include "libslic3r/PlateSlicingContext.hpp"

namespace Slic3r {

class ModelInstance;

namespace GUI {

class Plater;

class ArrangeJob : public Job
{
    using ArrangePolygon = arrangement::ArrangePolygon;
    using ArrangePolygons = arrangement::ArrangePolygons;

    //BBS: add locked logic
    ArrangePolygons m_selected, m_unselected, m_unprintable, m_locked;
    std::vector<ModelInstance*> m_unarranged;
    std::map<int, ArrangePolygons> m_selected_groups;   // groups of selected items for sequential printing
    std::vector<int> m_uncompatible_plates;  // plate indices with different printing sequence than global

    arrangement::ArrangeParams params;
    int current_plate_index = 0;
    Polygon bed_poly;
    Plater *m_plater;

    // BBS: add flag for whether on current part plate
    bool only_on_partplate{false};

    // clear m_selected and m_unselected, reserve space for next usage
    void clear_input();

    // Prepare the selected and unselected items separately. If nothing is
    // selected, behaves as if everything would be selected.
    void prepare_selected();

    void prepare_all();

    //BBS:prepare the items from current selected partplate
    void prepare_partplate();
    void prepare_wipe_tower();

    //Per-plate machines: every plate owns a printer and therefore a bed, so one uniform
    //bed never describes the world and there is no shared pool of unassigned plates left
    //to fall back to. Each plate is arranged on its own, against its own bed, and its
    //items never migrate to another plate - moving one would change which machine prints
    //it. What does not fit goes to an overflow bed of the SAME shape, which finalize turns
    //into a plate carrying this plate's context.
    void arrange_per_plate(Ctl& ctl);

    //Overflow beds, numbered after every existing plate. An item that does not fit its
    //plate lands on one of arrange's extra beds of the SAME shape, which finalize turns
    //into a real plate. Each entry is the context that plate must be given, so an object
    //never changes machine by failing to fit. Index is (bed - m_overflow_bed_base).
    int                              m_overflow_bed_base = 0;
    std::vector<PlateSlicingContext> m_overflow_plate_contexts;

    ArrangePolygon prepare_arrange_polygon(int object_idx, int instance_idx);

protected:

    void check_unprintable();

public:

    void prepare();

    void process(Ctl &ctl) override;

    ArrangeJob();

    int status_range() const
    {
        // ensure finalize() is called after all operations in process() is finished.
        return int(m_selected.size() + m_unprintable.size() + 1);
    }

    void finalize(bool canceled, std::exception_ptr &e) override;
};

std::optional<arrangement::ArrangePolygon> get_wipe_tower_arrangepoly(const Plater &);

// The gap between logical beds in the x axis expressed in ratio of
// the current bed width.
static const constexpr double LOGICAL_BED_GAP = 1. / 5.;

//NOTE: bed_stride_x()/bed_stride_y() lived here; removed with per-plate machines.
//Use PartPlateList::get_plate_origin_2d()/predict_plate_origin() for positions.

arrangement::ArrangeParams init_arrange_params(Plater *p);

//What place_instances_on_plate() managed to do. `arranged` is false when the plate
//supplied no bed to pack against at all, which is a defect in the plate rather than
//a reason to refuse the operation that created the instances.
struct PlacementResult
{
    int  placed{0};
    int  unplaced{0};
    bool arranged{false};
};

//Per-plate machines: place the given instances into the free space of ONE plate,
//against that plate's own bed and that plate's own exclusion areas. Instances are
//named by (object index, instance index) into plater->model().
//
//Nothing already on the plate is moved: the user placed those deliberately, and
//shoving them aside is a larger harm than the stacking this exists to end. Anything
//that will not fit is laid out in a readable row directly in front of the plate and
//named in a notification, because a paste that refuses, or that drops the copy where
//it cannot be seen, is worse than an honest placement plus a message.
PlacementResult place_instances_on_plate(Plater *plater, int plate_idx,
                                         const std::vector<std::pair<int, int>> &instances);

}} // namespace Slic3r::GUI

#endif // ARRANGEJOB_HPP
